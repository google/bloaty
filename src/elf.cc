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

#include <algorithm>
#include <string>
#include <iostream>
#include "absl/numeric/int128.h"
#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "third_party/freebsd_elf/elf.h"
#include "bloaty.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

// Not present in the FreeBSD ELF headers.
#define NT_GNU_BUILD_ID 3

using absl::string_view;

ABSL_ATTRIBUTE_NORETURN
static void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)
#define WARN(x) fprintf(stderr, "bloaty: %s\n", x);

namespace bloaty {

namespace {

uint64_t CheckedAdd(uint64_t a, uint64_t b) {
  absl::uint128 a_128(a), b_128(b);
  absl::uint128 c_128 = a_128 + b_128;
  if (c_128 > UINT64_MAX) {
    THROW("integer overflow in addition");
  }
  return static_cast<uint64_t>(c_128);
}

uint64_t CheckedMul(uint64_t a, uint64_t b) {
  absl::uint128 a_128(a), b_128(b);
  absl::uint128 c = a * b;
  if (c > UINT64_MAX) {
    THROW("integer overflow in multiply");
  }
  return static_cast<uint64_t>(c);
}

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

size_t StringViewToSize(string_view str) {
  size_t ret;
  if (!absl::SimpleAtoi(str, &ret)) {
    THROWF("couldn't convert string '$0' to integer.", str);
  }
  return ret;
}

template <class T>
const T* GetStructPointer(string_view data) {
  if (sizeof(T) > data.size()) {
    THROW("Premature EOF reading ELF data.");
  }
  return reinterpret_cast<const T*>(data.data());
}

template <class T>
void AdvancePastStruct(string_view* data) {
  *data = data->substr(sizeof(T));
}

static string_view StrictSubstr(string_view data, size_t off, size_t n) {
  uint64_t end = CheckedAdd(off, n);
  if (end > data.size()) {
    THROW("ELF region out-of-bounds");
  }
  return data.substr(off, n);
}

static string_view StrictSubstr(string_view data, size_t off) {
  if (off > data.size()) {
    THROW("ELF region out-of-bounds");
  }
  return data.substr(off);
}

static size_t AlignUp(size_t offset, size_t granularity) {
  // Granularity must be a power of two.
  return (offset + granularity - 1) & ~(granularity - 1);
}

// ElfFile /////////////////////////////////////////////////////////////////////

// For parsing the pieces we need out of an ELF file (.o, .so, and binaries).

class ElfFile {
 public:
  ElfFile(string_view data) : data_(data) {
    ok_ = Initialize();
  }

  bool IsOpen() { return ok_; }

  // Regions of the file where different headers live.
  string_view entire_file() const { return data_; }
  string_view header_region() const { return header_region_; }
  string_view section_headers() const { return section_headers_; }
  string_view segment_headers() const { return segment_headers_; }

  const Elf64_Ehdr& header() const { return header_; }
  Elf64_Xword section_count() const { return section_count_; }
  Elf64_Xword section_string_index() const { return section_string_index_; }

  // Represents an ELF segment (data used by the loader / dynamic linker).
  class Segment {
   public:
    const Elf64_Phdr& header() const { return header_; }
    string_view contents() const { return contents_; }
    string_view range() const { return range_; }

   private:
    friend class ElfFile;
    Elf64_Phdr header_;
    string_view contents_;
    string_view range_;
  };

  // Represents an ELF section (.text, .data, .bss, etc.)
  class Section {
   public:
    const Elf64_Shdr& header() const { return header_; }
    string_view contents() const { return contents_; }
    string_view range() const { return range_; }

    // For SHN_UNDEF (undefined name), returns [nullptr, 0].
    string_view GetName() const;

    // Requires: this is a section with fixed-width entries (symbol table,
    // relocation table, etc).
    Elf64_Word GetEntryCount() const;

    // Requires: header().sh_type == SHT_STRTAB.
    string_view ReadString(Elf64_Word index) const;

    // Requires: header().sh_type == SHT_SYMTAB || header().sh_type ==
    // SHT_DYNSYM
    void ReadSymbol(Elf64_Word index, Elf64_Sym* sym,
                    string_view* file_range) const;

    // Requires: header().sh_type == SHT_REL
    void ReadRelocation(Elf64_Word index, Elf64_Rel* rel,
                        string_view* file_range) const;

    // Requires: header().sh_type == SHT_RELA
    void ReadRelocationWithAddend(Elf64_Word index, Elf64_Rela* rel,
                                  string_view* file_range) const;

    const ElfFile& elf() const { return *elf_; }

   private:
    friend class ElfFile;
    const ElfFile* elf_;
    Elf64_Shdr header_;
    string_view contents_;
    string_view range_;
  };

  class NoteIter {
   public:
    NoteIter(const Section& section)
        : elf_(&section.elf()), remaining_(section.contents()) {
      Next();
    }

    bool IsDone() const { return done_; }
    uint32_t type() const { return type_; }
    string_view name() const { return name_; }
    string_view descriptor() const { return descriptor_; }

    void Next();

   public:
    const ElfFile* elf_;
    string_view name_;
    string_view descriptor_;
    string_view remaining_;
    uint32_t type_;
    bool done_ = false;
  };

  void ReadSegment(Elf64_Word index, Segment* segment) const;
  void ReadSection(Elf64_Word index, Section* section) const;

  bool FindSectionByName(absl::string_view name, Section* section) const;

  bool is_64bit() const { return is_64bit_; }
  bool is_native_endian() const { return is_native_endian_; }

 private:
  friend class Section;

  bool Initialize();

  string_view GetRegion(uint64_t start, uint64_t n) const {
    return StrictSubstr(data_, start, n);
  }

