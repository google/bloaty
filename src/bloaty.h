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

#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "capstone.h"
#include "re2/re2.h"

#define BLOATY_DISALLOW_COPY_AND_ASSIGN(class_name) \
  class_name(const class_name&) = delete; \
  void operator=(const class_name&) = delete;

#if ABSL_HAVE_BUILTIN(__builtin_unreachable)
#define BLOATY_UNREACHABLE() do { \
  assert(false); \
  __builtin_unreachable(); \
} while (0)
#else
#define BLOATY_UNREACHABLE() do { \
  assert(false); \
} while (0)
#endif

#if ABSL_HAVE_BUILTIN(__builtin_expect)
#define BLOATY_EXPECT(pred, val) __builtin_expect(pred, val)
#else
#define BLOATY_EXPECT(pred, val) pred
#endif

#ifdef NDEBUG
// Prevent "unused variable" warnings.
#define BLOATY_ASSERT(expr) do {} while (false && (expr))
#else
#define BLOATY_ASSERT(expr) assert(expr)
#endif

namespace bloaty {

class NameMunger;
class Options;
struct DualMap;
struct DisassemblyInfo;

enum class DataSource {
  kArchiveMembers,
  kCompileUnits,
  kInlines,
  kSections,
  kSegments,

  // We always set this to one of the concrete symbol types below before
  // setting it on a sink.
  kSymbols,

  kRawSymbols,
  kFullSymbols,
  kShortSymbols
};

class Error : public std::runtime_error {
 public:
  Error(const char* msg, const char* file, int line)
      : std::runtime_error(msg), file_(file), line_(line) {}
  Error(const char* msg) : std::runtime_error(msg) {}

  // TODO(haberman): add these to Bloaty's error message when verbose is
  // enabled.
  const char* file() const { return file_; }
  int line() const { return line_; }

 private:
  const char* file_ = nullptr;
  int line_ = 0;
};

void ThrowBloatyError(const char *str);

class InputFile {
 public:
  InputFile(const std::string& filename) : filename_(filename) {}
  virtual ~InputFile() {}

  const std::string& filename() const { return filename_; }
  absl::string_view data() const { return data_; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(InputFile);
  const std::string filename_;

 protected:
  absl::string_view data_;
};

class InputFileFactory {
 public:
  virtual ~InputFileFactory() {}

  // Throws if the file could not be opened.
  virtual std::unique_ptr<InputFile> OpenFile(
      const std::string& filename) const = 0;
};

class MmapInputFileFactory : public InputFileFactory {
 public:
  std::unique_ptr<InputFile> OpenFile(
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
            const DualMap* translator);
  ~RangeSink();

  void AddOutput(DualMap* map, const NameMunger* munger);

  DataSource data_source() const { return data_source_; }
  const InputFile& input_file() const { return *file_; }

  // If vmsize or filesize is zero, this mapping is presumed not to exist in
  // that domain.  For example, .bss mappings don't exist in the file, and
  // .debug_* mappings don't exist in memory.
  void AddRange(absl::string_view name, uint64_t vmaddr, uint64_t vmsize,
                uint64_t fileoff, uint64_t filesize);

  void AddRange(absl::string_view name, uint64_t vmaddr, uint64_t vmsize,
                      absl::string_view file_range) {
    AddRange(name, vmaddr, vmsize, file_range.data() - file_->data().data(),
             file_range.size());
  }

  void AddFileRange(absl::string_view name,
                    uint64_t fileoff, uint64_t filesize);

  void AddFileRange(absl::string_view name, absl::string_view file_range) {
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
  const DualMap* translator_;
  std::vector<std::pair<DualMap*, const NameMunger*>> outputs_;
};


// NameMunger //////////////////////////////////////////////////////////////////

// Use to transform input names according to the user's configuration.
// For example, the user can use regexes.
class NameMunger {
 public:
  NameMunger() {}

  // Adds a regex that will be applied to all names.  All regexes will be
  // applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);

  std::string Munge(absl::string_view name) const;

  bool IsEmpty() const { return regexes_.empty(); }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(NameMunger);
  std::vector<std::pair<std::unique_ptr<RE2>, std::string>> regexes_;
};

typedef std::map<absl::string_view, std::pair<uint64_t, uint64_t>> SymbolTable;

// Represents an object/executable file in a format like ELF, Mach-O, PE, etc.
// To support a new file type, implement this interface.
class ObjectFile {
 public:
  ObjectFile(std::unique_ptr<InputFile> file_data)
      : file_data_(std::move(file_data)) {}
  virtual ~ObjectFile() {}
  virtual void ProcessBaseMap(RangeSink* sink) = 0;

