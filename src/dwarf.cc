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

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bloaty.h"
#include "dwarf_constants.h"
#include "re2/re2.h"

using namespace dwarf2reader;

static size_t AlignUpTo(size_t offset, size_t granularity) {
  // Granularity must be a power of two.
  return (offset + granularity - 1) & ~(granularity - 1);
}

namespace bloaty {
namespace dwarf {

int DivRoundUp(int n, int d) {
  return (n + (d - 1)) / d;
}

#define CHECK_RETURN(call) if (!(call)) { return false; }
#define CHECK_RETURN_STRINGPIECE(call) if (!(call)) { return StringPiece(); }


// Low-level Parsing Routines //////////////////////////////////////////////////

// For parsing the low-level values found in DWARF files.  These are the only
// routines that touch the bytes of the input buffer directly.  Everything else
// is layered on top of these.

template <class T>
bool ReadMemcpy(StringPiece* data, T* val) {
  CHECK_RETURN(data->size() >= sizeof(T));
  memcpy(val, data->data(), sizeof(T));
  data->remove_prefix(sizeof(T));
  return true;
}

bool ReadPiece(size_t bytes, StringPiece* data, StringPiece* val) {
  CHECK_RETURN(data->size() >= bytes);
  *val = data->substr(0, bytes);
  data->remove_prefix(bytes);
  return true;
}

bool SkipBytes(size_t bytes, StringPiece* data) {
  CHECK_RETURN(data->size() >= bytes);
  data->remove_prefix(bytes);
  return true;
}

bool ReadNullTerminated(StringPiece* data, StringPiece* val) {
  const char* nullz =
      static_cast<const char*>(memchr(data->data(), '\0', data->size()));

  // Return false if not NULL-terminated.
  CHECK_RETURN(nullz != NULL);

  size_t len = nullz - data->data();
  *val = data->substr(0, len);
  data->remove_prefix(len + 1);  // Remove NULL also.
  return true;
}

// Parses the LEB128 format defined by DWARF (both signed and unsigned
// versions).

template <class T>
typename std::enable_if<std::is_unsigned<T>::value, bool>::type ReadLEB128(
    StringPiece* data, T* out) {
  uint64_t ret = 0;
  int shift = 0;
  int maxshift = 70;
  const char* ptr = data->data();
  const char* limit = ptr + data->size();

  for (; ptr < limit && shift < maxshift; shift += 7) {
    char byte = *(ptr++);
    ret |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      data->remove_prefix(ptr - data->data());
      if (ret > std::numeric_limits<T>::max()) {
        fprintf(stderr,
                "DWARF data contained larger LEB128 than we were expecting.\n");
        return false;
      }
      *out = static_cast<T>(ret);
      return true;
    }
  }

  fprintf(stderr, "Corrupt DWARF data, unterminated LEB128.\n");
  return false;
}

template <class T>
typename std::enable_if<std::is_signed<T>::value, bool>::type ReadLEB128(
    StringPiece* data, T* out) {
  int64_t ret = 0;
  int shift = 0;
  int maxshift = 70;
  const char* ptr = data->data();
  const char* limit = ptr + data->size();

  while (ptr < limit && shift < maxshift) {
    char byte = *(ptr++);
    ret |= (byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) {
      data->remove_prefix(ptr - data->data());
      if (byte & 0x40) {
        ret |= -(1 << shift);
      }
      if (ret > std::numeric_limits<T>::max() ||
          ret < std::numeric_limits<T>::min()) {
        fprintf(stderr,
                "DWARF data contained larger LEB128 than we were expecting.\n");
        return false;
      }
      *out = ret;
      return true;
    }
  }

  fprintf(stderr, "Corrupt DWARF data, unterminated LEB128.\n");
  return false;
}

bool SkipLEB128(StringPiece* data) {
  size_t limit =
      std::min(static_cast<size_t>(data->size()), static_cast<size_t>(10));
  for (size_t i = 0; i < limit; i++) {
    if (((*data)[i] & 0x80) == 0) {
      data->remove_prefix(i + 1);
      return true;
    }
  }

  fprintf(stderr, "Corrupt DWARF data, unterminated LEB128.\n");
  return false;
}

// Some size information attached to each compilation unit.  The size of an
// address or offset in the DWARF data depends on this state which is parsed
// from the header.
struct CompilationUnitSizes {
  // When true, DWARF offsets are 64 bits, otherwise they are 32 bit.
  bool dwarf64;

  // The size of addresses.
  uint8_t address_size;

  // To allow this as the key in a map.
  bool operator<(const CompilationUnitSizes& rhs) const {
    return std::tie(dwarf64, address_size) <
           std::tie(rhs.dwarf64, rhs.address_size);
  }

  // Reads a DWARF offset based on whether we are reading dwarf32 or dwarf64
  // format.
  bool ReadDWARFOffset(StringPiece* data, uint64_t* ofs) const {
    if (dwarf64) {
      return ReadMemcpy(data, ofs);
    } else {
      uint32_t ofs32;
      CHECK_RETURN(ReadMemcpy(data, &ofs32));
      *ofs = ofs32;
      return true;
    }
  }

  // Reads an address according to the expected address_size.
  bool ReadAddress(StringPiece* data, uint64_t* addr) const {
    if (address_size == 8) {
      return ReadMemcpy(data, addr);
    } else if (address_size == 4) {
      uint32_t addr32;
      CHECK_RETURN(ReadMemcpy(data, &addr32));
      *addr = addr32;
      return true;
    } else {
      fprintf(stderr, "bloaty: unexpected address size: %d\n",
              static_cast<int>(address_size));
      return false;
    }
  }

  // Reads an "initial length" as specified in many DWARF headers.  This
  // contains either a 32-bit or a 64-bit length, and signals whether we are
  // using the 32-bit or 64-bit DWARF format (so it sets dwarf64 appropriately).
  //
  // Stores the range for this section in |data| and all of the remaining data
  // in |next|.
  bool ReadInitialLength(StringPiece* data, StringPiece* next) {
    uint64_t len;
    uint32_t len32;
    CHECK_RETURN(ReadMemcpy(data, &len32));

    if (len32 == 0xffffffff) {
      dwarf64 = true;
      CHECK_RETURN(ReadMemcpy(data, &len));
    } else {
      dwarf64 = false;
      len = len32;
    }

    CHECK_RETURN(data->size() >= len);

    if (next) *next = data->substr(len);
    data->remove_suffix(data->size() - len);
    return true;
  }
};


// AbbrevTable /////////////////////////////////////////////////////////////////

// Parses and stores a representation of (a portion of) the .debug_abbrev
// section of a DWARF file.  An abbreviation is defined by a unique "code"
// (unique within one table), and defines the DIE tag and set of attributes.
// The encoding of the DIE then contains just the abbreviation code and the
// attribute values -- thanks to the abbreviation table, the tag and attribute
// keys/names are not required.
//
// The abbreviations are an internal detail of the DWARF format and users should
// not need to care about them.

class AbbrevTable {
 public:
  // Reads abbreviations until a terminating abbreviation is seen.  Returns
  // false if there is a parse error or a premature EOF.
  bool ReadAbbrevs(StringPiece data);

  // In a DWARF abbreviation, each attribute has a name and a form.
  struct Attribute {
    uint16_t name;
    uint8_t form;
  };

  // The representation of a single abbreviation.
  struct Abbrev {
    uint32_t code;
    uint16_t tag;
    bool has_child;
    std::vector<Attribute> attr;
  };

  bool IsEmpty() const { return abbrev_.empty(); }