  // Shared code for reading various ELF structures.  Handles endianness
  // conversion and 32->64 bit conversion, when necessary.
  class StructReader {
   public:
    StructReader(const ElfFile& elf, string_view data)
        : elf_(elf), data_(data) {}

    template <class T32, class T64, class Munger>
    void Read(uint64_t offset, Munger /*munger*/, absl::string_view* range,
              T64* out) const {
      if (elf_.is_64bit() && elf_.is_native_endian()) {
        return Memcpy(offset, range, out);
      } else {
        return ReadFallback<T32, T64, Munger>(offset, range, out);
      }
    }

   private:
    const ElfFile& elf_;
    string_view data_;

    template <class T32, class T64, class Munger>
    void ReadFallback(uint64_t offset, absl::string_view* range,
                      T64* out) const;

    template <class T>
    void Memcpy(uint64_t offset, absl::string_view* range, T* out) const {
      uint64_t end = CheckedAdd(offset, sizeof(T));
      if (end > data_.size()) {
        THROW("out-of-bounds read to ELF file");
      }
      if (range) {
        *range = absl::string_view(data_.data() + offset, sizeof(*out));
      }
      memcpy(out, data_.data() + offset, sizeof(*out));
    }
  };

  template <class T32, class T64, class Munger>
  void ReadStruct(absl::string_view contents, uint64_t offset, Munger munger,
                  absl::string_view* range, T64* out) const {
    StructReader(*this, contents).Read<T32>(offset, munger, range, out);
  }

  bool ok_;
  bool is_64bit_;
  bool is_native_endian_;
  string_view data_;
  Elf64_Ehdr header_;
  Elf64_Xword section_count_;
  Elf64_Xword section_string_index_;
  string_view header_region_;
  string_view section_headers_;
  string_view segment_headers_;
  Section section_name_table_;
};

// ELF uses different structure definitions for 32/64 bit files.  The sizes of
// members are different, and members are even in a different order!
//
// These mungers can convert 32 bit structures to 64-bit ones.  They can also
// handle converting endianness.  We use templates so a single template function
// can handle all three patterns:
//
//   32 native  -> 64 native
//   32 swapped -> 64 native
//   64 swapped -> 64 native

struct EhdrMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Ehdr* to, Func func) {
    memmove(&to->e_ident[0], &from.e_ident[0], EI_NIDENT);
    to->e_type       = func(from.e_type);
    to->e_machine    = func(from.e_machine);
    to->e_version    = func(from.e_version);
    to->e_entry      = func(from.e_entry);
    to->e_phoff      = func(from.e_phoff);
    to->e_shoff      = func(from.e_shoff);
    to->e_flags      = func(from.e_flags);
    to->e_ehsize     = func(from.e_ehsize);
    to->e_phentsize  = func(from.e_phentsize);
    to->e_phnum      = func(from.e_phnum);
    to->e_shentsize  = func(from.e_shentsize);
    to->e_shnum      = func(from.e_shnum);
    to->e_shstrndx   = func(from.e_shstrndx);
  }
};

struct ShdrMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Shdr* to, Func func) {
    to->sh_name       = func(from.sh_name);
    to->sh_type       = func(from.sh_type);
    to->sh_flags      = func(from.sh_flags);
    to->sh_addr       = func(from.sh_addr);
    to->sh_offset     = func(from.sh_offset);
    to->sh_size       = func(from.sh_size);
    to->sh_link       = func(from.sh_link);
    to->sh_info       = func(from.sh_info);
    to->sh_addralign  = func(from.sh_addralign);
    to->sh_entsize    = func(from.sh_entsize);
  }
};

struct PhdrMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Phdr* to, Func func) {
    to->p_type   = func(from.p_type);
    to->p_flags  = func(from.p_flags);
    to->p_offset = func(from.p_offset);
    to->p_vaddr  = func(from.p_vaddr);
    to->p_paddr  = func(from.p_paddr);
    to->p_filesz = func(from.p_filesz);
    to->p_memsz  = func(from.p_memsz);
    to->p_align  = func(from.p_align);
  }
};

struct SymMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Sym* to, Func func) {
    to->st_name   = func(from.st_name);
    to->st_info   = func(from.st_info);
    to->st_other  = func(from.st_other);
    to->st_shndx  = func(from.st_shndx);
    to->st_value  = func(from.st_value);
    to->st_size   = func(from.st_size);
  }
};

struct RelMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Rel* to, Func func) {
    to->r_offset = func(from.r_offset);
    to->r_info   = func(from.r_info);
  }
};

struct RelaMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Rela* to, Func func) {
    to->r_offset = func(from.r_offset);
    to->r_info   = func(from.r_info);
    to->r_addend = func(from.r_addend);
  }
};

struct NoteMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Nhdr* to, Func func) {
    to->n_namesz = func(from.n_namesz);
    to->n_descsz = func(from.n_descsz);
    to->n_type   = func(from.n_type);
  }
};

template <class T32, class T64, class Munger>
void ElfFile::StructReader::ReadFallback(uint64_t offset,
                                         absl::string_view* range,
                                         T64* out) const {
  if (elf_.is_64bit()) {
    assert(!elf_.is_native_endian());
    Memcpy(offset, range, out);
    Munger()(*out, out, ByteSwapFunc());
  } else {
    T32 data32;
    Memcpy(offset, range, &data32);
    if (elf_.is_native_endian()) {
      Munger()(data32, out, NullFunc());
    } else {
      Munger()(data32, out, ByteSwapFunc());
    }
  }
}

string_view ElfFile::Section::GetName() const {
  if (header_.sh_name == SHN_UNDEF) {
    return string_view(nullptr, 0);
  }
  return elf_->section_name_table_.ReadString(header_.sh_name);
}

