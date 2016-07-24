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

class RangeMap;
class MemoryFileMap;
class VMRangeAdder;

// Contains a [range] -> label map for VM space and file space.
class MemoryMap {
 public:
  MemoryMap();
  virtual ~MemoryMap();

  // For the file space mapping, Fills in generic "Unmapped" sections for
  // portions of the file that have no mapping.
  void FillInUnmapped(long filesize);

  // Adds a regex that will be applied to all labels prior to inserting them in
  // the map.  All regexes will be applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);

  bool FindAtAddr(uintptr_t vmaddr, std::string* name) const;
  bool FindContainingAddr(uintptr_t vmaddr, uintptr_t* start,
                          std::string* name) const;

  const RangeMap* file_map() const { return file_map_.get(); }
  const RangeMap* vm_map() const { return vm_map_.get(); }
  RangeMap* file_map() { return file_map_.get(); }
  RangeMap* vm_map() { return vm_map_.get(); }

 protected:
  std::string ApplyNameRegexes(const std::string& name);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MemoryMap);
  friend class VMRangeAdder;

  std::unique_ptr<RangeMap> vm_map_;
  std::unique_ptr<RangeMap> file_map_;
  std::unordered_map<std::string, std::string> aliases_;
  std::vector<std::pair<std::unique_ptr<RE2>, std::string>> regexes_;
};

// A MemoryMap for things like segments and sections, where every range exists
// in both VM space and file space.  We can use MemoryFileMaps to translate VM
// addresses into file offsets.
class MemoryFileMap : public MemoryMap {
 public:
  MemoryFileMap();
  virtual ~MemoryFileMap();

  // If vmsize or filesize is zero, this mapping is presumed not to exist in
  // that domain.  For example, .bss mappings don't exist in the file, and
  // .debug_* mappings don't exist in memory.
  void AddRange(const std::string& name, uintptr_t vmaddr, size_t vmsize,
                long fileoff, long filesize);

  // Translates a VM address to a file offset.  Returns false if this VM address
  // is not mapped from the file.
  bool TranslateVMAddress(uintptr_t vmaddr, uintptr_t* fileoff) const;

  // Translates a VM address range to a file offset range.  Returns false if
  // nothing in this VM address range is mapped into a file.
  //
  // This VM address range may not translate to multiple discrete file ranges.
  // We generally would never expect this to happen.
  bool TranslateVMRange(uintptr_t vmaddr, size_t vmsize,
                        uintptr_t* fileoff, uintptr_t* filesize) const;

 private:
  // Maps each vm_map_ start address to a corresponding file_map_ start address.
  // If a given vm_map_ start address is missing from the map, it does not come
  // from the file.
  std::unordered_map<uintptr_t, uintptr_t> vm_to_file_;
};

// Allows adding ranges by VM addr/size only.  Uses an underlying MemoryFileMap
// to translate the VM addresses to file addresses.
class VMRangeAdder {
 public:
  VMRangeAdder(MemoryMap* map, const MemoryFileMap* translator)
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
    DATA_SOURCE_MAP,     // Data source fills a MemoryMap.
    DATA_SOURCE_FILEMAP  // Data source fills a MemoryFileMap.
  };

  DataSource(Type type_, const std::string& name_) : type(type_), name(name_) {}

  Type type;

  // The name, as specified on the command-line
  std::string name;

  typedef void VMRangeAdderFunc(const std::string& filename,
                                VMRangeAdder* adder);
  typedef void MemoryFileMapFunc(
      const std::string& filename, MemoryFileMap* map);

  // The function that will populate the data structure.
  union {
    VMRangeAdderFunc *map;
    MemoryFileMapFunc *filemap;
  } func;
};

inline DataSource VMRangeAdderDataSource(const std::string& name,
                                         DataSource::VMRangeAdderFunc* func) {
  DataSource ret(DataSource::DATA_SOURCE_MAP, name);
  ret.func.map = func;
  return ret;
}

inline DataSource MemoryFileMapDataSource(const std::string& name,
                                          DataSource::MemoryFileMapFunc* func) {
  DataSource ret(DataSource::DATA_SOURCE_FILEMAP, name);
  ret.func.filemap = func;
  return ret;
}

// Provided by arch-specific platform modules.
void RegisterELFDataSources(std::vector<DataSource>* sources);
void RegisterMachODataSources(std::vector<DataSource>* sources);

// Provided by dwarf.cc.
void ReadDWARFSourceFiles(const std::string& filename, VMRangeAdder* adder);
void ReadDWARFLineInfo(const std::string& filename, VMRangeAdder* adder,
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
