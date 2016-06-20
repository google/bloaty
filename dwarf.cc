
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

void GetFunctionFilePairs(const std::string& filename, ProgramDataSink* sink) {
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

  std::map<Dwarf_Off, std::vector<std::string>> all_srcfiles;

  for (auto& compilation_unit : reader) {
    std::vector<std::string>& srcfiles = all_srcfiles[compilation_unit.GetOffset()];
    compilation_unit.GetSrcfiles(&srcfiles);

    for (auto& die : compilation_unit) {
      Dwarf_Half tag = die.GetTag();

      if (tag == DW_TAG_subprogram || tag == DW_TAG_variable) {
        dwarf::Attribute linkage_name = die.GetAttribute(DW_AT_linkage_name);
        dwarf::Attribute decl_file = die.GetAttribute(DW_AT_decl_file);

        // For unknown reasons, the toolchain seems to sometimes emit this
        // older, non-standard attribute instead.
        if (linkage_name.is_null()) {
          linkage_name = die.GetAttribute(DW_AT_MIPS_linkage_name);
        }

        if (!linkage_name.is_null() && !decl_file.is_null()) {
          Dwarf_Unsigned u = decl_file.GetUnsigned();
          const auto& filename = srcfiles[u - 1];
          const auto symname = linkage_name.GetString();

          auto obj = sink->FindObjectByName(symname);

          if (obj) {
            auto file = sink->GetOrCreateFile(filename);
            obj->file->object_size -= obj->size;  // Should be the "no file".
            obj->file = file;
            file->object_size += obj->size;
            // We were successful at assigning a file.
            continue;
          } else {
            //std::cerr << "Didn't find object for linkage name: " << symname
            //          << "\n";
          }
        }
      }

      if (tag == DW_TAG_variable) {
        dwarf::Attribute location = die.GetAttribute(DW_AT_location);
        uintptr_t addr;
        if (location.is_null() ||
            location.GetForm() != DW_FORM_exprloc ||
            !dwarf::TryGetAddress(location.GetExpression(), &addr)) {
          continue;
        }
        auto obj = sink->FindObjectByAddr(addr);
        if (!obj) {
          continue;
        }
        dwarf::Attribute decl_file = die.GetAttribute(DW_AT_decl_file);
        dwarf::Attribute specification = die.GetAttribute(DW_AT_specification);
        const std::string* filename;

        if (!specification.is_null()) {
          Dwarf_Off off = specification.GetRef() + reader.GetCurrentCompilationUnitOffset();
          //std::cerr << "Looking up offset " << off << " from offset " << die.GetOffset() << " for symbol " << obj->name << " at address: " << obj->vmaddr << "\n";
          auto spec_die = reader.GetDIEAtOffset(off);
          if (spec_die.is_null()) {
            continue;
          }
          if (spec_die.GetTag() != DW_TAG_variable &&
              spec_die.GetTag() != DW_TAG_member) {
            std::cerr << "Tag was actually: " << spec_die.GetTag() << "\n";
          }
          assert(spec_die.GetTag() == DW_TAG_variable ||
                 spec_die.GetTag() == DW_TAG_member);
          decl_file = spec_die.GetAttribute(DW_AT_decl_file);

          if (decl_file.is_null()) {
            std::cerr << "Hmm, no file declared.\n";
            continue;
          }

          auto it = all_srcfiles.upper_bound(off);
          assert(it != all_srcfiles.begin());
          --it;
          std::vector<std::string>& cu_srcfiles = it->second;
          Dwarf_Unsigned idx = decl_file.GetUnsigned() - 1;
          if (idx >= cu_srcfiles.size()) {
            std::cerr << "(1) idx > size?  weird. " << idx << ", " << cu_srcfiles.size() << ", " << obj->name << "\n";
            continue;
          }
          filename = &cu_srcfiles[idx];
          //std::cerr << "Yay! Filename for " << obj->name << " is: " << *filename << "\n";
        } else {
          if (decl_file.is_null()) {
            continue;
          }
          Dwarf_Unsigned idx = decl_file.GetUnsigned() - 1;
          if (idx >= srcfiles.size()) {
            std::cerr << "(2) idx > size?  weird. " << idx << ", " << srcfiles.size() << ", " << obj->name << "\n";
            continue;
          }
          filename = &srcfiles[idx];
        }

        assert(filename);
        auto file = sink->GetOrCreateFile(*filename);
        obj->file->object_size -= obj->size;  // Should be the "no file".
        obj->file = file;
        file->object_size += obj->size;
      }

    }
  }

  reader.Reset();

  // Use line info to find files for any symbols we couldn't find DIEs for.
  // We use this as a last resort because it's less reliable.  If the first
  // instruction of a function was inlined from somewhere else, we'll report the
  // wrong filename for the function.
  for (auto& compilation_unit : reader) {
    dwarf::LineList lines = compilation_unit.GetLineList();
    for (size_t i = 0; i < lines.size(); i++) {
      auto obj = sink->FindObjectByAddr(lines.GetAddress(i));
      if (obj && obj->file->is_unknown) {
        auto file = sink->GetOrCreateFile(lines.GetSourceFilename(i));
        obj->file->object_size -= obj->size;  // Should be the "no file".
        obj->file = file;
        file->object_size += obj->size;
      }
    }
  }
}

} // namespace bloaty