string_view ElfFile::Section::ReadString(Elf64_Word index) const {
  assert(header().sh_type == SHT_STRTAB);

  if (index == SHN_UNDEF || index >= contents_.size()) {
    THROWF("can't read index $0 from strtab, total size is $1", index,
           contents_.size());
  }

  string_view ret = StrictSubstr(contents_, index);

  const char* null_pos =
      static_cast<const char*>(memchr(ret.data(), '\0', ret.size()));

  if (null_pos == NULL) {
    THROW("no NULL terminator found");
  }

  size_t len = null_pos - ret.data();
  ret = ret.substr(0, len);
  return ret;
}

Elf64_Word ElfFile::Section::GetEntryCount() const {
  if (header_.sh_entsize == 0) {
    THROW("sh_entsize is zero");
  }
  return contents_.size() / header_.sh_entsize;
}

void ElfFile::Section::ReadSymbol(Elf64_Word index, Elf64_Sym* sym,
                                  string_view* file_range) const {
  assert(header().sh_type == SHT_SYMTAB || header().sh_type == SHT_DYNSYM);
  size_t offset = header_.sh_entsize * index;
  elf_->ReadStruct<Elf32_Sym>(contents(), offset, SymMunger(), file_range, sym);
}

void ElfFile::Section::ReadRelocation(Elf64_Word index, Elf64_Rel* rel,
                                      string_view* file_range) const {
  assert(header().sh_type == SHT_REL);
  size_t offset = header_.sh_entsize * index;
  elf_->ReadStruct<Elf32_Rel>(contents(), offset, RelMunger(), file_range, rel);
}

void ElfFile::Section::ReadRelocationWithAddend(Elf64_Word index,
                                                Elf64_Rela* rela,
                                                string_view* file_range) const {
  assert(header().sh_type == SHT_RELA);
  size_t offset = header_.sh_entsize * index;
  elf_->ReadStruct<Elf32_Rela>(contents(), offset, RelaMunger(), file_range,
                               rela);
}

void ElfFile::NoteIter::Next() {
  if (remaining_.empty()) {
    done_ = true;
    return;
  }

  Elf_Note note;
  elf_->ReadStruct<Elf_Note>(remaining_, 0, NoteMunger(), nullptr, &note);

  // 32-bit and 64-bit note are the same size, so we don't have to treat
  // them separately when advancing.
  AdvancePastStruct<Elf_Note>(&remaining_);

  type_ = note.n_type;
  name_ = StrictSubstr(remaining_, 0, note.n_namesz);

  // Size might include NULL terminator.
  if (name_[name_.size() - 1] == 0) {
    name_ = name_.substr(0, name_.size() - 1);
  }

  remaining_ = StrictSubstr(remaining_, AlignUp(note.n_namesz, 4));
  descriptor_ = StrictSubstr(remaining_, 0, note.n_descsz);
  remaining_ = StrictSubstr(remaining_, AlignUp(note.n_descsz, 4));
}

bool ElfFile::Initialize() {
  if (data_.size() < EI_NIDENT) {
    return false;
  }

  unsigned char ident[EI_NIDENT];
  memcpy(ident, data_.data(), EI_NIDENT);

  if (memcmp(ident, "\177ELF", 4) != 0) {
    // Not an ELF file.
    return false;
  }

  switch (ident[EI_CLASS]) {
    case ELFCLASS32:
      is_64bit_ = false;
      break;
    case ELFCLASS64:
      is_64bit_ = true;
      break;
    default:
      THROWF("unexpected ELF class: $0", ident[EI_CLASS]);
  }

  switch (ident[EI_DATA]) {
    case ELFDATA2LSB:
      is_native_endian_ = IsLittleEndian();
      break;
    case ELFDATA2MSB:
      is_native_endian_ = !IsLittleEndian();
      break;
    default:
      THROWF("unexpected ELF data: $0", ident[EI_DATA]);
  }

  absl::string_view range;
  ReadStruct<Elf32_Ehdr>(entire_file(), 0, EhdrMunger(), &range, &header_);

  Section section0;
  bool has_section0 = 0;

  // ELF extensions: if certain fields overflow, we have to find their true data
  // from elsewhere.  For more info see:
  // https://docs.oracle.com/cd/E19683-01/817-3677/chapter6-94076/index.html
  if (header_.e_shoff > 0 &&
      data_.size() > (header_.e_shoff + header_.e_shentsize)) {
    section_count_ = 1;
    ReadSection(0, &section0);
    has_section0 = true;
  }

  section_count_ = header_.e_shnum;
  section_string_index_ = header_.e_shstrndx;

  if (section_count_ == 0 && has_section0) {
    section_count_ = section0.header().sh_size;
  }

  if (section_string_index_ == SHN_XINDEX && has_section0) {
    section_string_index_ = section0.header().sh_link;
  }

  header_region_ = GetRegion(0, header_.e_ehsize);
  section_headers_ = GetRegion(header_.e_shoff,
                               CheckedMul(header_.e_shentsize, section_count_));
  segment_headers_ = GetRegion(
      header_.e_phoff, CheckedMul(header_.e_phentsize, header_.e_phnum));

  if (section_count_ > 0) {
    ReadSection(section_string_index_, &section_name_table_);
    if (section_name_table_.header().sh_type != SHT_STRTAB) {
      THROW("section string index pointed to non-strtab");
    }
  }

  return true;
}

