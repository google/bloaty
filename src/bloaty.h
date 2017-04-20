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

// This file contains APIs for use within Bloaty.  None of these APIs have any
// guarantees whatsoever about their stability!  The public API for bloaty is
// its command-line interface.

#ifndef BLOATY_H_
#define BLOATY_H_

#include <stdlib.h>
#define __STDC_LIMIT_MACROS
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

typedef re2::StringPiece StringPiece;

class MemoryMap;

enum class DataSource {
  kArchiveMembers,
  kCompileUnits,
  kInlines,
  kSections,
  kSegments,
  kSymbols,
};

class InputFile {
 public:
  InputFile(const std::string& filename) : filename_(filename) {}
  virtual ~InputFile() {}

  const std::string& filename() const { return filename_; }
  StringPiece data() const { return data_; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(InputFile);
  const std::string filename_;

 protected:
  StringPiece data_;
};

class InputFileFactory {
 public:
  // Returns nullptr if the file could not be opened.
  virtual std::unique_ptr<InputFile> TryOpenFile(
      const std::string& filename) const = 0;
};

class MmapInputFileFactory : public InputFileFactory {
 public:
  std::unique_ptr<InputFile> TryOpenFile(
      const std::string& filename) const override;
};

// NOTE: all sizes are uint64, even on 32-bit platforms:
//   - 32-bit platforms can have files >4GB in some cases.
//   - for object files (not executables/shared libs) we pack both a section
//     index and an address into the "vmaddr" value, and we need enough bits to
//     safely do this.

// A RangeSink allows data sources to assign labels to ranges of VM address
// space and/or file offsets.
class RangeSink {
 public:
  RangeSink(const InputFile* file, DataSource data_source,
            const MemoryMap* translator, MemoryMap* map);
  ~RangeSink();

  DataSource data_source() const { return data_source_; }
  const InputFile& input_file() const { return *file_; }

  // If vmsize or filesize is zero, this mapping is presumed not to exist in
  // that domain.  For example, .bss mappings don't exist in the file, and
  // .debug_* mappings don't exist in memory.
  void AddRange(StringPiece name, uint64_t vmaddr, uint64_t vmsize,
                uint64_t fileoff, uint64_t filesize);

  void AddRange(StringPiece name, uint64_t vmaddr, uint64_t vmsize,
                      StringPiece file_range) {
    AddRange(name, vmaddr, vmsize, file_range.data() - file_->data().data(),
             file_range.size());
  }

  void AddFileRange(StringPiece name, uint64_t fileoff, uint64_t filesize);

  void AddFileRange(StringPiece name, StringPiece file_range) {
    AddFileRange(name, file_range.data() - file_->data().data(),
                 file_range.size());
  }

  // The VM-only functions below may not be used to populate the base map!

