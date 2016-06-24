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

#include <assert.h>
#include <stdio.h>

#include <iostream>
#include <memory>
#include <stack>
#include <vector>

#include <dwarf.h>
#include <libdwarf.h>
#include <libelf.h>

#include "bloaty.h"
#include "re2/re2.h"

static size_t align_up_to(size_t offset, size_t granularity) {
  // Granularity must be a power of two.
  return (offset + granularity - 1) & ~(granularity - 1);
}


namespace bloaty {
namespace dwarf {

static void DieWithDwarfError(Dwarf_Error err) {
  std::cerr << "bloaty: fatal error reading DWARF debugging information: ";
  std::cerr << dwarf_errmsg(err) << "\n";
  abort();
}

// It would be nice to express the DWARF RAII pattern we're using here more
// explicitly.  I tried something like this but ran into more problems than I
// want to deal with right now:
//
// template <class T, int DEALLOC_TYPE>
// class Owned {
//  public:
//   Owned() : obj_(nullptr) {}
//
//   Owned(Dwarf_Debug dwarf, T obj)
//       : dwarf_(dwarf), obj_(obj) {}
//
//   ~Owned() {
//     if (obj_) {
//       dwarf_dealloc(dwarf_, obj_, DEALLOC_TYPE);
//     }
//   }
//
//   Owned(Owned&& other)
//       : dwarf_(other.dwarf_), obj_(other.obj_) {
//     other.obj_ = nullptr;
//   }
//
//   Owned& operator=(Owned&& other) {
//     if (obj_) {
//       dwarf_dealloc(dwarf_, obj_, DEALLOC_TYPE);
//     }
//
//     dwarf_ = other.dwarf_;
//     obj_ = other.obj_;
//     other.obj_ = nullptr;
//
//     return *this;
//   }
//
//   bool is_null() {
//     return obj_ == nullptr;
//   }
//
//  protected:
//   Dwarf_Debug dwarf_;
//   T obj_;
// };

class Attribute {
 public:
  Attribute(Dwarf_Debug dwarf, Dwarf_Attribute attr)
      : dwarf_(dwarf), attr_(attr) {}

  ~Attribute() {
    if (attr_) {
      dwarf_dealloc(dwarf_, attr_, DW_DLA_ATTR);
    }
  }

  Attribute(Attribute&& other)
      : dwarf_(other.dwarf_), attr_(other.attr_) {
    other.attr_ = nullptr;
  }

  Attribute& operator=(Attribute&& other) {
    if (attr_) {
      dwarf_dealloc(dwarf_, attr_, DW_DLA_ATTR);
    }

    dwarf_ = other.dwarf_;
    attr_ = other.attr_;
    other.attr_ = nullptr;

    return *this;
  }

  bool is_null() {
    return attr_ == nullptr;
  }

  static Attribute Null() {
    return Attribute();
  }

  Dwarf_Half GetForm() {
    Dwarf_Error err;
    Dwarf_Half ret;

    if (dwarf_whatform(attr_, &ret, &err) != DW_DLV_OK) {
      DieWithDwarfError(err);
      exit(1);
    }

    return ret;
  }

  std::string GetString() {
    char *str;
    Dwarf_Error err;

    if (dwarf_formstring(attr_, &str, &err) != DW_DLV_OK) {
      DieWithDwarfError(err);
      exit(1);
    }

    std::string ret(str);
    dwarf_dealloc(dwarf_, str, DW_DLA_STRING);

    return ret;
  }

  std::string GetExpression() {
    Dwarf_Unsigned len;
    Dwarf_Ptr ptr;
    Dwarf_Error err;

    if (dwarf_formexprloc(attr_, &len, &ptr, &err) != DW_DLV_OK) {
      std::cerr << "Unexpected form for expression data.\n";
      std::cerr << "Form is: " << GetForm() << "\n";
      exit(1);
    }

    return std::string(static_cast<char*>(ptr), len);
  }

  Dwarf_Unsigned GetUnsigned() {
    // DWARF doesn't specify for each attribute whether it will be encoded as
    // signed or unsigned, so we have to try both.
    Dwarf_Error err;
    Dwarf_Unsigned u;
    Dwarf_Signed s;

    if (dwarf_formudata(attr_, &u, &err) == DW_DLV_OK) {
      return u;
    } else if (dwarf_formsdata(attr_, &s, &err) == DW_DLV_OK) {
      if (s < 0) {
        std::cerr << "Unexpected value <0.\n";
        exit(1);
      }
      return static_cast<Dwarf_Unsigned>(s);
    } else {
      std::cerr << "Unexpected form for unsigned data.\n";
      std::cerr << "Form is: " << GetForm() << "\n";
      exit(1);
    }
  }

