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

// Contains a [range] -> label map for VM space only.  This makes sense for
// things defined only in terms of VM addresses (symbols, anything that comes
// from debug information).
class MemoryMap {
 public:
  // If we construct this with a MemoryFileMap, it lets us map VM addresses to
  // file offsets, so we can view everything in the file domain if we want.
  // We can also check that everything we define is covered by the base map (it
  // is unusual/unexpected that a symbol would lie outside of any section, for
  // example).
  MemoryMap(const MemoryFileMap* base);
  virtual ~MemoryMap();

  // Adds a regex that will be applied to all labels prior to inserting them in
  // the map.  All regexes will be applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);

  // Adds a region to the memory map.  It should not overlap any previous
  // region added with Add(), but it should overlap the base memory map.
  void AddVMRange(uintptr_t vmaddr, size_t size, const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this name becomes an alias of the
  // previous name.
  void AddVMRangeAllowAlias(uintptr_t vmaddr, size_t size,
                            const std::string& name);

  bool FindAtAddr(uintptr_t vmaddr, std::string* name) const;
  bool FindContainingAddr(uintptr_t vmaddr, uintptr_t* start,
                          std::string* name) const;

  // Returns true if the given vm addresses are completely covered by this map.
  bool CoversVMAddresses(uintptr_t vmaddr, size_t vmsize) const;

  const RangeMap* vm_map() const { return vm_map_.get(); }
  const MemoryFileMap* base() const { return base_; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MemoryMap);

  const MemoryFileMap* base_;
  std::unique_ptr<RangeMap> vm_map_;
  std::unordered_map<std::string, std::string> aliases_;
  std::vector<std::pair<std::unique_ptr<RE2>, std::string>> regexes_;
};

// Contains [range] -> label maps for both VM space and file space.
// This makes sense for Segments and Sections, which are defined in both spaces
// (ie. object files specify both VM and file offsets for each segment/section).
// This gives us information about sections/segments that live in one but not
// the other (eg. .debug_info lives only in the file, .bss lives only in RAM),
// and also lets us translate addresses between the two domains.
class MemoryFileMap : public MemoryMap {
 public:
  MemoryFileMap(const MemoryFileMap* base);
  virtual ~MemoryFileMap();

  // If vmsize or filesize is zero, this mapping is presumed not to exist in
  // that domain.  For example, .bss mappings don't exist in the file, and
  // .debug_* mappings don't exist in memory.
  void AddFileRange(const std::string& name, uintptr_t vmaddr, size_t vmsize,
                    long fileoff, long filesize);

  // Fills in generic "Unmapped" sections for portions of the file that have no
  // mapping.
  void FillInUnmapped(long filesize);

  // Returns true if the given file ranges are completely covered by this map.
  bool CoversFileOffsets(uintptr_t fileoff, size_t filesize) const;

  const RangeMap* file_map() { return file_map_.get(); }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MemoryFileMap);

  std::unique_ptr<RangeMap> file_map_;

  // Maps each vm_map_ start address to a corresponding file_map_ start address.
  // This will allow us to map VM addresses to file address if we choose to.
  std::unordered_map<uintptr_t, uintptr_t> vm_to_file_;
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

  typedef void MemoryMapFunc(const std::string& filename, MemoryMap* map);
  typedef void MemoryFileMapFunc(
      const std::string& filename, MemoryFileMap* map);

  // The function that will populate the data structure.
  union {
    MemoryMapFunc *map;
    MemoryFileMapFunc *filemap;
  } func;
};

inline DataSource MemoryMapDataSource(const std::string& name,
                                      DataSource::MemoryMapFunc* func) {
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