void ElfFile::ReadSegment(Elf64_Word index, Segment* segment) const {
  if (index >= header_.e_phnum) {
    THROWF("segment $0 doesn't exist, only $1 segments", index,
           header_.e_phnum);
  }

  Elf64_Phdr* header = &segment->header_;
  ReadStruct<Elf32_Phdr>(
      entire_file(),
      CheckedAdd(header_.e_phoff, CheckedMul(header_.e_phentsize, index)),
      PhdrMunger(), &segment->range_, header);
  segment->contents_ = GetRegion(header->p_offset, header->p_filesz);
}

void ElfFile::ReadSection(Elf64_Word index, Section* section) const {
  if (index >= section_count_) {
    THROWF("tried to read section $0, but there are only $1", index,
           section_count_);
  }

  Elf64_Shdr* header = &section->header_;
  ReadStruct<Elf32_Shdr>(
      entire_file(),
      CheckedAdd(header_.e_shoff, CheckedMul(header_.e_shentsize, index)),
      ShdrMunger(), &section->range_, header);

  if (header->sh_type == SHT_NOBITS) {
    section->contents_ = string_view();
  } else {
    section->contents_ = GetRegion(header->sh_offset, header->sh_size);
  }

  section->elf_ = this;
}

bool ElfFile::FindSectionByName(absl::string_view name, Section* section) const {
  for (Elf64_Word i = 0; i < section_count_; i++) {
    ReadSection(i, section);
    if (section->GetName() == name) {
      return true;
    }
  }
  return false;
}


// ArFile //////////////////////////////////////////////////////////////////////

// For parsing .a files (static libraries).
//
// The best documentation I've been able to find for this file format is
// Wikipedia: https://en.wikipedia.org/wiki/Ar_(Unix)
//
// So far we only parse the System V / GNU variant.

class ArFile {
 public:
  ArFile(string_view data)
      : magic_(StrictSubstr(data, 0, kMagicSize)),
        contents_(data.substr(std::min<size_t>(data.size(), kMagicSize))) {}

  bool IsOpen() const { return magic() == string_view(kMagic); }

  string_view magic() const { return magic_; }
  string_view contents() const { return contents_; }

  struct MemberFile {
    enum {
      kSymbolTable,        // Stores a symbol table.
      kLongFilenameTable,  // Stores long filenames, users should ignore.
      kNormal,             // Regular data file.
    } file_type;
    string_view filename;  // Only when file_type == kNormal
    size_t size;
    string_view header;
    string_view contents;
  };

  class MemberReader {
   public:
    MemberReader(const ArFile& ar) : remaining_(ar.contents()) {}
    bool ReadMember(MemberFile* file);
    bool IsEof() const { return remaining_.size() == 0; }

   private:
    string_view Consume(size_t n) {
      n = (n % 2 == 0 ? n : n + 1);
      if (remaining_.size() < n) {
        THROW("premature end of file");
      }
      string_view ret = remaining_.substr(0, n);
      remaining_.remove_prefix(n);
      return ret;
    }

    string_view long_filenames_;
    string_view remaining_;
  };

 private:
  const string_view magic_;
  const string_view contents_;

  static constexpr const char* kMagic = "!<arch>\n";
  static constexpr int kMagicSize = 8;
};

bool ArFile::MemberReader::ReadMember(MemberFile* file) {
  struct Header {
    char file_id[16];
    char modified_timestamp[12];
    char owner_id[6];
    char group_id[6];
    char mode[8];
    char size[10];
    char end[2];
  };

  if (remaining_.size() == 0) {
    return false;
  } else if (remaining_.size() < sizeof(Header)) {
    THROW("Premature EOF in AR data");
  }

  const Header* header = reinterpret_cast<const Header*>(remaining_.data());
  file->header = Consume(sizeof(Header));

  string_view file_id(&header->file_id[0], sizeof(header->file_id));
  string_view size_str(&header->size[0], sizeof(header->size));
  file->size = StringViewToSize(size_str);
  file->contents = Consume(file->size);
  file->file_type = MemberFile::kNormal;

  if (file_id[0] == '/') {
    // Special filename, internal to the format.
    if (file_id[1] == ' ') {
      file->file_type = MemberFile::kSymbolTable;
    } else if (file_id[1] == '/') {
      file->file_type = MemberFile::kLongFilenameTable;
      long_filenames_ = file->contents;
    } else if (isdigit(file_id[1])) {
      size_t offset = StringViewToSize(file_id.substr(1));
      size_t end = long_filenames_.find('/', offset);

      if (end == std::string::npos) {
        THROW("Unterminated long filename");
      }

      file->filename = long_filenames_.substr(offset, end - offset);
    } else {
      THROW("Unexpected special filename in AR archive");
    }
  } else {
    // Normal filename, slash-terminated.
    size_t slash = file_id.find('/');

    if (slash == std::string::npos) {
      THROW("BSD-style AR not yet implemented");
    }

    file->filename = file_id.substr(0, slash);
  }

  return true;
}

void MaybeAddFileRange(const char* analyzer, RangeSink* sink, string_view label,
                       string_view range) {
  if (sink) {
    sink->AddFileRange(analyzer, label, range);
  }
}

// Iterate over each ELF file, agnostic to whether it is inside a .a (AR) file
// or not.
template <class Func>
void ForEachElf(const InputFile& file, RangeSink* sink, Func func) {
  ArFile ar_file(file.data());
  unsigned long index_base = 0;

  if (ar_file.IsOpen()) {
    ArFile::MemberFile member;
    ArFile::MemberReader reader(ar_file);

    MaybeAddFileRange("ar_archive", sink, "[AR Headers]", ar_file.magic());

    while (reader.ReadMember(&member)) {
      MaybeAddFileRange("ar_archive", sink, "[AR Headers]", member.header);
      switch (member.file_type) {
        case ArFile::MemberFile::kNormal: {
          ElfFile elf(member.contents);
          if (elf.IsOpen()) {
            func(elf, member.filename, index_base);
            index_base += elf.section_count();
          } else {
            MaybeAddFileRange("ar_archive", sink, "[AR Non-ELF Member File]",
                              member.contents);
          }
          break;
        }
        case ArFile::MemberFile::kSymbolTable:
          MaybeAddFileRange("ar_archive", sink, "[AR Symbol Table]",
                            member.contents);
          break;
        case ArFile::MemberFile::kLongFilenameTable:
          MaybeAddFileRange("ar_archive", sink, "[AR Headers]",
                            member.contents);
          break;
      }
    }
  } else {
    ElfFile elf(file.data());
    if (!elf.IsOpen()) {
      THROWF("Not an ELF or Archive file: $0", file.filename());
    }

    func(elf, file.filename(), index_base);
  }
}