  Dwarf_Addr GetAddress() {
    Dwarf_Addr ret;
    Dwarf_Error err;

    if (dwarf_formaddr(attr_, &ret, &err) == DW_DLV_OK) {
      return ret;
    } else {
      std::cerr << "Unexpected form for address data.\n";
      std::cerr << "Form is: " << GetForm() << "\n";
      exit(1);
    }
  }

  Dwarf_Off GetRef() {
    Dwarf_Off off;
    Dwarf_Error err;

    if (dwarf_formref(attr_, &off, &err) == DW_DLV_OK) {
      return off;
    } else {
      std::cerr << "Unexpected form for ref data.\n";
      std::cerr << "Form is: " << GetForm() << "\n";
      exit(1);
    }
  }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Attribute);

  Attribute() : attr_(nullptr) {}

  Dwarf_Debug dwarf_;
  Dwarf_Attribute attr_;
};

class LineList {
 public:
  LineList(Dwarf_Debug dwarf, Dwarf_Line* line, Dwarf_Signed count)
      : dwarf_(dwarf), lines_(line), count_(count) {}

  LineList(LineList&& other) : dwarf_(other.dwarf_), lines_(other.lines_) {
    other.lines_ = nullptr;
  }

  LineList& operator=(LineList&& other) {
    Dealloc();

    dwarf_ = other.dwarf_;
    lines_ = other.lines_;
    other.lines_ = nullptr;

    return *this;
  }

  ~LineList() { Dealloc(); }

  static LineList Null() { return LineList(); }

  size_t size() const { return count_; }

  Dwarf_Addr GetAddress(size_t i) const {
    assert(i < size());

    Dwarf_Addr addr;
    Dwarf_Error err;

     if (dwarf_lineaddr(lines_[i], &addr, &err) != DW_DLV_OK) {
       DieWithDwarfError(err);
     }

     return addr;
  }

  std::string GetSourceFilename(size_t i) {
    assert(i < size());

    char *filename;
    Dwarf_Error err;

    if (dwarf_linesrc(lines_[i], &filename, &err) != DW_DLV_OK) {
       DieWithDwarfError(err);
       exit(1);
    }

    std::string ret(filename);
    dwarf_dealloc(dwarf_, filename, DW_DLA_STRING);
    return ret;
  }

 protected:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(LineList);

  LineList() : lines_(NULL), count_(0) {}

  void Dealloc() {
    if (lines_) {
      dwarf_srclines_dealloc(dwarf_, lines_, count_);
    }
  }

  Dwarf_Debug dwarf_;
  Dwarf_Line* lines_;
  Dwarf_Signed count_;
};

class DIEFlatIterator;

class DIE {
 public:
  DIE(Dwarf_Debug dwarf, Dwarf_Die die) : dwarf_(dwarf), die_(die) {}

  DIE(DIE&& other)
      : dwarf_(other.dwarf_), die_(other.die_) {
    other.die_ = nullptr;
  }

  DIE& operator=(DIE&& other) {
    if (die_) {
      dwarf_dealloc(dwarf_, die_, DW_DLA_DIE);
    }

    dwarf_ = other.dwarf_;
    die_ = other.die_;
    other.die_ = nullptr;

    return *this;
  }

  ~DIE() {
    if (die_) {
      dwarf_dealloc(dwarf_, die_, DW_DLA_DIE);
    }
  }

  static DIE Null() { return DIE(); }

  Dwarf_Off GetOffset() const {
    Dwarf_Off off;
    Dwarf_Error err;

    if (dwarf_dieoffset(die_, &off, &err) != DW_DLV_OK) {
      DieWithDwarfError(err);
      exit(1);
    }

    return off;
  }

  DIE GetChild() const {
    Dwarf_Die child;
    Dwarf_Error err;

    int result = dwarf_child(die_, &child, &err);

    if (result == DW_DLV_NO_ENTRY) {
      child = nullptr;
    } else if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
    } else {
      assert(result == DW_DLV_OK);
    }

    return DIE(dwarf_, child);
  }