  // Process this file, pushing data to |sinks| as appropriate for each data
  // source.
  virtual void ProcessFile(const std::vector<RangeSink*>& sinks) = 0;

  virtual bool GetDisassemblyInfo(absl::string_view symbol,
                                  DataSource symbol_source,
                                  DisassemblyInfo* info) = 0;

  const InputFile& file_data() const { return *file_data_; }

 private:
  std::unique_ptr<InputFile> file_data_;
};

std::unique_ptr<ObjectFile> TryOpenELFFile(std::unique_ptr<InputFile>& file);
std::unique_ptr<ObjectFile> TryOpenMachOFile(std::unique_ptr<InputFile>& file);

namespace dwarf {

struct File {
  absl::string_view debug_info;
  absl::string_view debug_types;
  absl::string_view debug_str;
  absl::string_view debug_abbrev;
  absl::string_view debug_aranges;
  absl::string_view debug_line;
};

}  // namespace dwarf

// Provided by dwarf.cc.  To use these, a module should fill in a dwarf::File
// and then call these functions.
void ReadDWARFCompileUnits(const dwarf::File& file, const SymbolTable& symtab,
                           RangeSink* sink);
void ReadDWARFInlines(const dwarf::File& file, RangeSink* sink,
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

// Demangle C++ symbols according to the Itanium ABI.  The |source| argument
// controls what demangling mode we are using.
std::string ItaniumDemangle(absl::string_view symbol, DataSource source);


// RangeMap ////////////////////////////////////////////////////////////////////

// Maps
//
//   [uint64_t, uint64_t) -> std::string, [optional other range base]
//
// where ranges must be non-overlapping.
//
// This is used to map the address space (either pointer offsets or file
// offsets).
//
// The other range base allows us to use this RangeMap to translate addresses
// from this domain to another one (like vm_addr -> file_addr or vice versa).
//
// This type is only exposed in the .h file for unit testing purposes.

class RangeMapTest;

class RangeMap {
 public:
  RangeMap() = default;
  RangeMap(RangeMap&& other) = default;
  RangeMap& operator=(RangeMap&& other) = default;

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

  // Looks for a range within this map that contains |addr|.  If found, returns
  // true and sets |label| to the corresponding label, and |offset| to the
  // offset from the beginning of this range.
  bool TryGetLabel(uint64_t addr, std::string* label, uint64_t* offset) const;

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
};


// DualMap /////////////////////////////////////////////////////////////////////

// Contains a RangeMap for VM space and file space for a given file.

struct DualMap {
  RangeMap vm_map;
  RangeMap file_map;
};

struct DisassemblyInfo {
  absl::string_view text;
  DualMap symbol_map;
  cs_arch arch;
  cs_mode mode;
  uint64_t start_address;
};

std::string DisassembleFunction(const DisassemblyInfo& info);

// Top-level API ///////////////////////////////////////////////////////////////

// This should only be used by main.cc and unit tests.

class Rollup;

struct RollupRow {
  RollupRow(const std::string& name_) : name(name_) {}

  std::string name;
  int64_t vmsize = 0;
  int64_t filesize = 0;
  int64_t other_count = 0;
  int64_t sortkey;
  double vmpercent;
  double filepercent;
  std::vector<RollupRow> sorted_children;

  static bool Compare(const RollupRow& a, const RollupRow& b) {
    // Sort value high-to-low.
    if (a.sortkey != b.sortkey) {
      return a.sortkey > b.sortkey;
    }
    // Sort name low to high.
    return a.name < b.name;
  }
};

enum class OutputFormat {
  kPrettyPrint,
  kCSV,
};

struct OutputOptions {
  OutputFormat output_format = OutputFormat::kPrettyPrint;
  size_t max_label_len = 80;
};

struct RollupOutput {
 public:
  RollupOutput() : toplevel_row_("TOTAL") {}

  void AddDataSourceName(absl::string_view name) {
    source_names_.emplace_back(std::string(name));
  }

  const std::vector<std::string>& source_names() const { return source_names_; }

  void Print(const OutputOptions& options, std::ostream* out) {
    if (!source_names_.empty()) {
      switch (options.output_format) {
        case bloaty::OutputFormat::kPrettyPrint:
          PrettyPrint(options.max_label_len, out);
          break;
        case bloaty::OutputFormat::kCSV:
          PrintToCSV(out);
          break;
        default:
          BLOATY_UNREACHABLE();
      }
    }

    if (!disassembly_.empty()) {
      *out << disassembly_;
    }
  }

  void SetDisassembly(absl::string_view disassembly) {
    disassembly_ = std::string(disassembly);
  }

  absl::string_view GetDisassembly() { return disassembly_; }

  // For debugging.
  const RollupRow& toplevel_row() const { return toplevel_row_; }
  bool diff_mode() const { return diff_mode_; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(RollupOutput);
  friend class Rollup;

  std::vector<std::string> source_names_;
  RollupRow toplevel_row_;
  std::string disassembly_;

  // When we are in diff mode, rollup sizes are relative to the baseline.
  bool diff_mode_ = false;

  void PrettyPrint(size_t max_label_len, std::ostream* out) const;
  void PrintToCSV(std::ostream* out) const;
  size_t CalculateLongestLabel(const RollupRow& row, int indent) const;
  void PrettyPrintRow(const RollupRow& row, size_t indent, size_t longest_row,
                      std::ostream* out) const;
  void PrettyPrintTree(const RollupRow& row, size_t indent, size_t longest_row,
                       std::ostream* out) const;
  void PrintRowToCSV(const RollupRow& row,
                     std::vector<std::string> parent_labels,
                     std::ostream* out) const;
  void PrintTreeToCSV(const RollupRow& row,
                      std::vector<std::string> parent_labels,
                      std::ostream* out) const;
};

bool ParseOptions(bool skip_unknown, int* argc, char** argv[], Options* options,
                  OutputOptions* output_options, std::string* error);
bool BloatyMain(const Options& options, const InputFileFactory& file_factory,
                RollupOutput* output, std::string* error);

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

struct ByteSwapFunc {
  template <class T>
  T operator()(T val) {
    return ByteSwap(val);
  }
};

struct NullFunc {
  template <class T>
  T operator()(T val) { return val; }
};

// File formats like ELF/Mach-O vary between 32/64 bits and also can be both
// big/little endian.  For applications that only care about reading "native"
// files (binaries that match the system you're running on), this doesn't
// usually matter, but we want Bloaty to handle reading non-native files.
// Maybe you are cross-compiling or analyzing a binary from another system.
//
// The 32/64 bit structures can vary in member sizes and even orders!  For
// example, take ELF segment headers:
//
// typedef struct {
// 	Elf32_Word	p_type;		/* Entry type. */
// 	Elf32_Off	p_offset;	/* File offset of contents. */
// 	Elf32_Addr	p_vaddr;	/* Virtual address in memory image. */
// 	Elf32_Addr	p_paddr;	/* Physical address (not used). */
// 	Elf32_Word	p_filesz;	/* Size of contents in file. */
// 	Elf32_Word	p_memsz;	/* Size of contents in memory. */
// 	Elf32_Word	p_flags;	/* Access permission flags. */
// 	Elf32_Word	p_align;	/* Alignment in memory and file. */
// } Elf32_Phdr;
//
// typedef struct {
// 	Elf64_Word	p_type;		/* Entry type. */
// 	Elf64_Word	p_flags;	/* Access permission flags. */
// 	Elf64_Off	p_offset;	/* File offset of contents. */
// 	Elf64_Addr	p_vaddr;	/* Virtual address in memory image. */
// 	Elf64_Addr	p_paddr;	/* Physical address (not used). */
// 	Elf64_Xword	p_filesz;	/* Size of contents in file. */
// 	Elf64_Xword	p_memsz;	/* Size of contents in memory. */
// 	Elf64_Xword	p_align;	/* Alignment in memory and file. */
// } Elf64_Phdr;
//
// Bloaty will always use 64-bit data-types internally, so that 64-bit files
// can be analyzed with any build of Bloaty.  So when we're analyzing 32-bit
// files, we need to convert from the 32-bit struct to the 64-bit one.
//
// StructPtr encodes a pointer to a struct, along with tags for bit size and
// endianness.  The client always reads the 64-bit structure members, and
// StructPtr will convert from 32-bits if necessary, and byte-swap if
// necessary.
//
// To define the 32/64-bit structure types and the mapping between them, you
// create a class like so:
//
// struct ElfPhdr {
//   typedef Elf32_Phdr Struct32;
//   typedef Elf64_Phdr Struct64;
//
//   template <class From, class Func>
//   void operator()(const From& from, Struct64* to, Func func) {
//     to->p_type   = func(from.p_type);
//     to->p_flags  = func(from.p_flags);
//     to->p_offset = func(from.p_offset);
//     to->p_vaddr  = func(from.p_vaddr);
//     to->p_paddr  = func(from.p_paddr);
//     to->p_filesz = func(from.p_filesz);
//     to->p_memsz  = func(from.p_memsz);
//     to->p_align  = func(from.p_align);
//   }
// };
//
// Then use it like so:
//
//   // Throws if buffer isn't large enough for this struct.
//   StructPtr<ElfPhdr> phdr(is_cross_endian, is_32bit, data);
//
//   // We can read & convert the whole struct at once if we want to.
//   // For the trivial case (64-bit, native endian) this will just be memcpy().
//   Elf64_Phdr phdr64;
//   phdr.ReadStruct(&phdr64);
//
//   // We can also read specific members, to avoid the work of converting
//   // the whole structure.  Reading multiple members at once will probably
//   // help the compiler hoist the checks for whether we have to convert.
//   Elf64_Off offset = phdr.ReadMember(&Elf64::p_offset);
//   Elf64_Off flags = phdr.ReadMember(&Elf64::p_flags);
//
//   // For reading multiple at a time.
//   std::tie(offset, flags) =
//       phdr.ReadTuple(&Elf64::p_offset, &Elf64::p_flags);
template <class T>
class StructPtr {
 public:
  StructPtr(bool is_cross_endian, bool is_32bit, absl::string_view data)
      : StructPtr(is_cross_endian | (is_32bit << 1), data) {}

  // Takes endianness and 32-bit-ness from another StructPtr.
  template <typename U>
  StructPtr(StructPtr<U> other, absl::string_view data)
      : StructPtr(other.GetFlags(), data) {}

  StructPtr() : tagged_ptr_(0) {}

  void Reset(bool is_cross_endian, bool is_32bit, absl::string_view data) {
    *this = StructPtr<T>(is_cross_endian, is_32bit, data);
  }

  void Advance(absl::string_view* data) {
    data->remove_prefix(GetSize());
  }

  void ReadStruct(typename T::Struct64* data) {
    switch (BLOATY_EXPECT(GetFlags(), 0)) {
      case 0:
        memcpy(data, Ptr64Unsafe(), sizeof(typename T::Struct64));
        break;
      case kIsCrossEndian:
        T()(*Ptr64(), data, ByteSwapFunc());
        break;
      case kIs32Bit:
        T()(*Ptr32(), data, NullFunc());
        break;
      case kIsCrossEndian | kIs32Bit:
        T()(*Ptr32(), data, ByteSwapFunc());
        break;
      default:
        BLOATY_UNREACHABLE();
    }
  }

  template <typename U>
  U ReadMember(U T::Struct64::*member) const {
    // Compiler should be smart enough to skip converting the struct members
    // that aren't used.
    typename T::Struct64 tmp;
    ReadStruct(&tmp);
    return tmp.*member;
  }

  template <typename... Args>
  std::tuple<Args...> ReadTuple(Args T::*...args) {
    // Compiler should be smart enough to skip converting the struct members
    // that aren't used.
    typename T::Struct64 tmp;
    ReadStruct(&tmp);
    return std::make_tuple(tmp.*args...);
  }

 private:
  template <typename U>
  friend class StructPtr;

  static const char kIsCrossEndian = 0x1;
  static const char kIs32Bit = 0x2;

  size_t GetSize() {
    return Is32Bit() ? sizeof(typename T::Struct32)
                     : sizeof(typename T::Struct64);
  }

  char GetFlags() { return tagged_ptr_ & 0x3; }
  bool Is32Bit() { return tagged_ptr_ & kIs32Bit; }

  typename T::Struct64* Ptr64Unsafe() {
    assert(GetFlags() == 0);
    return reinterpret_cast<typename T::Struct64*>(tagged_ptr_);
  }

  typename T::Struct64* Ptr64() {
    assert(!Is32Bit());
    return reinterpret_cast<typename T::Struct64*>(tagged_ptr_ & ~3);
  }

  typename T::Struct32* Ptr32() {
    assert(Is32Bit());
    return reinterpret_cast<typename T::Struct32*>(tagged_ptr_ & ~3);
  }

  StructPtr(char flags, absl::string_view data)
      : tagged_ptr_(reinterpret_cast<intptr_t>(data.data()) | flags) {
    if (GetSize() > data.size()) {
      ThrowBloatyError("Premature EOF reading struct");
    }
  }

  intptr_t tagged_ptr_;
};

}  // namespace bloaty

#endif