// For object files, addresses are relative to the section they live in, which
// is indicated by ndx.  We split this into:
//
// - 24 bits for index (up to 16M symbols with -ffunction-sections)
// - 40 bits for address (up to 1TB section)
static uint64_t ToVMAddr(size_t addr, long ndx, bool is_object) {
  if (is_object) {
    if (ndx >= 1 << 24) {
      THROW("ndx overflow: too many sections");
    }
    if (addr >= 1UL << 40) {
      THROW("address overflow: section too big");
    }
    return (ndx << 40) | addr;
  } else {
    return addr;
  }
}

static bool IsArchiveFile(string_view data) {
  ArFile ar(data);
  return ar.IsOpen();
}

static bool IsObjectFile(string_view data) {
  ElfFile elf(data);
  return IsArchiveFile(data) || (elf.IsOpen() && elf.header().e_type == ET_REL);
}

static void CheckNotObject(const char* source, RangeSink* sink) {
  if (IsObjectFile(sink->input_file().data())) {
    THROWF(
        "can't use data source '$0' on object files (only binaries and shared "
        "libraries)",
        source);
  }
}

static void ElfMachineToCapstone(Elf64_Half e_machine, cs_arch* arch,
                                 cs_mode* mode) {
  switch (e_machine) {
    case EM_386:
      *arch = CS_ARCH_X86;
      *mode = CS_MODE_32;
      break;
    case EM_X86_64:
      *arch = CS_ARCH_X86;
      *mode = CS_MODE_64;
      break;

    // These aren't tested, but we include them on the off-chance
    // that it will work.
    case EM_ARM:
      *arch = CS_ARCH_ARM;
      *mode = CS_MODE_LITTLE_ENDIAN;
      break;
    case EM_AARCH64:
      *arch = CS_ARCH_ARM64;
      *mode = CS_MODE_ARM;
      break;
    case EM_MIPS:
      *arch = CS_ARCH_MIPS;
      break;
    case EM_PPC:
      *arch = CS_ARCH_PPC;
      *mode = CS_MODE_32;
      break;
    case EM_PPC64:
      *arch = CS_ARCH_PPC;
      *mode = CS_MODE_64;
      break;
    case EM_SPARC:
      *arch = CS_ARCH_SPARC;
      *mode = CS_MODE_BIG_ENDIAN;
      break;
    case EM_SPARCV9:
      *arch = CS_ARCH_SPARC;
      *mode = CS_MODE_V9;
      break;
    default:
      THROWF("Unknown ELF machine value: $0'", e_machine);
  }
}

static void ReadElfArchMode(const InputFile& file, cs_arch* arch, cs_mode* mode) {
  ForEachElf(file, nullptr,
             [=](const ElfFile& elf, string_view /*filename*/,
                 uint32_t /*index_base*/) {
               // Last .o file wins?  (For .a files)?  It's kind of arbitrary,
               // but a single .a file shouldn't have multiple archs in it.
               ElfMachineToCapstone(elf.header().e_machine, arch, mode);
             });
}

static void ReadELFSymbols(const InputFile& file, RangeSink* sink,
                           SymbolTable* table, bool disassemble) {
  bool is_object = IsObjectFile(file.data());
  DisassemblyInfo info;
  DisassemblyInfo* infop = &info;
  ReadElfArchMode(file, &info.arch, &info.mode);

  ForEachElf(
      file, sink,
      [=](const ElfFile& elf, string_view /*filename*/, uint32_t index_base) {
        for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
          ElfFile::Section section;
          elf.ReadSection(i, &section);

          if (section.header().sh_type != SHT_SYMTAB) {
            continue;
          }

          Elf64_Word symbol_count = section.GetEntryCount();

          // Find the corresponding section where the strings for the symbol
          // table can be found.
          ElfFile::Section strtab_section;
          elf.ReadSection(section.header().sh_link, &strtab_section);
          if (strtab_section.header().sh_type != SHT_STRTAB) {
            THROW("symtab section pointed to non-strtab section");
          }

          for (Elf64_Word i = 1; i < symbol_count; i++) {
            Elf64_Sym sym;

            section.ReadSymbol(i, &sym, nullptr);

            if (ELF64_ST_TYPE(sym.st_info) == STT_SECTION) {
              continue;
            }

            if (sym.st_shndx == STN_UNDEF) {
              continue;
            }

            if (sym.st_size == 0) {
              // Maybe try to refine?  See ReadELFSectionsRefineSymbols below.
              continue;
            }

            string_view name = strtab_section.ReadString(sym.st_name);
            uint64_t full_addr =
                ToVMAddr(sym.st_value, index_base + sym.st_shndx, is_object);
            if (sink && !disassemble) {
              sink->AddVMRangeAllowAlias(
                  "elf_symbols", full_addr, sym.st_size,
                  ItaniumDemangle(name, sink->data_source()));
            }
            if (table) {
              table->insert(
                  std::make_pair(name, std::make_pair(full_addr, sym.st_size)));
            }
            if (disassemble && ELF64_ST_TYPE(sym.st_info) == STT_FUNC) {
              if (verbose_level > 1) {
                printf("Disassembling function: %s\n", name.data());
              }
              infop->text = sink->TranslateVMToFile(full_addr).substr(0, sym.st_size);
              infop->start_address = full_addr;
              DisassembleFindReferences(*infop, sink);
            }
          }
        }
      });
}