  DIE GetSibling() const {
    Dwarf_Die sibling;
    Dwarf_Error err;

    int result = dwarf_siblingof(dwarf_, die_, &sibling, &err);

    if (result == DW_DLV_NO_ENTRY) {
      sibling = nullptr;
    } else if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
    } else {
      assert(result == DW_DLV_OK);
    }

    return DIE(dwarf_, sibling);
  }

  // Treats this object like an iterator and replaces its value with its next
  // sibling.
  void Next() {
    *this = GetSibling();
  }

  bool operator!=(const DIE& other) const {
    return die_ != other.die_;
  }

  void operator++() { Next(); }

  DIEFlatIterator begin() const;
  DIEFlatIterator end() const;

  bool is_null() const { return die_ == nullptr; }

  Dwarf_Half GetTag() const {
    Dwarf_Half ret;
    Dwarf_Error err;

    if (dwarf_tag(die_, &ret, &err) != DW_DLV_OK) {
      DieWithDwarfError(err);
    }

    return ret;
  }

  bool HasAttribute(Dwarf_Half attr) const {
    Dwarf_Bool has;
    Dwarf_Error err;

    if (dwarf_hasattr(die_, attr, &has, &err) != DW_DLV_OK) {
      DieWithDwarfError(err);
    }

    return has;
  }

  Attribute GetAttribute(Dwarf_Half attr_key) const {
    Dwarf_Error err;
    Dwarf_Attribute attr;

    int result = dwarf_attr(die_, attr_key, &attr, &err);

    if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
      exit(1);
    } else if (result == DW_DLV_OK) {
      return Attribute(dwarf_, attr);
    } else {
      return Attribute::Null();
    }
  }

  // Only valid for DW_TAG_compile_unit DIEs.
  LineList GetLineList() const {
    assert(GetTag() == DW_TAG_compile_unit);

    Dwarf_Line *lines;
    Dwarf_Signed linecount;
    Dwarf_Error err;

    int result = dwarf_srclines(die_, &lines, &linecount, &err);

    if (result == DW_DLV_OK) {
      return LineList(dwarf_, lines, linecount);
    } else if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
      exit(1);
    } else {
      return LineList::Null();
    }
  }

  // Only valid for DW_TAG_compile_unit DIEs.
  void GetSrcfiles(std::vector<std::string>* out) const {
    assert(GetTag() == DW_TAG_compile_unit);

    char **srcfiles;
    Dwarf_Signed srccount;
    Dwarf_Error err;

    int result = dwarf_srcfiles(die_, &srcfiles, &srccount, &err);

    if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
    } else if (result == DW_DLV_OK) {
      for (Dwarf_Signed i = 0; i < srccount; i++) {
        out->push_back(srcfiles[i]);
        dwarf_dealloc(dwarf_, srcfiles[i], DW_DLA_STRING);
      }
      dwarf_dealloc(dwarf_, srcfiles, DW_DLA_LIST);
    }
  }

 protected:
  DIE() : die_(NULL) {}
  BLOATY_DISALLOW_COPY_AND_ASSIGN(DIE);

  Dwarf_Debug dwarf_;
  Dwarf_Die die_;
};

class DIEFlatIterator {
 public:
  DIEFlatIterator(DIE die) {
    if (!die.is_null()) {
      stack_.push(std::move(die));
    }
  }

  bool operator!=(const DIEFlatIterator& other) const {
    return stack_.empty() != other.stack_.empty();
  }

  void operator++() {
    assert(!stack_.empty());

    auto child = stack_.top().GetChild();
    ++stack_.top();

    if (!child.is_null()) {
      stack_.push(std::move(child));
    } else {
      while (!stack_.empty() && stack_.top().is_null()) {
        stack_.pop();
      }
    }
  }

  const DIE& operator*() const {
    assert(!stack_.empty());
    return stack_.top();
  }

  static DIEFlatIterator NullIterator() {
    return DIEFlatIterator();
  }

 private:
  DIEFlatIterator() {}

  std::stack<DIE> stack_;
};

DIEFlatIterator DIE::begin() const {
  return DIEFlatIterator(GetChild());
}

DIEFlatIterator DIE::end() const {
  return DIEFlatIterator::NullIterator();
}

class CompilationUnitIterator;

class AddressRange {
 public:
  AddressRange(Dwarf_Debug dwarf, Dwarf_Arange arange) : dwarf_(dwarf), arange_(arange) {}