  // Looks for an abbreviation with the given code.  Returns true if the lookup
  // succeeded.
  bool GetAbbrev(uint32_t code, const Abbrev** abbrev) const {
    auto it = abbrev_.find(code);
    if (it != abbrev_.end()) {
      *abbrev = &it->second;
      return true;
    } else {
      return false;
    }
  }

 private:
  // Keyed by abbreviation code.
  // Generally we expect these to be small, so we could almost use a vector<>.
  // But you never know what crazy input data is going to do...
  std::unordered_map<uint32_t, Abbrev> abbrev_;
};

bool AbbrevTable::ReadAbbrevs(StringPiece data) {
  while (true) {
    uint32_t code;
    CHECK_RETURN(ReadLEB128(&data, &code));

    if (code == 0) {
      return true;  // Terminator entry.
    }

    Abbrev& abbrev = abbrev_[code];

    if (abbrev.code) {
      fprintf(stderr, "bloaty: DWARF data contained duplicate abbrev code.\n");
      return false;
    }

    uint8_t has_child;

    abbrev.code = code;
    CHECK_RETURN(ReadLEB128(&data, &abbrev.tag));
    CHECK_RETURN(ReadMemcpy(&data, &has_child));

    switch (has_child) {
      case DW_children_yes:
        abbrev.has_child = true;
        break;
      case DW_children_no:
        abbrev.has_child = false;
        break;
      default:
        return false;
    }

    while (true) {
      Attribute attr;
      CHECK_RETURN(ReadLEB128(&data, &attr.name));
      CHECK_RETURN(ReadLEB128(&data, &attr.form));

      if (attr.name == 0 && attr.form == 0) {
        break;  // End of this abbrev
      }

      abbrev.attr.push_back(attr);
    }
  }
}


// StringTable /////////////////////////////////////////////////////////////////

// Represents the .debug_str portion of a DWARF file and contains code for
// reading strings out of it.  This is an internal detail of the DWARF format
// and users should not need to care about it.

class StringTable {
 public:
  // Construct with the debug_str data from a DWARF file.
  StringTable(StringPiece debug_str) : debug_str_(debug_str) {}

  // Read a string from the table.
  bool ReadEntry(size_t ofs, StringPiece* val) const;

 private:
  StringPiece debug_str_;
};

bool StringTable::ReadEntry(size_t ofs, StringPiece* val) const {
  CHECK_RETURN(ofs < debug_str_.size());
  StringPiece str = debug_str_.substr(ofs);
  CHECK_RETURN(ReadNullTerminated(&str, val));
  return true;
}


// AddressRanges ///////////////////////////////////////////////////////////////

// Code for reading address ranges out of .debug_aranges.

class AddressRanges {
 public:
  AddressRanges(StringPiece data) : section_(data), next_unit_(data) {}

  // Offset into .debug_info for the current compilation unit.
  uint64_t debug_info_offset() { return debug_info_offset_; }

  // Address and length for this range.
  uint64_t address() { return address_; }
  uint64_t length() { return length_; }

  // Advance to the next range.  The values will be available in address() and
  // length().  Returns false when the end of this compilation unit is hit.
  // Must call this once before reading the first range.
  bool NextRange();

  // Advance to the next compilation unit.  The unit offset will be available in
  // debug_info_offset().  Must call this once before reading the first unit.
  bool NextUnit();

 private:
  CompilationUnitSizes sizes_;
  StringPiece section_;
  StringPiece unit_remaining_;
  StringPiece next_unit_;
  uint64_t debug_info_offset_;
  uint64_t address_;
  uint64_t length_;
};

bool AddressRanges::NextRange() {
  CHECK_RETURN(sizes_.ReadAddress(&unit_remaining_, &address_));
  CHECK_RETURN(sizes_.ReadAddress(&unit_remaining_, &length_));
  return true;
}

bool AddressRanges::NextUnit() {
  unit_remaining_ = next_unit_;
  CHECK_RETURN(sizes_.ReadInitialLength(&unit_remaining_, &next_unit_));

  uint16_t version;
  CHECK_RETURN(ReadMemcpy(&unit_remaining_, &version));

  if (version > 2) {
    fprintf(stderr, "bloaty: DWARF data is too new for us.\n");
    return false;
  }

  CHECK_RETURN(sizes_.ReadDWARFOffset(&unit_remaining_, &debug_info_offset_));

  uint8_t segment_size;

  CHECK_RETURN(ReadMemcpy(&unit_remaining_, &sizes_.address_size));
  CHECK_RETURN(ReadMemcpy(&unit_remaining_, &segment_size));

  if (segment_size) {
    fprintf(stderr,
            "bloaty: we don't know how to handle segmented addresses.\n");
    return false;
  }

  size_t ofs = unit_remaining_.data() - section_.data();
  size_t aligned_ofs = AlignUpTo(ofs, sizes_.address_size * 2);
  unit_remaining_.remove_prefix(aligned_ofs - ofs);

  return true;
}


// DIEReader ///////////////////////////////////////////////////////////////////

// Reads a sequence of DWARF DIE's (Debugging Information Entries) from the
// .debug_info or .debug_types section of a binary.
//
// Each DIE contains a tag and a set of attribute/value pairs.  We rely on the
// abbreviations in an AbbrevTable to decode the DIEs.

template <class T, class Enable = void>
class FormReader;

class DIEReader {
 public:
  // Constructs a new DIEReader.  Cannot be used until you call one of the
  // Seek() methods below.
  DIEReader(const File& file) : dwarf_(file) {}

  // Returns true if we are at the end of DIEs for the current depth and no
  // error occurred.
  bool IsEof() const { return state_ == State::kEof; }

  // Returns true if an error has occurred in reading.
  bool IsError() const { return state_ == State::kError; }

  // DIEs exist in both .debug_info and .debug_types.
  enum class Section {
    kDebugInfo,
    kDebugTypes
  };

  // Seeks to the overall start or the start of a specific compilation unit.
  // Note that |header_offset| is the offset of the compilation unit *header*,
  // not the offset of the first DIE.
  bool SeekToCompilationUnit(Section section, uint64_t header_offset);
  bool SeekToStart(Section section) {
    return SeekToCompilationUnit(section, 0);
  }

  bool NextCompilationUnit();

  // Advances to the next overall DIE, ignoring whether it happens to be a
  // child, a sibling, or an uncle/aunt.  Returns false at error or EOF.
  bool NextDIE();

  const AbbrevTable::Abbrev& GetAbbrev() const {
    assert(!IsEof());
    return *current_abbrev_;
  }

  // Returns the tag of the current DIE.
  // Requires that ReadCode() has been called at least once.
  uint16_t GetTag() const { return GetAbbrev().tag; }

  // Returns whether the current DIE has a child.
  // Requires that ReadCode() has been called at least once.
  bool HasChild() const { return GetAbbrev().has_child; }

  const File& dwarf() const { return dwarf_; }