  // Adds a region to the memory map.  It should not overlap any previous
  // region added with Add(), but it should overlap the base memory map.
  void AddVMRange(uint64_t vmaddr, uint64_t vmsize, const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this name becomes an alias of the
  // previous name.
  //
  // This is for things like symbol tables that sometimes map multiple names to
  // the same physical function.
  void AddVMRangeAllowAlias(uint64_t vmaddr, uint64_t size,
                            const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this add is simply ignored.
  //
  // This is for cases like sourcefiles.  Sometimes a single function appears to
  // come from multiple source files.  But if it does, we don't want to alias
  // the entire source file to another, because it's probably only part of the
  // source file that overlaps.
  void AddVMRangeIgnoreDuplicate(uint64_t vmaddr, uint64_t size,
                                 const std::string& name);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(RangeSink);

  const InputFile* file_;
  DataSource data_source_;
  const MemoryMap* translator_;
  MemoryMap* map_;
};

// The main interface that modules should implement to handle a particular file
// type.
class FileHandler {
 public:
  virtual ~FileHandler() {}

  virtual bool ProcessBaseMap(RangeSink* sink) = 0;

  // Process this file, pushing data to |sinks| as appropriate for each data
  // source.
  virtual bool ProcessFile(const std::vector<RangeSink*>& sinks) = 0;
};

std::unique_ptr<FileHandler> TryOpenELFFile(const InputFile& file);
std::unique_ptr<FileHandler> TryOpenMachOFile(const InputFile& file);

namespace dwarf {

struct File {
  StringPiece debug_info;
  StringPiece debug_types;
  StringPiece debug_str;
  StringPiece debug_abbrev;
  StringPiece debug_aranges;
  StringPiece debug_line;
};

}  // namespace dwarf

typedef std::map<StringPiece, std::pair<uint64_t, uint64_t>> SymbolTable;

// Provided by dwarf.cc.  To use these, a module should fill in a dwarf::File
// and then call these functions.
bool ReadDWARFCompileUnits(const dwarf::File& file, const SymbolTable& symtab,
                           RangeSink* sink);
bool ReadDWARFInlines(const dwarf::File& file, RangeSink* sink,
                      bool include_line);

// LineReader //////////////////////////////////////////////////////////////////

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

  bool operator!=(const LineIterator& /*other*/) const {
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


// RangeMap ////////////////////////////////////////////////////////////////////

// Maps
//
//   [uint64_t, uint64_t) -> std::string
//
// where ranges must be non-overlapping.
//
// This is used to map the address space (either pointer offsets or file
// offsets).
//
// This type is only exposed in the .h file for unit testing purposes.

class RangeMapTest;

class RangeMap {
 public:
  RangeMap() {}

  // Adds a range to this map.
  void AddRange(uint64_t addr, uint64_t size, const std::string& val);

  // Adds a range to this map (in domain D1) that also corresponds to a
  // different range in a different map (in domain D2).  The correspondance will
  // be noted to allow us to translate into the other domain later.
  void AddDualRange(uint64_t addr, uint64_t size, uint64_t otheraddr,
                    const std::string& val);

  // Adds a range to this map (in domain D1), and also adds corresponding ranges
  // to |other| (in domain D2), using |translator| (in domain D1) to translate
  // D1->D2.  The translation is performed using information from previous
  // AddDualRange() calls on |translator|.
  void AddRangeWithTranslation(uint64_t addr, uint64_t size,
                               const std::string& val,
                               const RangeMap& translator, RangeMap* other);

  // Translates |addr| into the other domain, returning |true| if this was
  // successful.
  bool Translate(uint64_t addr, uint64_t *translated) const;

  template <class Func>
  static void ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                            const std::string& filename, int filename_position,
                            Func func);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(RangeMap);

  friend class RangeMapTest;

  struct Entry {
    Entry(const std::string& label_, uint64_t end_, uint64_t other_)
        : label(label_), end(end_), other_start(other_) {}
    std::string label;
    uint64_t end;
    uint64_t other_start;  // UINT64_MAX if there is no mapping.

    bool HasTranslation() const { return other_start != UINT64_MAX; }
  };

  typedef std::map<uint64_t, Entry> Map;
  Map mappings_;

  template <class T>
  static bool EntryContains(T iter, uint64_t addr) {
    return addr >= iter->first && addr < iter->second.end;
  }

  static uint64_t RangeEnd(Map::const_iterator iter) {
    return iter->second.end;
  }

  bool IterIsEnd(Map::const_iterator iter) const {
    return iter == mappings_.end();
  }

  template <class T>
  static uint64_t TranslateWithEntry(T iter, uint64_t addr);

  template <class T>
  static bool TranslateAndTrimRangeWithEntry(T iter, uint64_t addr,
                                             uint64_t end, uint64_t* out_addr,
                                             uint64_t* out_end);

  // Finds the entry that contains |addr|.  If no such mapping exists, returns
  // mappings_.end().
  Map::iterator FindContaining(uint64_t addr);
  Map::const_iterator FindContaining(uint64_t addr) const;
  Map::const_iterator FindContainingOrAfter(uint64_t addr) const;

  Entry* TryGet(uint64_t addr, uint64_t* start, uint64_t* size) const;
  const std::string* TryGetExactly(uint64_t addr, uint64_t* size) const;
};


// Top-level API ///////////////////////////////////////////////////////////////

// This should only be used by main.cc and unit tests.

class Rollup;

struct RollupRow {
  RollupRow(const std::string& name_) : name(name_) {}

  std::string name;
  int64_t vmsize = 0;
  int64_t filesize = 0;
  double vmpercent;
  double filepercent;
  std::vector<RollupRow> sorted_children;
  std::vector<RollupRow> shrinking;
  std::vector<RollupRow> mixed;

  // When this is false, sorted_children contains actual sizes, and
  // shrinking/mixed are unused.
  //
  // When this is true, sorted_children contains entites that grew, and
  // shrinking/mixed indicate entries that shrank or that had one dimension grow
  // and one shrink.
  bool diff_mode = false;
};

struct RollupOutput {
 public:
  RollupOutput() : toplevel_row_("TOTAL") {}
  const RollupRow& toplevel_row() { return toplevel_row_; }
  void Print(std::ostream* out) const;

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(RollupOutput);
  friend class Rollup;

  size_t longest_label_;
  RollupRow toplevel_row_;

  void PrintRow(const RollupRow& row, size_t indent, std::ostream* out) const;
  void PrintTree(const RollupRow& row, size_t indent, std::ostream* out) const;
};

bool BloatyMain(int argc, char* argv[], const InputFileFactory& file_factory,
                RollupOutput* output);

// Endianness utilities ////////////////////////////////////////////////////////

inline bool IsLittleEndian() {
  int x = 1;
  return *(char*)&x == 1;
}

// It seems like it would be simpler to just specialize on:
//   template <class T> T ByteSwap(T val);
//   template <> T ByteSwap<uint16>(T val) { /* ... */ }
//   template <> T ByteSwap<uint32>(T val) { /* ... */ }
//   // etc...
//
// But this doesn't work out so well.  Consider that on LP32, uint32 could
// be either "unsigned int" or "unsigned long".  Specializing ByteSwap<uint32>
// will leave one of those two unspecialized.  C++ is annoying in this regard.
// Our approach here handles both cases with just one specialization.
template <class T, size_t size> struct ByteSwapper { T operator()(T val); };

template <class T>
struct ByteSwapper<T, 1> {
  T operator()(T val) { return val; }
};

template <class T>
struct ByteSwapper<T, 2> {
  T operator()(T val) {
    return ((val & 0xff) << 8) |
        ((val & 0xff00) >> 8);
  }
};

template <class T>
struct ByteSwapper<T, 4> {
  T operator()(T val) {
    return ((val & 0xff) << 24) |
        ((val & 0xff00) << 8) |
        ((val & 0xff0000ULL) >> 8) |
        ((val & 0xff000000ULL) >> 24);
  }
};

template <class T>
struct ByteSwapper<T, 8> {
  T operator()(T val) {
    return ((val & 0xff) << 56) |
        ((val & 0xff00) << 40) |
        ((val & 0xff0000) << 24) |
        ((val & 0xff000000) << 8) |
        ((val & 0xff00000000ULL) >> 8) |
        ((val & 0xff0000000000ULL) >> 24) |
        ((val & 0xff000000000000ULL) >> 40) |
        ((val & 0xff00000000000000ULL) >> 56);
  }
};

template <class T>
T ByteSwap(T val) { return ByteSwapper<T, sizeof(T)>()(val); }

}  // namespace bloaty

#endif
