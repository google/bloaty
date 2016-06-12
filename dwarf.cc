
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
      exit(1);
    }
  }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Attribute);

  Attribute() : attr_(nullptr) {}

  Dwarf_Debug dwarf_;
  Dwarf_Attribute attr_;
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
  void GetSrcfiles(std::vector<std::string>* out) const {
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
    Dwarf_Unsigned next_cu_offset;

    // This advances an internal cursor inside dwarf_ that will make the next
    // compilation unit the "current" one.
    int result = dwarf_next_cu_header(dwarf_, nullptr, nullptr, nullptr,
                                      nullptr, &next_cu_offset, &err);

    if (result == DW_DLV_NO_ENTRY) {
      eof_ = true;
    } else if (result == DW_DLV_ERROR) {
      DieWithDwarfError(err);
    } else {
      assert(result == DW_DLV_OK);
    }
  }

  CompilationUnitIterator begin();
  CompilationUnitIterator end();

  DIE GetCurrentCompilationUnit() {
    assert(!eof_);

    Dwarf_Die cu;
    Dwarf_Error err;

    int result = dwarf_siblingof(dwarf_, nullptr, &cu, &err);
    assert(result == DW_DLV_OK);

    return DIE(dwarf_, cu);
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

  for (auto& compilation_unit : reader) {
    std::vector<std::string> srcfiles;
    compilation_unit.GetSrcfiles(&srcfiles);

    for (auto& die : compilation_unit) {
      if (die.GetTag() == DW_TAG_subprogram) {
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
          } else {
            //std::cerr << "Didn't find object for linkage name: " << symname
            //          << "\n";
          }
        }
      }
    }
  }
}

} // namespace bloaty