  CompilationUnitSizes unit_sizes() const { return unit_sizes_; }
  uint32_t abbrev_version() const { return abbrev_version_; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(DIEReader);

  template<typename...> friend class FixedAttrReader;

  // APIs for our friends to use to update our state.

  // Call to get the current read head where attributes should be parsed.
  StringPiece ReadAttributesBegin() {
    assert(state_ == State::kReadyToReadAttributes);
    return remaining_;
  }

  // When some data has been parsed, this updates our read head.
  bool ReadAttributesEnd(StringPiece remaining, uint64_t sibling) {
    assert(state_ == State::kReadyToReadAttributes);
    if (remaining.data() == nullptr) {
      state_ = State::kError;
      return false;
    } else {
      remaining_ = remaining;
      sibling_offset_ = sibling;
      state_ = State::kReadyToNext;
      return true;
    }
  }

  // Internal APIs.

  bool ReadCompilationUnitHeader(StringPiece data);
  bool ReadCode();

  enum class State {
    kReadyToReadAttributes,
    kReadyToNext,
    kEof,
    kError
  } state_;

  std::string error_;

  const File& dwarf_;

  // Abbreviation for the current entry.
  const AbbrevTable::Abbrev* current_abbrev_;

  // Our current read position.
  StringPiece remaining_;
  uint64_t sibling_offset_;

  // The read position of the next entry at each level, or size()==0 for levels
  // where we don't know (because we're not at the top-level and the previous
  // DIE didn't include DW_AT_sibling).  Length of this array indicates the
  // current depth.
  StringPiece next_unit_;

  // All of the AbbrevTables we've read from .debug_abbrev, indexed by their
  // offset within .debug_abbrev.
  std::unordered_map<uint64_t, AbbrevTable> abbrev_tables_;

  // Whether we are in .debug_types or .debug_info.
  Section section_;

  // Information about the current compilation unit.
  StringPiece unit_data_;
  CompilationUnitSizes unit_sizes_;
  AbbrevTable* unit_abbrev_;

  // A small integer that uniquely identifies the combination of unit_abbrev_
  // and unit_sizes_.  Attribute readers use this to know when they can reuse an
  // existing (abbrev code) -> (Actions) mapping, since this table depends on
  // both the current abbrev. table and the sizes.
  uint32_t abbrev_version_;

  std::map<std::pair<AbbrevTable*, CompilationUnitSizes>, uint32_t>
      abbrev_versions_;

  // Only for .debug_types
  uint64_t unit_type_signature_;
  uint64_t unit_type_offset_;
};

bool DIEReader::ReadCode() {
  uint32_t code;
  state_ = State::kError;
  StringPiece data = remaining_;

  CHECK_RETURN(ReadLEB128(&data, &code));

  if (code == 0) {
    remaining_ = data;
    state_ = State::kEof;
    return false;
  } else {
    CHECK_RETURN(unit_abbrev_->GetAbbrev(code, &current_abbrev_));
    remaining_ = data;
    state_ = State::kReadyToReadAttributes;
    sibling_offset_ = 0;
    return true;
  }
}

bool DIEReader::NextCompilationUnit() {
  if (next_unit_.size() == 0) {
    state_ = State::kEof;
    return false;
  }

  CHECK_RETURN(ReadCompilationUnitHeader(next_unit_));
  CHECK_RETURN(ReadCode());
  return true;
}

bool DIEReader::NextDIE() {
  do {
    if (remaining_.size() == 0) {
      state_ = State::kEof;
      return false;
    }
    ReadCode();
  } while (state_ == State::kEof);

  return state_ == State::kReadyToReadAttributes;
}

bool DIEReader::SeekToCompilationUnit(Section section, uint64_t offset) {
  StringPiece data;
  section_ = section;

  if (section == Section::kDebugInfo) {
    data = dwarf_.debug_info;
  } else {
    data = dwarf_.debug_types;
  }

  CHECK_RETURN(offset < data.size());
  data.remove_prefix(offset);
  CHECK_RETURN(ReadCompilationUnitHeader(data));
  CHECK_RETURN(ReadCode());

  return true;
}

bool DIEReader::ReadCompilationUnitHeader(StringPiece data) {
  if (data.size() == 0) {
    state_ = State::kEof;
    return false;
  }

  StringPiece unit_data = data;
  StringPiece next_unit;
  unit_sizes_.ReadInitialLength(&data, &next_unit);

  uint16_t version;
  CHECK_RETURN(ReadMemcpy(&data, &version));

  if (version > 4) {
    fprintf(stderr, "Data is in new DWARF format we don't understand.\n");
    return false;
  }

  uint64_t debug_abbrev_offset;
  CHECK_RETURN(unit_sizes_.ReadDWARFOffset(&data, &debug_abbrev_offset));
  unit_abbrev_ = &abbrev_tables_[debug_abbrev_offset];

  // If we haven't already read abbreviations for this debug_abbrev_offset, we
  // need to do so now.
  if (unit_abbrev_->IsEmpty()) {
    StringPiece abbrev_data = dwarf_.debug_abbrev;
    abbrev_data.remove_prefix(debug_abbrev_offset);
    CHECK_RETURN(unit_abbrev_->ReadAbbrevs(abbrev_data));
  }

  CHECK_RETURN(ReadMemcpy(&data, &unit_sizes_.address_size));

  if (section_ == Section::kDebugTypes) {
    CHECK_RETURN(ReadMemcpy(&data, &unit_type_signature_));
    CHECK_RETURN(unit_sizes_.ReadDWARFOffset(&data, &unit_type_offset_));
  }

  unit_data_ = unit_data;
  remaining_ = data;
  next_unit_ = next_unit;

  auto abbrev_id = std::make_pair(unit_abbrev_, unit_sizes_);
  auto insert_pair = abbrev_versions_.insert(
      std::make_pair(abbrev_id, abbrev_versions_.size()));

  // This will be either the newly inserted value or the existing one, if there
  // was one.
  abbrev_version_ = insert_pair.first->second;

  return true;
}


// FormReader //////////////////////////////////////////////////////////////////

// A mapping of DWARF "forms" into C++ datatypes, and code to parse an attribute
// into those C++ types.  This is the main parsing code for parsing DIE
// attributes, and there's a lot going on here because DWARF specifies a lot of
// forms/encodings with ambiguous/overloaded semantics in some cases.
//
// Note that this code is only concerned with mapping DWARF data into C++.  It
// is not concerned with any possible *semantic* differences between the forms.
// For example, DW_FORM_block and DW_FORM_exprloc both represent delimited
// sections of the input, so this code treats them identically (both map to
// StringPiece) even though DW_FORM_exprloc carries extra semantic meaning about
// the *interpretation* of those bytes.

// The type of the decoding function yielded from all GetFunctionForForm()
// functions.  The return value indicates the data that remains after we parsed
// our value out.  If return_value.data() == nullptr, there was an error.
typedef StringPiece FormDecodeFunc(const DIEReader& reader, StringPiece data,
                                   void* val);

// Helper to get decoding function as a function pointer.
template <class T>
FormDecodeFunc* GetFormDecodeFunc(uint8_t form, CompilationUnitSizes sizes) {
  FormDecodeFunc* func = nullptr;
  FormReader<T>::GetFunctionForForm(sizes, form, [&func](FormDecodeFunc* f) {
    func = f;
    return true;
  });
  return func;
}

template <class Derived>
class FormReaderBase {
 public:
  FormReaderBase(const DIEReader& reader, StringPiece data)
      : reader_(reader), data_(data) {}

  StringPiece data() const { return data_; }

 protected:
  const DIEReader& reader_;
  StringPiece data_;

  // Function for parsing a specific, known form.  This function compiles into
  // extremely tight/optimized code for parsing this specific form into one
  // specific C++ type.
  template <bool (Derived::*mf)()>
  static StringPiece ReadAttr(const DIEReader& reader, StringPiece data,
                              void* val) {
    Derived form_reader(reader, data,
                        static_cast<typename Derived::type*>(val));
    if ((form_reader.*mf)() == false) { return StringPiece(); }
    return form_reader.data();
  }