static void ReadELFSymbolTableEntries(const ElfFile& elf,
                                      const ElfFile::Section& section,
                                      uint64_t index_base, bool is_object,
                                      RangeSink* sink) {
  Elf64_Word symbol_count = section.GetEntryCount();

  // Find the corresponding section where the strings for the symbol
  // table can be found.
  ElfFile::Section strtab_section;
  elf.ReadSection(section.header().sh_link, &strtab_section);
  if (strtab_section.header().sh_type != SHT_STRTAB) {
    THROW("symtab section pointed to non-strtab section");
  }

  for (Elf64_Word i = 1; i < symbol_count; i++) {
    Elf64_Sym sym;
    string_view sym_range;
    section.ReadSymbol(i, &sym, &sym_range);

    if (ELF64_ST_TYPE(sym.st_info) == STT_SECTION ||
        sym.st_shndx == STN_UNDEF ||
        sym.st_name == SHN_UNDEF) {
      continue;
    }

    string_view name = strtab_section.ReadString(sym.st_name);
    uint64_t full_addr =
        ToVMAddr(sym.st_value, index_base + sym.st_shndx, is_object);
    // Capture the trailing NULL.
    name = string_view(name.data(), name.size() + 1);
    sink->AddFileRangeForVMAddr("elf_symtab_name", full_addr, name);
    sink->AddFileRangeForVMAddr("elf_symtab_sym", full_addr, sym_range);
  }
}

static void ReadELFRelaEntries(const ElfFile::Section& section,
                               uint64_t index_base, bool is_object,
                               RangeSink* sink) {
  Elf64_Word rela_count = section.GetEntryCount();
  Elf64_Word sh_info = section.header().sh_info;
  for (Elf64_Word i = 1; i < rela_count; i++) {
    Elf64_Rela rela;
    string_view rela_range;
    section.ReadRelocationWithAddend(i, &rela, &rela_range);
    uint64_t full_addr =
        ToVMAddr(rela.r_offset, index_base + sh_info, is_object);
    sink->AddFileRangeForVMAddr("elf_rela", full_addr, rela_range);
  }
}

// Adds file ranges for the symbol tables and string tables *themselves* (ie.
// the space that the symtab/strtab take up in the file).  This will cover
//   .symtab
//   .strtab
//   .dynsym
//   .dynstr
static void ReadELFTables(const InputFile& file, RangeSink* sink) {
  bool is_object = IsObjectFile(file.data());

  // Disassemble first, because sometimes other tables will refer to things we
  // discovered through disassembling.
  ReadELFSymbols(file, sink, nullptr, true);

  // Now scan other tables.
  ForEachElf(file, sink,
             [sink, is_object](const ElfFile& elf, string_view /*filename*/,
                               uint32_t index_base) {
               for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
                 ElfFile::Section section;
                 elf.ReadSection(i, &section);

                 switch (section.header().sh_type) {
                   case SHT_SYMTAB:
                   case SHT_DYNSYM:
                     ReadELFSymbolTableEntries(elf, section, index_base,
                                               is_object, sink);
                     break;
                   case SHT_RELA:
                     ReadELFRelaEntries(section, index_base, is_object, sink);
                     break;
                 }

                 // We are looking by section name, which is a little different
                 // than what the loader actually does (which is find
                 // eh_frame_hdr from the program headers and then find eh_frame
                 // fde entries from there). But these section names should be
                 // standard enough that this approach works also.
                 if (section.GetName() == ".eh_frame") {
                   ReadEhFrame(section.contents(), sink);
                 } else if (section.GetName() == ".eh_frame_hdr") {
                   ReadEhFrameHdr(section.contents(), sink);
                 }
               }
             });
}

enum ReportSectionsBy {
  kReportBySectionName,
  kReportByEscapedSectionName,
  kReportByFlags,
  kReportByArchiveMember,
};

static void DoReadELFSections(RangeSink* sink, enum ReportSectionsBy report_by) {
  bool is_object = IsObjectFile(sink->input_file().data());
  ForEachElf(
      sink->input_file(), sink,
      [=](const ElfFile& elf, string_view filename, uint32_t index_base) {
        std::string name_from_flags;
        for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
          ElfFile::Section section;
          elf.ReadSection(i, &section);
          string_view name = section.GetName();

          if (name.size() == 0) {
            return;
          }

          const auto& header = section.header();
          auto addr = header.sh_addr;
          auto size = header.sh_size;
          auto filesize = (header.sh_type == SHT_NOBITS) ? 0 : size;
          auto vmsize = (header.sh_flags & SHF_ALLOC) ? size : 0;

          string_view contents = StrictSubstr(section.contents(), 0, filesize);

          uint64_t full_addr = ToVMAddr(addr, index_base + i, is_object);

          if (report_by == kReportByFlags) {
            name_from_flags = std::string(name);

            name_from_flags = "Section [";

            if (header.sh_flags & SHF_ALLOC) {
              name_from_flags += 'A';
            }

            if (header.sh_flags & SHF_WRITE) {
              name_from_flags += 'W';
            }

            if (header.sh_flags & SHF_EXECINSTR) {
              name_from_flags += 'X';
            }

            name_from_flags += ']';
            sink->AddRange("elf_section", name_from_flags, full_addr, vmsize,
                           contents);
          } else if (report_by == kReportBySectionName) {
            sink->AddRange("elf_section", name, full_addr, vmsize, contents);
          } else if (report_by == kReportByEscapedSectionName) {
            if (!sink->IsBaseMap()) {
              sink->AddFileRangeForFileRange("elf_section", contents,
                                             section.range());
            }
            sink->AddRange("elf_section",
                           std::string("[section ") + std::string(name) + "]",
                           full_addr, vmsize, contents);
          } else if (report_by == kReportByArchiveMember) {
            sink->AddRange("elf_section", filename, full_addr, vmsize,
                           contents);
          }
        }

        if (report_by == kReportByArchiveMember) {
          // Cover unmapped parts of the file.
          sink->AddFileRange("unmapped_armember", filename, elf.entire_file());
        }
      });
}