  AddressRange(AddressRange&& other)
      : dwarf_(other.dwarf_), arange_(other.arange_) {
    other.arange_ = nullptr;
  }

  AddressRange& operator=(AddressRange&& other) {
    if (arange_) {
      dwarf_dealloc(dwarf_, arange_, DW_DLA_DIE);
    }

    dwarf_ = other.dwarf_;
    arange_ = other.arange_;
    other.arange_ = nullptr;

    return *this;
  }

  ~AddressRange() {
    if (arange_) {
      dwarf_dealloc(dwarf_, arange_, DW_DLA_ARANGE);
    }
  }

  static AddressRange Null() { return AddressRange(); }

  void GetInfo(Dwarf_Addr* start, Dwarf_Unsigned* length, Dwarf_Off *cu_die_offset) const {
    Dwarf_Error err;

    if (dwarf_get_arange_info(arange_, start, length, cu_die_offset, &err) !=
        DW_DLV_OK) {
      DieWithDwarfError(err);
      exit(1);
    }
  }

 protected:
  AddressRange() : arange_(NULL) {}
  BLOATY_DISALLOW_COPY_AND_ASSIGN(AddressRange);

  Dwarf_Debug dwarf_;
  Dwarf_Arange arange_;
};

// Type for reading all DWARF entries out of a file.
class Reader {
 public:
  Reader() {}
  ~Reader() { CloseIfOpen(); }

  bool Open(int fd) {
    Dwarf_Error err;
    int result = dwarf_init(fd, DW_DLC_READ, nullptr, nullptr, &dwarf_, &err);

    if (result == DW_DLV_OK) {
      NextCompilationUnit();
      return true;
    } else if (result == DW_DLV_NO_ENTRY) {
      // No DWARF debugging information present.
      return false;
    } else {
      DieWithDwarfError(err);
      return false;
    }
  }

  void NextCompilationUnit() {
    assert(!eof_);

    Dwarf_Error err;

    cu_offset_ = next_cu_offset_;

    // This advances an internal cursor inside dwarf_ that will make the next
    // compilation unit the "current" one.
    int result = dwarf_next_cu_header(dwarf_, nullptr, nullptr, nullptr,
                                      nullptr, &next_cu_offset_, &err);

    if (result == DW_DLV_NO_ENTRY) {
      eof_ = true;
    } else if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
    } else {
      assert(result == DW_DLV_OK);
    }
  }

  Dwarf_Unsigned GetCurrentCompilationUnitOffset() {
    assert(!eof_);
    return cu_offset_;
  }

  void Reset() {
    assert(eof_);
    eof_ = false;
    NextCompilationUnit();
  }

  CompilationUnitIterator begin();
  CompilationUnitIterator end();

  DIE GetCurrentCompilationUnit() {
    assert(!eof_);

    Dwarf_Die cu;
    Dwarf_Error err;

    int result = dwarf_siblingof(dwarf_, nullptr, &cu, &err);
    (void)result;
    assert(result == DW_DLV_OK);

    return DIE(dwarf_, cu);
  }

  DIE GetDIEAtOffset(Dwarf_Off off) {
    assert(off);
    Dwarf_Die die;
    Dwarf_Error err = 0;

    int result = dwarf_offdie(dwarf_, off, &die, &err);
    if (result == DW_DLV_OK) {
      return DIE(dwarf_, die);
    } else if (result == DW_DLV_NO_ENTRY) {
      std::cerr << "No DIE at this offset?\n";
      return DIE::Null();
    } else {
      std::cerr << "Couldn't get DIE at offset: " << off << "\n";
      DieWithDwarfError(err);
      exit(1);
    }
  }

  void GetAddressRanges(std::vector<AddressRange>* out) const {
    Dwarf_Arange *aranges;
    Dwarf_Signed arange_count;
    Dwarf_Error err;

    int result = dwarf_get_aranges(dwarf_, &aranges, &arange_count, &err);

    if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
    } else if (result == DW_DLV_OK) {
      for (Dwarf_Signed i = 0; i < arange_count; i++) {
        out->push_back(std::move(AddressRange(dwarf_, aranges[i])));
      }
      dwarf_dealloc(dwarf_, aranges, DW_DLA_LIST);
    }
  }

  bool eof() { return eof_; }

 private:
  void CloseIfOpen() {
    if (!dwarf_) return;

    Elf* elf;
    Dwarf_Error err;

    if (dwarf_get_elf(dwarf_, &elf, &err) != DW_DLV_OK) {
      elf = nullptr;
    }

    dwarf_finish(dwarf_, &err);

    if (elf) {
      // The libdwarf manual tells us to do this but it seems to cause an
      // infinite loop in libelf!
      // elf_end(elf);
    }

    dwarf_ = nullptr;
  }

  bool eof_ = false;
  Dwarf_Unsigned cu_offset_;
  Dwarf_Unsigned next_cu_offset_ = 0;
  Dwarf_Debug dwarf_ = nullptr;
};