  // Function for parsing the "indirect" form, which only gives you the concrete
  // form when you see the data.  This compiles into a switch() statement based
  // on the form we parse.
  static StringPiece ReadIndirect(const DIEReader& reader, StringPiece data,
                                  void* value) {
    uint16_t form;
    CHECK_RETURN_STRINGPIECE(ReadLEB128(&data, &form));
    CHECK_RETURN_STRINGPIECE(form != DW_FORM_indirect);
    bool ok = Derived::GetFunctionForForm(reader.unit_sizes(), form,
                                          [&](FormDecodeFunc* func) {
                                            data = func(reader, data, value);
                                            return data.data() != nullptr;
                                          });
    CHECK_RETURN_STRINGPIECE(ok);
    return data;
  }
};

// FormReader for StringPiece.  We accept the true string forms (DW_FORM_string
// and DW_FORM_strp) as well as a number of other forms that contain delimited
// string data.  We also accept the generic/opaque DW_FORM_data* types; the
// StringPiece can store the uninterpreted data which can then be interpreted by
// a higher layer.
template <>
class FormReader<StringPiece> : public FormReaderBase<FormReader<StringPiece>> {
 public:
  typedef FormReader ME;
  typedef FormReaderBase<ME> Base;
  typedef StringPiece type;
  using Base::data_;

  FormReader(const DIEReader& reader, StringPiece data, StringPiece* val)
      : Base(reader, data), val_(val) {}

  template <class Func>
  static bool GetFunctionForForm(CompilationUnitSizes sizes, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_block1:
        return func(&ReadAttr<&FormReader::ReadBlock<uint8_t>>);
      case DW_FORM_block2:
        return func(&ReadAttr<&FormReader::ReadBlock<uint16_t>>);
      case DW_FORM_block4:
        return func(&ReadAttr<&FormReader::ReadBlock<uint32_t>>);
      case DW_FORM_block:
      case DW_FORM_exprloc:
        return func(&ReadAttr<&FormReader::ReadVariableBlock>);
      case DW_FORM_string:
        return func(&ReadAttr<&FormReader::ReadString>);
      case DW_FORM_strp:
        if (sizes.dwarf64) {
          return func(&ReadAttr<&FormReader::ReadIndirectString<uint64_t>>);
        } else {
          return func(&ReadAttr<&FormReader::ReadIndirectString<uint32_t>>);
        }
      case DW_FORM_data1:
        return func(&ReadAttr<&FormReader::ReadFixed<1>>);
      case DW_FORM_data2:
        return func(&ReadAttr<&FormReader::ReadFixed<2>>);
      case DW_FORM_data4:
        return func(&ReadAttr<&FormReader::ReadFixed<4>>);
      case DW_FORM_data8:
        return func(&ReadAttr<&FormReader::ReadFixed<8>>);
      case DW_FORM_indirect:
        return func(&FormReader::ReadIndirect);
      default:
        return false;
    }
  }

 private:
  StringPiece* val_;

  template <size_t N>
  bool ReadFixed() {
    return ReadPiece(N, &data_, val_);
  }

  template <class D>
  bool ReadBlock() {
    D len;
    CHECK_RETURN(ReadMemcpy(&data_, &len));
    CHECK_RETURN(ReadPiece(len, &data_, val_));
    return true;
  }

  bool ReadVariableBlock() {
    uint64_t len;
    CHECK_RETURN(ReadLEB128(&data_, &len));
    CHECK_RETURN(ReadPiece(len, &data_, val_));
    return true;
  }

  bool ReadString() {
    return ReadNullTerminated(&data_, val_);
  }

  template <class D>
  bool ReadIndirectString() {
    D ofs;
    StringTable table(reader_.dwarf().debug_str);
    CHECK_RETURN(ReadMemcpy(&data_, &ofs));
    CHECK_RETURN(table.ReadEntry(ofs, val_));
    return true;
  }
};

// FormReader for all integral types.  We accept any DW_FORM_data* forms (sign
// or zero-extending as necessary), as well as the true integer and address
// types.
template <class T>
class FormReader<T, typename std::enable_if<std::is_integral<T>::value>::type>
    : public FormReaderBase<FormReader<T>> {
 public:
  typedef FormReader ME;
  typedef FormReaderBase<ME> Base;
  typedef T type;
  using Base::data_;

  FormReader(const DIEReader& reader, StringPiece data, T* val)
      : Base(reader, data), val_(val) {}

  template <class Func>
  static bool GetFunctionForForm(CompilationUnitSizes sizes, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_data1:
      case DW_FORM_ref1:
        return func(&Base::template ReadAttr<&ME::ReadFixed<int8_t>>);
      case DW_FORM_data2:
      case DW_FORM_ref2:
        CHECK_RETURN(sizeof(T) >= 2);
        return func(&Base::template ReadAttr<&ME::ReadFixed<int16_t>>);
      case DW_FORM_data4:
      case DW_FORM_ref4:
        CHECK_RETURN(sizeof(T) >= 4);
        return func(&Base::template ReadAttr<&ME::ReadFixed<int32_t>>);
      case DW_FORM_data8:
      case DW_FORM_ref8:
        CHECK_RETURN(sizeof(T) >= 8);
        return func(&Base::template ReadAttr<&ME::ReadFixed<int64_t>>);
      case DW_FORM_addr:
        // We require FORM_addr to be parsed into 8 bytes, since there is always
        // the possibility of running into 64-bit files.
        CHECK_RETURN(sizeof(T) >= 8);
        CHECK_RETURN(!std::is_signed<T>::value);
        if (sizes.address_size == 8) {
          return func(&Base::template ReadAttr<&ME::ReadFixed<int64_t>>);
        } else if (sizes.address_size == 4) {
          return func(&Base::template ReadAttr<&ME::ReadFixed<int32_t>>);
        } else {
          return false;
        }
        return true;
      case DW_FORM_sec_offset:
        // We require FORM_addr to be parsed into 8 bytes, since there is always
        // the possibility of running into 64-bit files.
        CHECK_RETURN(sizeof(T) >= 8);
        CHECK_RETURN(!std::is_signed<T>::value);
        if (sizes.dwarf64) {
          return func(&Base::template ReadAttr<&ME::ReadFixed<int64_t>>);
        } else {
          return func(&Base::template ReadAttr<&ME::ReadFixed<int32_t>>);
        }
      case DW_FORM_sdata:
        CHECK_RETURN(std::is_signed<T>::value);
        return func(&Base::template ReadAttr<&ME::ReadVariable>);
      case DW_FORM_udata:
        CHECK_RETURN(!std::is_signed<T>::value);
        return func(&Base::template ReadAttr<&ME::ReadVariable>);
      case DW_FORM_indirect:
        return func(&Base::ReadIndirect);
      default:
        return false;
    }
  }

 private:
  T* val_;

  template <class U>
  bool ReadFixed() {
    if (std::is_signed<T>::value) {
      // I don't know if this case exists or not in practice.  Do producers ever
      // ship a data1 that is meant to represent a signed number?
      typename std::make_signed<U>::type tmp;
      CHECK_RETURN(ReadMemcpy(&data_, &tmp));
      *val_ = tmp;
    } else {
      typename std::make_unsigned<U>::type tmp;
      CHECK_RETURN(ReadMemcpy(&data_, &tmp));
      *val_ = tmp;
    }
    return true;
  }

  bool ReadVariable() {
    return ReadLEB128(&data_, val_);
  }
};

// FormReader for bool.  The only types we expect for a bool field are
// DW_FORM_flag and DW_FORM_flag_present.
template <>
class FormReader<bool> : public FormReaderBase<FormReader<bool>> {
 public:
  typedef FormReader ME;
  typedef FormReaderBase<ME> Base;
  typedef bool type;
  using Base::data_;

  FormReader(const DIEReader& reader, StringPiece data, bool* val)
      : Base(reader, data), val_(val) {}