enum ReportSegmentsBy {
  kReportBySegmentName,
  kReportByEscapedSegmentName,
};

static void DoReadELFSegments(RangeSink* sink, ReportSegmentsBy report_by) {
  ForEachElf(sink->input_file(), sink,
             [=](const ElfFile& elf, string_view /*filename*/,
                 uint32_t /*index_base*/) {
               for (Elf64_Xword i = 0; i < elf.header().e_phnum; i++) {
                 ElfFile::Segment segment;
                 elf.ReadSegment(i, &segment);
                 const auto& header = segment.header();

                 if (header.p_type != PT_LOAD) {
                   continue;
                 }

                 // Include the segment index in the label, to support embedded.
                 //
                 // Including the index in the segment label differentiates
                 // segments with the same access control (e.g. RWX vs RW). In
                 // ELF files built for embedded microcontroller projects, a
                 // segment is used for each distinct type of memory. In simple
                 // cases, there is a segment for the flash (which will store
                 // code and read-only data) and a segment for RAM (which
                 // usually stores globals, stacks, and maybe a heap). In more
                 // involved projects, there may be special segments for faster
                 // RAM (e.g. core coupled RAM or CCRAM), or there may even be
                 // memory overlays to support manual paging of code from flash
                 // (which may be slow) into RAM.
                 std::string name(absl::StrCat("LOAD #", i, " ["));

                 if (header.p_flags & PF_R) {
                   name += 'R';
                 }

                 if (header.p_flags & PF_W) {
                   name += 'W';
                 }

                 if (header.p_flags & PF_X) {
                   name += 'X';
                 }

                 name += ']';

                 if (report_by == kReportByEscapedSegmentName) {
                   name = absl::StrCat("[", name, "]");
                 }

                 sink->AddRange("elf_segment", name, header.p_vaddr,
                                header.p_memsz, segment.contents());
               }
             });
  ForEachElf(sink->input_file(), sink,
             [=](const ElfFile& elf, string_view /*filename*/,
                 uint32_t /*index_base*/) {
               for (Elf64_Xword i = 0; i < elf.header().e_phnum; i++) {
                 ElfFile::Segment segment;
                 elf.ReadSegment(i, &segment);
                 const auto& header = segment.header();
                 if(header.p_type != PT_TLS) continue;
                 std::string name = "TLS";
                 sink->AddRange("elf_segment", "TLS", header.p_vaddr, header.p_memsz,
                                segment.contents());
               }
             });
}

static void ReadELFSegments(RangeSink* sink) {
  if (IsObjectFile(sink->input_file().data())) {
    // Object files don't actually have segments.  But we can cheat a little bit
    // and make up "segments" based on section flags.  This can be really useful
    // when you are compiling with -ffunction-sections and -fdata-sections,
    // because in those cases the actual "sections" report becomes pretty
    // useless (since every function/data has its own section, it's like the
    // "symbols" report except less readable).
    DoReadELFSections(sink, kReportByFlags);
  } else {
    DoReadELFSegments(sink, kReportBySegmentName);
  }
}

// ELF files put debug info directly into the binary, so we call the DWARF
// reader directly on them.  At the moment we don't attempt to make these
// work with object files.

static void ReadDWARFSections(const InputFile& file, dwarf::File* dwarf) {
  ElfFile elf(file.data());
  assert(elf.IsOpen());
  for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
    ElfFile::Section section;
    elf.ReadSection(i, &section);
    string_view name = section.GetName();

    if (name == ".debug_aranges") {
      dwarf->debug_aranges = section.contents();
    } else if (name == ".debug_str") {
      dwarf->debug_str = section.contents();
    } else if (name == ".debug_info") {
      dwarf->debug_info = section.contents();
    } else if (name == ".debug_types") {
      dwarf->debug_types = section.contents();
    } else if (name == ".debug_abbrev") {
      dwarf->debug_abbrev = section.contents();
    } else if (name == ".debug_line") {
      dwarf->debug_line = section.contents();
    } else if (name == ".debug_loc") {
      dwarf->debug_loc = section.contents();
    } else if (name == ".debug_pubnames") {
      dwarf->debug_pubnames = section.contents();
    } else if (name == ".debug_pubtypes") {
      dwarf->debug_pubtypes = section.contents();
    } else if (name == ".debug_ranges") {
      dwarf->debug_ranges = section.contents();
    }
  }
}

void AddCatchAll(RangeSink* sink) {
  // The last-line fallback to make sure we cover the entire VM space.
  if (sink->data_source() != DataSource::kSegments) {
    DoReadELFSections(sink, kReportByEscapedSectionName);
  }
  DoReadELFSegments(sink, kReportByEscapedSegmentName);

  ForEachElf(sink->input_file(), sink,
             [sink](const ElfFile& elf, string_view /*filename*/,
                    uint32_t /*index_base*/) {
               sink->AddFileRange("elf_catchall", "[ELF Headers]",
                                  elf.header_region());
               sink->AddFileRange("elf_catchall", "[ELF Headers]",
                                  elf.section_headers());
               sink->AddFileRange("elf_catchall", "[ELF Headers]",
                                  elf.segment_headers());
             });

  // The last-line fallback to make sure we cover the entire file.
  sink->AddFileRange("elf_catchall", "[Unmapped]", sink->input_file().data());
}

