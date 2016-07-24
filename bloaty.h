// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BLOATY_H_
#define BLOATY_H_

#include <stdlib.h>
#include <stdint.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "re2/re2.h"

#define BLOATY_DISALLOW_COPY_AND_ASSIGN(class_name) \
  class_name(const class_name&) = delete; \
  void operator=(const class_name&) = delete;

namespace bloaty {

class MemoryFileMap;
class MemoryMap;

// A VMFileRangeSink allows data sources like "segments" or "sections" to add
// ranges to our map that exist in *both* VM space and File space.
class VMFileRangeSink {
 public:
  VMFileRangeSink(MemoryFileMap* map) : map_(map) {}

  // If vmsize or filesize is zero, this mapping is presumed not to exist in
  // that domain.  For example, .bss mappings don't exist in the file, and
  // .debug_* mappings don't exist in memory.
  void AddRange(const std::string& name, uintptr_t vmaddr, size_t vmsize,
                long fileoff, long filesize);

 private:
  MemoryFileMap* map_;
};

// A VMRangeSink allows data sources like "symbols" to add ranges to our map.
// These ranges are in virtual memory (VM) space only.  If we want to know the
// effect on file size we must map these ranges into file space using mappings
// like "segments" or "sections" which know the VM -> File address tranlsation.
class VMRangeSink {
 public:
  VMRangeSink(MemoryMap* map, const MemoryFileMap* translator)
      : map_(map), translator_(translator) {}

  // Adds a region to the memory map.  It should not overlap any previous
  // region added with Add(), but it should overlap the base memory map.
  void AddVMRange(uintptr_t vmaddr, size_t vmsize, const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this name becomes an alias of the
  // previous name.
  //
  // This is for things like symbol tables that sometimes map multiple names to
  // the same physical function.
  void AddVMRangeAllowAlias(uintptr_t vmaddr, size_t size,
                            const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this add is simply ignored.
  //
  // This is for cases like sourcefiles.  Sometimes a single function appears to
  // come from multiple source files.  But if it does, we don't want to alias
  // the entire source file to another, because it's probably only part of the
  // source file that overlaps.
  void AddVMRangeIgnoreDuplicate(uintptr_t vmaddr, size_t size,
                                 const std::string& name);

 private:
  MemoryMap* map_;
  const MemoryFileMap* translator_;
};

// An interface for adding address -> address references to a map.
// Used by modules that know how to scan for these references.
class AddressReferenceSink {
 public:
  void Add(uintptr_t from, uintptr_t to);
};

// Contains a label -> label map specifying dependencies.
// When X -> Y is in the map, X depends on Y.
class DependencyMap {
 public:
  void Add(const std::string& name, const std::string& depends_on);

  // Starting at the given memory map's entry point, trace out all dependencies
  // and add unreachable symbols to garbage.
  void FindGarbage(const MemoryMap& map, std::vector<std::string>* garbage);

  // Writes a graph in .dot format showing the overall map of the binary and its
  // size.
  void WriteWeightTree(const MemoryMap& map, const std::string& filename);

 private:
  std::unordered_map<std::string, std::set<std::string>> deps_;
};

// Each function that can read a certain kind of info (segments, sections,
// symbols, etc) registers itself as a data source.
struct DataSource {
  enum Type {
    DATA_SOURCE_VM_RANGE,      // Data source fills a VMRangeSink.
    DATA_SOURCE_VM_FILE_RANGE  // Data source fills a VMFileRangeSink.
  };

  DataSource(Type type_, const std::string& name_) : type(type_), name(name_) {}

  Type type;

  // The name, as specified on the command-line
  std::string name;

  typedef void VMRangeFunc(const std::string& filename, VMRangeSink* sink);
  typedef void VMFileRangeFunc(const std::string& filename,
                               VMFileRangeSink* map);

  // The function that will populate the data structure.
  union {
    VMRangeFunc *vm_range;
    VMFileRangeFunc *vm_file_range;
  } func;
};

inline DataSource VMRangeDataSource(const std::string& name,
                                    DataSource::VMRangeFunc* func) {
  DataSource ret(DataSource::DATA_SOURCE_VM_RANGE, name);
  ret.func.vm_range = func;
  return ret;
}

inline DataSource VMFileRangeDataSource(const std::string& name,
                                        DataSource::VMFileRangeFunc* func) {
  DataSource ret(DataSource::DATA_SOURCE_VM_FILE_RANGE, name);
  ret.func.vm_file_range = func;
  return ret;
}

// Provided by arch-specific platform modules.
void RegisterELFDataSources(std::vector<DataSource>* sources);
void RegisterMachODataSources(std::vector<DataSource>* sources);

// Provided by dwarf.cc.
void ReadDWARFSourceFiles(const std::string& filename, VMRangeSink* sink);
void ReadDWARFLineInfo(const std::string& filename, VMRangeSink* sink,
                       bool include_line);


/** LineReader ****************************************************************/

// Provides range-based for to iterate over lines in a pipe.
//
// for ( auto& line : ReadLinesFromPipe("ls -l") ) {
// }

class LineIterator;

class LineReader {
 public:
  LineReader(FILE* file, bool pclose) : file_(file), pclose_(pclose) {}
  LineReader(LineReader&& other);

  ~LineReader() { Close(); }

  LineIterator begin();
  LineIterator end();

  void Next();

  const std::string& line() const { return line_; }
  bool eof() { return eof_; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(LineReader);

  void Close();

  FILE* file_;
  std::string line_;
  bool eof_ = false;
  bool pclose_;
};

class LineIterator {
 public:
  LineIterator(LineReader* reader) : reader_(reader) {}

  bool operator!=(const LineIterator& other) const {
    // Hack for range-based for.
    return !reader_->eof();
  }

  void operator++() { reader_->Next(); }

  const std::string& operator*() const {
    return reader_->line();
  }

 private:
  LineReader* reader_;
};

LineReader ReadLinesFromPipe(const std::string& cmd);

}  // namespace bloaty

#endif