  template <class Func>
  static bool GetFunctionForForm(const DIEReader& /*reader*/, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_flag:
        return func(&Base::template ReadAttr<&FormReader::ReadFlag>);
      case DW_FORM_flag_present:
        return func(&Base::template ReadAttr<&FormReader::ReadFlagPresent>);
      case DW_FORM_indirect:
        return func(&ME::ReadIndirect);
      default:
        return false;
    }
  }

 private:
  bool* val_;

  bool ReadFlag() {
    uint8_t byte;
    CHECK_RETURN(ReadMemcpy(&data_, &byte));
    *val_ = byte;
    return true;
  }

  bool ReadFlagPresent() {
    *val_ = true;
    return true;
  }
};

// FormReader for void.  For skipping the data instead of reading it somewhere.
template <>
class FormReader<void> : public FormReaderBase<FormReader<void>> {
 public:
  typedef FormReader ME;
  typedef FormReaderBase<ME> Base;
  typedef void type;
  using Base::data_;

  FormReader(const DIEReader& reader, StringPiece data, void* /*val*/)
      : Base(reader, data) {}

  template <class Func>
  static bool GetFunctionForForm(CompilationUnitSizes sizes, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_flag_present:
        return func(&Base::template ReadAttr<&ME::DoNothing>);
      case DW_FORM_data1:
      case DW_FORM_ref1:
      case DW_FORM_flag:
        return func(&Base::template ReadAttr<&ME::SkipFixed<1>>);
      case DW_FORM_data2:
      case DW_FORM_ref2:
        return func(&Base::template ReadAttr<&ME::SkipFixed<2>>);
      case DW_FORM_data4:
      case DW_FORM_ref4:
        return func(&Base::template ReadAttr<&ME::SkipFixed<4>>);
      case DW_FORM_data8:
      case DW_FORM_ref8:
      case DW_FORM_ref_sig8:
        return func(&Base::template ReadAttr<&ME::SkipFixed<8>>);
      case DW_FORM_addr:
      case DW_FORM_ref_addr:
        if (sizes.address_size) {
          return func(&Base::template ReadAttr<&ME::SkipFixed<8>>);
        } else if (sizes.address_size == 4) {
          return func(&Base::template ReadAttr<&ME::SkipFixed<4>>);
        } else {
          return false;
        }
      case DW_FORM_sec_offset:
      case DW_FORM_strp:
        if (sizes.dwarf64) {
          return func(&Base::template ReadAttr<&ME::SkipFixed<8>>);
        } else {
          return func(&Base::template ReadAttr<&ME::SkipFixed<4>>);
        }
      case DW_FORM_sdata:
      case DW_FORM_udata:
      case DW_FORM_ref_udata:
        return func(&Base::template ReadAttr<&ME::SkipVariable>);
        return true;
      case DW_FORM_block1:
        return func(&Base::template ReadAttr<&ME::SkipBlock<uint8_t>>);
        return true;
      case DW_FORM_block2:
        return func(&Base::template ReadAttr<&ME::SkipBlock<uint16_t>>);
      case DW_FORM_block4:
        return func(&Base::template ReadAttr<&ME::SkipBlock<uint32_t>>);
      case DW_FORM_block:
      case DW_FORM_exprloc:
        return func(&Base::template ReadAttr<&ME::SkipVariableBlock>);
      case DW_FORM_string:
        return func(&Base::template ReadAttr<&ME::SkipString>);
      case DW_FORM_indirect:
        return func(&ME::ReadIndirect);
      default:
        return false;
    }
  }

 private:
  bool DoNothing() { return true; }

  template <size_t N>
  bool SkipFixed() {
    return SkipBytes(N, &data_);
  }

  bool SkipVariable() {
    return SkipLEB128(&data_);
  }

  template <class D>
  bool SkipBlock() {
    D len;
    CHECK_RETURN(ReadMemcpy(&data_, &len));
    CHECK_RETURN(SkipBytes(len, &data_));
    return true;
  }

  bool SkipVariableBlock() {
    uint64_t len;
    CHECK_RETURN(ReadLEB128(&data_, &len));
    CHECK_RETURN(SkipBytes(len, &data_));
    return true;
  }

  bool SkipString() {
    const char* nullz =
        static_cast<const char*>(memchr(data_.data(), '\0', data_.size()));
    CHECK_RETURN(nullz != NULL);  // String must be null terminated.
    CHECK_RETURN(SkipBytes(nullz - data_.data(), &data_));
    return true;
  }
};


// ActionBuf ///////////////////////////////////////////////////////////////////

// ActionBuf is an optimized list of decoding functions to call (and pointers to
// where to store the data) when a particular abbreviation is seen.  It is used
// by the attribute readers.

class ActionBuf {
 private:
  struct AttrAction {
    AttrAction(FormDecodeFunc* func_, void* data_, bool* has_)
        : func(func_), data(data_), has(has_) {}
    FormDecodeFunc* func;
    void* data;
    bool* has;
  };

  struct IndexedAction {
    IndexedAction(size_t index_, FormDecodeFunc* func_, void* data_, bool* has_)
        : index(index_), action(func_, data_, has_) {}
    size_t index;         // The index where this action should go.
    AttrAction action;    // The action, but func will be nullptr if invalid.
  };

 public:
  // Build a list of actions to perform for the given abbreviation in a
  // compilation unit with the given sizes.  Any attributes you want to parse
  // should be listed in "actions" (which came from calling GetAction()).
  ActionBuf(const AbbrevTable::Abbrev& abbrev, CompilationUnitSizes sizes,
            std::initializer_list<IndexedAction> actions);

  // For the given |attr_name| and destination type |T|, destination data |data|
  // and |has| bool locations, returns an action suitable for passing to the
  // ActionBuf constructor.
  template <class T>
  static IndexedAction GetAction(uint16_t attr_name,
                                 const AbbrevTable::Abbrev& abbrev,
                                 CompilationUnitSizes sizes, void* data,
                                 bool* has);

  StringPiece ReadAttributes(const DIEReader& reader, StringPiece data) const;

 private:
  std::vector<AttrAction> action_list_;
};

ActionBuf::ActionBuf(const AbbrevTable::Abbrev& abbrev,
                     CompilationUnitSizes sizes,
                     std::initializer_list<IndexedAction> indexed_actions) {
  // Initialize list with functions that will just skip the fields.
  for (size_t i = 0; i < abbrev.attr.size(); i++) {
    const auto& attr = abbrev.attr[i];
    auto func = GetFormDecodeFunc<void>(attr.form, sizes);

    if (!func) {
      fprintf(stderr, "bloaty: don't know how to skip DWARF form %d\n",
              attr.form);
      exit(1);
    }

    action_list_.push_back(AttrAction(func, nullptr, nullptr));
  }

  // Overwrite any entries for attributes we actually want to store somewhere.
  for (const auto& action : indexed_actions) {
    if (action.action.func) {
      assert(action.index < action_list_.size());
      if (action_list_[action.index].data) {
        fprintf(stderr,
                "bloaty: internal error, specified same DWARF attribute more "
                "than once\n");
        exit(1);
      }
      action_list_[action.index] = action.action;
    }
  }
}

template <class T>
ActionBuf::IndexedAction ActionBuf::GetAction(uint16_t attr_name,
                                              const AbbrevTable::Abbrev& abbrev,
                                              CompilationUnitSizes sizes,
                                              void* data, bool* has) {
  for (size_t i = 0; i < abbrev.attr.size(); i++) {
    if (attr_name == abbrev.attr[i].name) {
      FormDecodeFunc* func = GetFormDecodeFunc<T>(abbrev.attr[i].form, sizes);

      if (!func) {
        fprintf(stderr,
                "Warning: don't know how to convert form %d to type %s\n",
                abbrev.attr[i].form, typeid(T).name());
      }

      return IndexedAction(i, func, data, has);
    }
  }

  // This attribute doesn't occur.
  return IndexedAction(0, nullptr, nullptr, nullptr);
}