class ElfObjectFile : public ObjectFile {
 public:
  ElfObjectFile(std::unique_ptr<InputFile> file)
      : ObjectFile(std::move(file)) {}

  std::string GetBuildId() const override {
    if (IsObjectFile(file_data().data())) {
      // Object files don't have a build ID.
      return std::string();
    }

    ElfFile elf(file_data().data());
    assert(elf.IsOpen());
    for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
      ElfFile::Section section;
      elf.ReadSection(i, &section);
      if (section.header().sh_type != SHT_NOTE) {
        continue;
      }

      for (ElfFile::NoteIter notes(section); !notes.IsDone(); notes.Next()) {
        if (notes.name() == "GNU" && notes.type() == NT_GNU_BUILD_ID) {
          return std::string(notes.descriptor());
        }
      }
    }

    // No build id section found.
    return std::string();
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) const override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
          ReadELFSegments(sink);
          break;
        case DataSource::kSections:
          DoReadELFSections(sink, kReportBySectionName);
          break;
        case DataSource::kRawSymbols:
        case DataSource::kShortSymbols:
        case DataSource::kFullSymbols:
          ReadELFSymbols(debug_file().file_data(), sink, nullptr, false);
          break;
        case DataSource::kArchiveMembers:
          DoReadELFSections(sink, kReportByArchiveMember);
          break;
        case DataSource::kCompileUnits: {
          CheckNotObject("compileunits", sink);
          SymbolTable symtab;
          DualMap symbol_map;
          NameMunger empty_munger;
          RangeSink symbol_sink(&debug_file().file_data(),
                                sink->options(),
                                DataSource::kRawSymbols,
                                &sinks[0]->MapAtIndex(0));
          symbol_sink.AddOutput(&symbol_map, &empty_munger);
          ReadELFSymbols(debug_file().file_data(), &symbol_sink, &symtab,
                         false);
          dwarf::File dwarf;
          ReadDWARFSections(debug_file().file_data(), &dwarf);
          ReadDWARFCompileUnits(dwarf, symtab, symbol_map, sink);
          break;
        }
        case DataSource::kInlines: {
          CheckNotObject("lineinfo", sink);
          dwarf::File dwarf;
          ReadDWARFSections(debug_file().file_data(), &dwarf);
          ReadDWARFInlines(dwarf, sink, true);
          DoReadELFSections(sink, kReportByEscapedSectionName);
          break;
        }
        default:
          THROW("unknown data source");
      }

      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
        case DataSource::kArchiveMembers:
          break;
        default:
          // Add these *after* processing all other data sources.
          ReadELFTables(sink->input_file(), sink);
          break;
      }

      AddCatchAll(sink);
    }
  }

  bool GetDisassemblyInfo(const absl::string_view symbol,
                          DataSource symbol_source,
                          DisassemblyInfo* info) const override {
    return DoGetDisassemblyInfo(&symbol, symbol_source, info);
  }

  bool DoGetDisassemblyInfo(const absl::string_view* symbol,
                            DataSource symbol_source,
                            DisassemblyInfo* info) const {
    // Find the corresponding file range.  This also could be optimized not to
    // build the entire map.
    DualMap base_map;
    NameMunger empty_munger;
    RangeSink base_sink(&file_data(), bloaty::Options(), DataSource::kSegments,
                        nullptr);
    base_sink.AddOutput(&base_map, &empty_munger);
    std::vector<RangeSink*> sink_ptrs{&base_sink};
    ProcessFile(sink_ptrs);

    // Could optimize this not to build the whole table if necessary.
    SymbolTable symbol_table;
    RangeSink symbol_sink(&file_data(), bloaty::Options(), symbol_source,
                          &base_map);
    symbol_sink.AddOutput(&info->symbol_map, &empty_munger);
    ReadELFSymbols(debug_file().file_data(), &symbol_sink, &symbol_table,
                   false);

    if (symbol) {
      auto entry = symbol_table.find(*symbol);
      if (entry == symbol_table.end()) {
        entry = symbol_table.find(ItaniumDemangle(*symbol, symbol_source));
        if (entry == symbol_table.end()) {
          return false;
        }
      }
      uint64_t vmaddr = entry->second.first;
      uint64_t size = entry->second.second;

      // TODO(haberman); Add PLT entries to symbol map, so call <plt stub> gets
      // symbolized.

      uint64_t fileoff;
      if (!base_map.vm_map.Translate(vmaddr, &fileoff)) {
        THROWF("Couldn't translate VM address for function $0", symbol);
      }

      info->text = StrictSubstr(file_data().data(), fileoff, size);
      info->start_address = vmaddr;
    }

    ReadElfArchMode(file_data(), &info->arch, &info->mode);
    return true;
  }
};

}  // namespace

std::unique_ptr<ObjectFile> TryOpenELFFile(std::unique_ptr<InputFile>& file) {
  ElfFile elf(file->data());
  ArFile ar(file->data());
  if (elf.IsOpen() || ar.IsOpen()) {
    return std::unique_ptr<ObjectFile>(new ElfObjectFile(std::move(file)));
  } else {
    return nullptr;
  }

  // A few functions that have been defined but are not yet used.
  (void)&ElfFile::FindSectionByName;
  (void)&ElfFile::Section::ReadRelocation;
}

}  // namespace bloaty