class CompilationUnitIterator {
 public:
  CompilationUnitIterator(Reader* reader)
      : reader_(reader),
        current_(reader ? reader_->GetCurrentCompilationUnit()
                        : DIE::Null()) {}

  bool operator!=(const CompilationUnitIterator& other) const {
    // Hack for range-based for.
    return !reader_->eof();
  }

  void operator++() {
    reader_->NextCompilationUnit();

    if (reader_->eof()) {
      current_ = DIE::Null();
    } else {
      current_ = reader_->GetCurrentCompilationUnit();
    }
  }

  const DIE& operator*() const { return current_; }

 private:
  Reader* reader_;
  DIE current_;
};

CompilationUnitIterator Reader::begin() {
  return CompilationUnitIterator(this);
}

CompilationUnitIterator Reader::end() {
  return CompilationUnitIterator(nullptr);
}

bool TryGetAddress(const std::string& str, uintptr_t* addr) {
  if (str[0] == DW_OP_addr && str.size() == sizeof(addr) + 1) {
    memcpy(addr, str.c_str() + 1, sizeof(*addr));
    return true;
  } else {
    return false;
  }
}

} // namespace dwarf

void ReadDWARFSourceFiles(const std::string& filename, MemoryMap* map) {
  dwarf::Reader reader;
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Can't open: " << filename << "\n";
    exit(1);
  }

  if (!reader.Open(fileno(file))) {
    std::cerr << "No debug information: " << filename << "\n";
    exit(1);
  }

  RE2 pattern(R"(\.pb\.cc$)");

  // Maps compilation unit offset -> source filename
  std::map<Dwarf_Off, std::string> source_files;

  for (auto& compilation_unit : reader) {
    std::vector<std::string> srcfiles;
    dwarf::Attribute name = compilation_unit.GetAttribute(DW_AT_name);
    if (!name.is_null()) {
      std::string name_str = name.GetString();
      if (RE2::PartialMatch(name_str, pattern)) {
        name_str = "Protobuf generated code";
      }
      source_files[compilation_unit.GetOffset()] = name_str;
    }
  }

  std::vector<dwarf::AddressRange> ranges;
  reader.GetAddressRanges(&ranges);

  for (const auto& range : ranges) {
    Dwarf_Addr start;
    Dwarf_Unsigned length;
    Dwarf_Off cu_die_offset;
    range.GetInfo(&start, &length, &cu_die_offset);
    map->Add(start, align_up_to(length, 16), source_files[cu_die_offset]);
  }
}

void ReadDWARFLineInfo(const std::string& filename, MemoryMap* map) {
  dwarf::Reader reader;
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Can't open: " << filename << "\n";
    exit(1);
  }

  if (!reader.Open(fileno(file))) {
    std::cerr << "No debug information: " << filename << "\n";
    exit(1);
  }

  uintptr_t begin_addr = 0;
  uintptr_t last_addr = 0;
  std::string last_source;

  for (auto& compilation_unit : reader) {
    auto lines = compilation_unit.GetLineList();

    for (size_t i = 0; i < lines.size(); i++) {
      uintptr_t addr = lines.GetAddress(i);
      std::string name = lines.GetSourceFilename(i);
      if (last_addr == 0) {
        // We don't trust a new address until it is in a region that seems like
        // it could plausibly be mapped.  We could use actual load instructions
        // to make this heuristic more robust.
        if (addr > 0x10000) {
          begin_addr = addr;
          last_addr = addr;
          last_source = name;
        }
      } else {
        if (addr - last_addr > 0x10000) {
          map->Add(begin_addr, last_addr - begin_addr, last_source);
          begin_addr = addr;
        } else if (name != last_source) {
          map->Add(begin_addr, addr - begin_addr, last_source);
          begin_addr = addr;
        }
        last_source = name;
        last_addr = addr;
      }
    }
  }
}

} // namespace bloaty