// The fast path function that reads all attributes by simply calling a list of
// function pointers to super-specialized functions.
StringPiece ActionBuf::ReadAttributes(const DIEReader& reader,
                                      StringPiece data) const {
  for (const auto& action : action_list_) {
    assert(action.func);
    data = action.func(reader, data, action.data);
    if (data.data() == nullptr) {
      return data;  // Propagate error.
    }
    if (action.has) {
      *action.has = true;
    }
  }
  return data;
}


// FixedAttrReader /////////////////////////////////////////////////////////////

// Parses a DIE's attributes into a tuple of values.  The user specifies the
// attributes they are expecting to see and the C++ types they want to parse
// into.  Any attributes that we don't list, or that have a type that doesn't
// fit our expected type, are skipped/ignored.  This is more convenient and more
// efficient than parsing all attributes into a generic representation and then
// selecting/converting them in a second phase.
//
// For the moment we don't distinguish between "data was not present" and "data
// was present but in a bad form."

template <class... Args>
class FixedAttrReader {
 public:
  typedef std::tuple<Args...> ValueTuple;

  // Constructs a decoder for the given attributes.  We will accept any
  // attribute forms that can decode to our target types (the template params on
  // this class).  If we want to be more restrictive about this later, we could
  // let users specify that only certain forms should be allowed.
  template <size_t N>
  FixedAttrReader(DIEReader* /*reader*/, const DwarfAttribute (&attributes)[N]) {
    static_assert(N == sizeof...(Args), "must match number of template params");
    std::copy(std::begin(attributes), std::end(attributes),
              std::begin(attributes_));
  }

  FixedAttrReader(DIEReader* /*reader*/,
                  std::initializer_list<DwarfAttribute> attributes) {
    assert(attributes.size() == sizeof...(Args));
    std::copy(std::begin(attributes), std::end(attributes),
              std::begin(attributes_));
  }

  // Returns true if the DIE's attributes were successfully parsed and all
  // expected attributes were present.  The values are available from values().
  //
  // If we wanted to allow some parameters to be optional, we could support
  // having params have an optional<> type.
  bool ReadAttributes(DIEReader* reader) {
    StringPiece data = reader->ReadAttributesBegin();

    // Clear all existing attributes.
    values_ = std::tuple<Args...>();
    memset(&has_attr_, 0, sizeof...(Args));

    // Parse all attributes.
    data = GetActionBuf(*reader).ReadAttributes(*reader, data);
    return reader->ReadAttributesEnd(data, 0);
  }

  template <size_t N>
  bool HasAttribute() const {
    static_assert(N < sizeof...(Args), "index too large");
    return has_attr_[N];
  }

  template <size_t N>
  typename std::tuple_element<N, ValueTuple>::type GetAttribute() const {
    return std::get<N>(values_);
  }

  const ValueTuple& values() const { return values_; }

 private:
  static const size_t kCount = sizeof...(Args);

  // Template to generate a compile-time sequence of integers, so we can do
  // "foreach element in the tuple".
  //
  // With C++14 we'll be able to use simple std:index_sequence instead of these
  // custom sequence-making templates.
  template <size_t... Indexes>
  struct IndexSequence {};

  template <size_t N, size_t... Indexes>
  struct MakeIndexSequence : MakeIndexSequence<N - 1, N - 1, Indexes...> {};

  template <size_t... Indexes>
  struct MakeIndexSequence<0, Indexes...> : IndexSequence<Indexes...> {};

  // Keyed by abbrev code, this stores a list of attribute actions and
  // associated data pointers.
  typedef std::unordered_map<uint32_t, ActionBuf> AbbrevCodeMap;

  const ActionBuf& GetActionBuf(const DIEReader& reader) {
    if (actions_.size() <= reader.abbrev_version()) {
      actions_.resize(reader.abbrev_version() + 1);
    }

    auto code = reader.GetAbbrev().code;
    auto& map = actions_[reader.abbrev_version()];
    auto it = map.find(code);
    auto sizes = reader.unit_sizes();

    if (it == map.end()) {
      return BuildActionBuf(reader.GetAbbrev(), sizes,
                            MakeIndexSequence<sizeof...(Args)>(), &map);
    } else {
      return it->second;
    }
  }

  template <size_t... I>
  const ActionBuf& BuildActionBuf(const AbbrevTable::Abbrev& abbrev,
                                  CompilationUnitSizes sizes,
                                  IndexSequence<I...>, AbbrevCodeMap* map);

  // Specifies for each attribute whether it was present or not.
  bool has_attr_[sizeof...(Args)];

  // The set of attributes we are expecting.
  DwarfAttribute attributes_[sizeof...(Args)];

  // The slots where we store the values we have parsed.
  ValueTuple values_;

  // We always store the sibling if we see one.
  uint64_t sibling_;

  // Indexed by DIEReader::abbrev_version(), so we have a different code map
  // when the abbreviation table or compilation unit sizes change.
  std::vector<AbbrevCodeMap> actions_;
};

template <class... Args>
template <size_t... I>
const ActionBuf& FixedAttrReader<Args...>::BuildActionBuf(
    const AbbrevTable::Abbrev& abbrev, CompilationUnitSizes sizes,
    FixedAttrReader<Args...>::IndexSequence<I...>, AbbrevCodeMap* map) {
  auto actions = {
      ActionBuf::GetAction<Args>(attributes_[I], abbrev, sizes,
                                 &std::get<I>(values_), &has_attr_[I])...,
      ActionBuf::GetAction<uint64_t>(DW_AT_sibling, abbrev, sizes, &sibling_,
                                     nullptr)};
  auto pair =
      map->emplace(std::piecewise_construct, std::make_tuple(abbrev.code),
                   std::make_tuple(abbrev, sizes, actions));

  // Must have inserted.
  assert(pair.second);
  return pair.first->second;
}


// LineInfoReader //////////////////////////////////////////////////////////////

// Code to read the .line_info programs in a DWARF file.

class LineInfoReader {
 public:
  LineInfoReader(const File& file) : file_(file), info_(0) {}

  struct LineInfo {
    LineInfo(bool default_is_stmt) : is_stmt(default_is_stmt) {}
    uint64_t address = 0;
    uint32_t file = 1;
    uint32_t line = 1;
    uint32_t column = 0;
    uint32_t discriminator = 0;
    bool end_sequence = false;
    bool basic_block = false;
    bool prologue_end = false;
    bool epilogue_begin = false;
    bool is_stmt;
    uint8_t op_index = 0;
    uint8_t isa = 0;
  };

  struct FileName {
    StringPiece name;
    uint32_t directory_index;
    uint64_t modified_time;
    uint64_t file_size;
  };

  bool SeekToOffset(uint64_t offset, uint8_t address_size);
  bool ReadLineInfo();
  const LineInfo& lineinfo() const { return info_; }
  const FileName& filename(size_t i) const { return file_names_[i]; }
  StringPiece include_directory(size_t i) const {
    return include_directories_[i];
  }

 private:
  struct Params {
    uint8_t minimum_instruction_length;
    uint8_t maximum_operations_per_instruction;
    uint8_t default_is_stmt;
    int8_t line_base;
    uint8_t line_range;
    uint8_t opcode_base;
  } params_;

  const File& file_;

  CompilationUnitSizes sizes_;
  std::vector<StringPiece> include_directories_;
  std::vector<FileName> file_names_;
  std::vector<uint8_t> standard_opcode_lengths_;

  StringPiece program_;
  StringPiece remaining_;

  // Whether we are in a "shadow" part of the bytecode program.  Sometimes parts
  // of the line info program make it into the final binary even though the
  // corresponding code was stripped.  We can tell when this happened by looking
  // for DW_LNE_set_address ops where the operand is 0.  This indicates that a
  // relocation for that argument never got applied, which probably means that
  // the code got stripped.
  //
  // While this is true, we don't yield any LineInfo entries, because the
  // "address" value is garbage.
  bool shadow_;

  LineInfo info_;

  void DoAdvance(uint64_t advance, uint8_t max_per_instr) {
    info_.address += params_.minimum_instruction_length *
                     ((info_.op_index + advance) / max_per_instr);
    info_.op_index = (info_.op_index + advance) % max_per_instr;
  }

  void Advance(uint64_t amount) {
    if (params_.maximum_operations_per_instruction == 1) {
      // This is by far the common case (only false on VLIW architectuers), and
      // this inlining/specialization avoids a costly division.
      DoAdvance(amount, 1);
    } else {
      DoAdvance(amount, params_.maximum_operations_per_instruction);
    }
  }

  uint8_t AdjustedOpcode(uint8_t op) { return op - params_.opcode_base; }

  void SpecialOpcodeAdvance(uint8_t op) {
    Advance(AdjustedOpcode(op) / params_.line_range);
  }
};

bool LineInfoReader::SeekToOffset(uint64_t offset, uint8_t address_size) {
  CHECK_RETURN(file_.debug_line.size() > offset);
  StringPiece data = file_.debug_line.substr(offset);
  program_ = data;

  uint16_t version;
  uint64_t header_length;
  sizes_.address_size = address_size;
  CHECK_RETURN(sizes_.ReadInitialLength(&data, nullptr));
  CHECK_RETURN(ReadMemcpy(&data, &version));
  CHECK_RETURN(sizes_.ReadDWARFOffset(&data, &header_length));

  StringPiece program = data.substr(header_length);

  CHECK_RETURN(ReadMemcpy(&data, &params_.minimum_instruction_length));
  if (version == 4) {
    CHECK_RETURN(
        ReadMemcpy(&data, &params_.maximum_operations_per_instruction));
  } else {
    params_.maximum_operations_per_instruction = 1;
  }
  CHECK_RETURN(ReadMemcpy(&data, &params_.default_is_stmt));
  CHECK_RETURN(ReadMemcpy(&data, &params_.line_base));
  CHECK_RETURN(ReadMemcpy(&data, &params_.line_range));
  CHECK_RETURN(ReadMemcpy(&data, &params_.opcode_base));

  standard_opcode_lengths_.resize(params_.opcode_base);
  for (size_t i = 1; i < params_.opcode_base; i++) {
    CHECK_RETURN(ReadMemcpy(&data, &standard_opcode_lengths_[i]));
  }

  // Read include_directories.
  include_directories_.clear();

  // Implicit current directory entry.
  include_directories_.push_back(StringPiece());

  while (true) {
    StringPiece dir;
    CHECK_RETURN(ReadNullTerminated(&data, &dir));
    if (dir.size() == 0) {
      break;
    }
    include_directories_.push_back(dir);
  }

  // Read file_names.
  file_names_.clear();

  // Filename 0 is unused.
  file_names_.push_back(FileName());
  while (true) {
    FileName file_name;
    CHECK_RETURN(ReadNullTerminated(&data, &file_name.name));
    if (file_name.name.size() == 0) {
      break;
    }
    CHECK_RETURN(ReadLEB128(&data, &file_name.directory_index));
    CHECK_RETURN(ReadLEB128(&data, &file_name.modified_time));
    CHECK_RETURN(ReadLEB128(&data, &file_name.file_size));
    file_names_.push_back(file_name);
  }

  info_ = LineInfo(params_.default_is_stmt);
  remaining_ = program;
  shadow_ = false;
  return true;
}

bool LineInfoReader::ReadLineInfo() {
  // Final step of last DW_LNS_copy / special opcode.
  info_.discriminator = 0;
  info_.basic_block = false;
  info_.prologue_end = false;
  info_.epilogue_begin = false;

  // Final step of DW_LNE_end_sequence.
  info_.end_sequence = false;

  StringPiece data = remaining_;

  while (true) {
    if (data.size() == 0) {
      remaining_ = data;
      return false;
    }

    uint8_t op;
    CHECK_RETURN(ReadMemcpy(&data, &op));

    if (op >= params_.opcode_base) {
      SpecialOpcodeAdvance(op);
      info_.line +=
          params_.line_base + (AdjustedOpcode(op) % params_.line_range);
      if (!shadow_) {
        remaining_ = data;
        return true;
      }
    } else {
      switch (op) {
        case DW_LNS_extended_op: {
          uint16_t len;
          uint8_t extended_op;
          CHECK_RETURN(ReadLEB128(&data, &len));
          CHECK_RETURN(ReadMemcpy(&data, &extended_op));
          switch (extended_op) {
            case DW_LNE_end_sequence: {
              // Preserve address and set end_sequence, but reset everything
              // else.
              uint64_t addr = info_.address;
              info_ = LineInfo(params_.default_is_stmt);
              info_.address = addr;
              info_.end_sequence = true;
              if (!shadow_) {
                remaining_ = data;
                return true;
              }
              break;
            }
            case DW_LNE_set_address:
              CHECK_RETURN(sizes_.ReadAddress(&data, &info_.address));
              info_.op_index = 0;
              shadow_ = (info_.address == 0);
              break;
            case DW_LNE_define_file: {
              FileName file_name;
              CHECK_RETURN(ReadNullTerminated(&data, &file_name.name));
              CHECK_RETURN(ReadLEB128(&data, &file_name.directory_index));
              CHECK_RETURN(ReadLEB128(&data, &file_name.modified_time));
              CHECK_RETURN(ReadLEB128(&data, &file_name.file_size));
              file_names_.push_back(file_name);
              break;
            }
            case DW_LNE_set_discriminator:
              CHECK_RETURN(ReadLEB128(&data, &info_.discriminator));
              break;
            default:
              // We don't understand this opcode, skip it.
              CHECK_RETURN(SkipBytes(len, &data));
              fprintf(stderr,
                      "bloaty: warning: unknown DWARF line table extended "
                      "opcode: %d\n",
                      extended_op);
              break;
          }
          break;
        }
        case DW_LNS_copy:
          if (!shadow_) {
            remaining_ = data;
            return true;
          }
          break;
        case DW_LNS_advance_pc: {
          uint64_t operand;
          CHECK_RETURN(ReadLEB128(&data, &operand));
          Advance(operand);
          break;
        }
        case DW_LNS_advance_line: {
          int32_t operand;
          CHECK_RETURN(ReadLEB128(&data, &operand));
          info_.line += operand;
          break;
        }
        case DW_LNS_set_file: {
          uint32_t operand;
          CHECK_RETURN(ReadLEB128(&data, &operand));
          info_.file = operand;
          break;
        }
        case DW_LNS_set_column: {
          uint32_t operand;
          CHECK_RETURN(ReadLEB128(&data, &operand));
          info_.column = operand;
          break;
        }
        case DW_LNS_negate_stmt:
          info_.is_stmt = !info_.is_stmt;
          break;
        case DW_LNS_set_basic_block:
          info_.basic_block = true;
          break;
        case DW_LNS_const_add_pc:
          SpecialOpcodeAdvance(255);
          break;
        case DW_LNS_fixed_advance_pc: {
          uint16_t operand;
          CHECK_RETURN(ReadMemcpy(&data, &operand));
          info_.address += operand;
          info_.op_index = 0;
          break;
        }
        case DW_LNS_set_prologue_end:
          info_.prologue_end = true;
          break;
        case DW_LNS_set_epilogue_begin:
          info_.epilogue_begin = true;
          break;
        case DW_LNS_set_isa:
          CHECK_RETURN(ReadLEB128(&data, &info_.isa));
          break;
        default:
          // Unknown opcode, but we know its length so can skip it.
          CHECK_RETURN(SkipBytes(standard_opcode_lengths_[op], &data));
          fprintf(stderr,
                  "bloaty: warning: unknown DWARF line table opcode: %d\n", op);
          break;
      }
    }
  }
}

} // namespace dwarf


// Bloaty DWARF Data Sources ///////////////////////////////////////////////////

// The DWARF .debug_aranges section should, in theory, give us exactly the
// information we need to map file ranges in linked binaries to compilation
// units from where that code came.  However, .debug_aranges is often incomplete
// or missing completely, so we use it as just one of several data sources for
// the "compileunits" data source.
static bool ReadDWARFAddressRanges(const dwarf::File& file, RangeSink* sink) {
  // Maps compilation unit offset -> source filename
  // Lazily initialized.
  class FilenameMap {
   public:
    FilenameMap(const dwarf::File& file)
        : die_reader_(file),
          attr_reader_(&die_reader_, {DW_AT_name}),
          missing_("[DWARF is missing filename]") {}

    std::string GetFilename(uint64_t compilation_unit_offset) {
      auto& name = map_[compilation_unit_offset];
      if (name.size() == 0) {
        name = LookupFilename(compilation_unit_offset);
      }
      return name;
    }

   private:
    std::string LookupFilename(uint64_t compilation_unit_offset) {
      auto section = dwarf::DIEReader::Section::kDebugInfo;
      if (die_reader_.SeekToCompilationUnit(section, compilation_unit_offset) &&
          die_reader_.GetTag() == DW_TAG_compile_unit &&
          attr_reader_.ReadAttributes(&die_reader_) &&
          attr_reader_.HasAttribute<0>()) {
        return attr_reader_.GetAttribute<0>().as_string();
      } else {
        return missing_;
      }
    }

    dwarf::DIEReader die_reader_;
    dwarf::FixedAttrReader<StringPiece> attr_reader_;
    std::unordered_map<uint64_t, std::string> map_;
    std::string missing_;
  } map(file);

  dwarf::AddressRanges ranges(file.debug_aranges);

  while (ranges.NextUnit()) {
    std::string filename = map.GetFilename(ranges.debug_info_offset());

    while (ranges.NextRange()) {
      sink->AddVMRangeIgnoreDuplicate(ranges.address(), ranges.length(),
                                      filename);
    }
  }

  return true;
}

// The DWARF debug info can help us get compileunits info.  DIEs for compilation
// units, functions, and global variables often have attributes that will
// resolve to addresses.
static bool ReadDWARFDebugInfo(const dwarf::File& file,
                               const SymbolTable& symtab, RangeSink* sink) {
  dwarf::DIEReader die_reader(file);
  dwarf::FixedAttrReader<StringPiece, StringPiece, uint64_t, uint64_t>
      attr_reader(&die_reader, {DW_AT_name, DW_AT_linkage_name, DW_AT_low_pc,
                                DW_AT_high_pc});

  CHECK_RETURN(die_reader.SeekToStart(dwarf::DIEReader::Section::kDebugInfo));

  do {
    CHECK_RETURN(attr_reader.ReadAttributes(&die_reader));
    std::string name = attr_reader.GetAttribute<0>().as_string();
    if (name.empty()) {
      continue;
    }

    do {
      uint64_t low_pc = attr_reader.GetAttribute<2>();
      uint64_t high_pc = attr_reader.GetAttribute<3>();

      if (attr_reader.HasAttribute<2>() && attr_reader.HasAttribute<3>()) {
        sink->AddVMRangeIgnoreDuplicate(low_pc, high_pc - low_pc, name);
      }

      if (attr_reader.HasAttribute<1>()) {
        auto it = symtab.find(attr_reader.GetAttribute<1>());
        if (it != symtab.end()) {
          sink->AddVMRangeIgnoreDuplicate(it->second.first, it->second.second,
                                          name);
        }
      }
    } while (die_reader.NextDIE() && attr_reader.ReadAttributes(&die_reader));
  } while (die_reader.NextCompilationUnit());

  return die_reader.IsEof();
}

bool ReadDWARFCompileUnits(const dwarf::File& file, const SymbolTable& symtab,
                           RangeSink* sink) {
  if (!file.debug_info.size()) {
    fprintf(stderr, "bloaty: missing debug info\n");
    return false;
  }

  if (file.debug_aranges.size()) {
    CHECK_RETURN(ReadDWARFAddressRanges(file, sink));
  }

  CHECK_RETURN(ReadDWARFDebugInfo(file, symtab, sink));

  return true;
}

static std::string LineInfoKey(const std::string& file, uint32_t line,
                               bool include_line) {
  if (include_line) {
    return file + ":" + std::to_string(line);
  } else {
    return file;
  }
}

bool ReadDWARFInlines(const dwarf::File& file, RangeSink* sink,
                      bool include_line) {
  if (!file.debug_info.size() || !file.debug_line.size()) {
    fprintf(stderr, "bloaty: missing debug info\n");
    return false;
  }

  dwarf::DIEReader die_reader(file);
  dwarf::LineInfoReader line_info_reader(file);
  dwarf::FixedAttrReader<uint64_t> attr_reader(&die_reader, {DW_AT_stmt_list});
  std::unordered_map<uint64_t, std::string> map_;
  std::string missing_;

  class FilenameMap {
   public:
    FilenameMap(const dwarf::LineInfoReader& reader) : reader_(reader) {}
    const std::string& GetSourceFilename(size_t index) {
      auto& ret = filenames_[index];
      if (ret.empty()) {
        const dwarf::LineInfoReader::FileName& filename =
            reader_.filename(index);
        StringPiece directory =
            reader_.include_directory(filename.directory_index);
        ret = directory.as_string();
        if (!ret.empty()) {
          ret += "/";
        }
        ret += filename.name.as_string();
      }
      return ret;
    }

   private:
    const dwarf::LineInfoReader& reader_;
    std::unordered_map<uint32_t, std::string> filenames_;
  };

  die_reader.SeekToStart(dwarf::DIEReader::Section::kDebugInfo);

  while (true) {
    CHECK_RETURN(attr_reader.ReadAttributes(&die_reader));
    FilenameMap map(line_info_reader);

    if (!attr_reader.HasAttribute<0>()) {
      continue;
    }

    CHECK_RETURN(line_info_reader.SeekToOffset(attr_reader.GetAttribute<0>(),
                 die_reader.unit_sizes().address_size));
    uint64_t span_startaddr = 0;
    std::string last_source;

    while (line_info_reader.ReadLineInfo()) {
      const auto& line_info = line_info_reader.lineinfo();
      auto addr = line_info.address;
      auto number = line_info.line;
      auto name = line_info.end_sequence
                      ? last_source
                      : LineInfoKey(map.GetSourceFilename(line_info.file),
                                    number, include_line);
      if (!span_startaddr) {
        span_startaddr = addr;
      } else if (line_info.end_sequence ||
          (!last_source.empty() && name != last_source)) {
        sink->AddVMRange(span_startaddr, addr - span_startaddr, last_source);
        if (line_info.end_sequence) {
          span_startaddr = 0;
        } else {
          span_startaddr = addr;
        }
      }
      last_source = name;
    }

    if (!die_reader.NextCompilationUnit()) {
      return die_reader.IsEof();
    }
  }

  return true;
}

} // namespace bloaty
