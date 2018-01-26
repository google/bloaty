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

#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "bloaty.h"
#include "dwarf_constants.h"
#include "re2/re2.h"

using namespace dwarf2reader;
using absl::string_view;

static size_t AlignUpTo(size_t offset, size_t granularity) {
  // Granularity must be a power of two.
  return (offset + granularity - 1) & ~(granularity - 1);
}

ABSL_ATTRIBUTE_NORETURN
static void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)

namespace bloaty {

extern int verbose_level;

namespace dwarf {

int DivRoundUp(int n, int d) {
  return (n + (d - 1)) / d;
}


// Low-level Parsing Routines //////////////////////////////////////////////////

// For parsing the low-level values found in DWARF files.  These are the only
// routines that touch the bytes of the input buffer directly.  Everything else
// is layered on top of these.

template <class T>
T ReadMemcpy(string_view* data) {
  T ret;
  if (data->size() < sizeof(T)) {
    THROW("premature EOF reading fixed-length DWARF data");
  }
  memcpy(&ret, data->data(), sizeof(T));
  data->remove_prefix(sizeof(T));
  return ret;
}

string_view ReadPiece(size_t bytes, string_view* data) {
  if(data->size() < bytes) {
    THROW("premature EOF reading variable-length DWARF data");
  }
  string_view ret = data->substr(0, bytes);
  data->remove_prefix(bytes);
  return ret;
}

void SkipBytes(size_t bytes, string_view* data) {
  if (data->size() < bytes) {
    THROW("premature EOF skipping DWARF data");
  }
  data->remove_prefix(bytes);
}

string_view ReadNullTerminated(string_view* data) {
  const char* nullz =
      static_cast<const char*>(memchr(data->data(), '\0', data->size()));

  // Return false if not NULL-terminated.
  if (nullz == NULL) {
    THROW("DWARF string was not NULL-terminated");
  }

  size_t len = nullz - data->data();
  string_view val = data->substr(0, len);
  data->remove_prefix(len + 1);  // Remove NULL also.
  return val;
}

void SkipNullTerminated(string_view* data) {
  const char* nullz =
      static_cast<const char*>(memchr(data->data(), '\0', data->size()));

  // Return false if not NULL-terminated.
  if (nullz == NULL) {
    THROW("DWARF string was not NULL-terminated");
  }

  size_t len = nullz - data->data();
  data->remove_prefix(len + 1);  // Remove NULL also.
}

// Parses the LEB128 format defined by DWARF (both signed and unsigned
// versions).
//
// Bloaty doesn't actually use any LEB128's for signed values at the moment.
// So while this attempts to implement the DWARF spec correctly with respect
// to signed values, this isn't actually tested/exercised right now.

uint64_t ReadLEB128Internal(bool is_signed, string_view* data) {
  uint64_t ret = 0;
  int shift = 0;
  int maxshift = 70;
  const char* ptr = data->data();
  const char* limit = ptr + data->size();

  while (ptr < limit && shift < maxshift) {
    char byte = *(ptr++);
    ret |= static_cast<uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) {
      data->remove_prefix(ptr - data->data());
      if (is_signed && (byte & 0x40)) {
        ret |= -(1ULL << shift);
      }
      return ret;
    }
  }

  THROW("corrupt DWARF data, unterminated LEB128");
}

template <typename T>
T ReadLEB128(string_view* data) {
  typedef typename std::conditional<std::is_signed<T>::value, int64_t,
                                    uint64_t>::type Int64Type;
  Int64Type val = ReadLEB128Internal(std::is_signed<T>::value, data);
  if (val > std::numeric_limits<T>::max() ||
      val < std::numeric_limits<T>::min()) {
    THROW("DWARF data contained larger LEB128 than we were expecting");
  }
  return static_cast<T>(val);
}

void SkipLEB128(string_view* data) {
  size_t limit =
      std::min(static_cast<size_t>(data->size()), static_cast<size_t>(10));
  for (size_t i = 0; i < limit; i++) {
    if (((*data)[i] & 0x80) == 0) {
      data->remove_prefix(i + 1);
      return;
    }
  }

  THROW("corrupt DWARF data, unterminated LEB128");
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
  uint64_t ReadDWARFOffset(string_view* data) const {
    if (dwarf64) {
      return ReadMemcpy<uint64_t>(data);
    } else {
      return ReadMemcpy<uint32_t>(data);
    }
  }

  // Reads an address according to the expected address_size.
  uint64_t ReadAddress(string_view* data) const {
    if (address_size == 8) {
      return ReadMemcpy<uint64_t>(data);
    } else if (address_size == 4) {
      return ReadMemcpy<uint32_t>(data);
    } else {
      THROWF("unexpected address size: $0", address_size);
    }
  }

  // Reads an "initial length" as specified in many DWARF headers.  This
  // contains either a 32-bit or a 64-bit length, and signals whether we are
  // using the 32-bit or 64-bit DWARF format (so it sets dwarf64 appropriately).
  //
  // Returns the range for this section and stores the remaining data
  // in |remaining|.
  string_view ReadInitialLength(string_view* remaining) {
    uint64_t len = ReadMemcpy<uint32_t>(remaining);

    if (len == 0xffffffff) {
      dwarf64 = true;
      len = ReadMemcpy<uint64_t>(remaining);
    } else {
      dwarf64 = false;
    }

    if (remaining->size() < len) {
      THROW("short DWARF compilation unit");
    }

    string_view unit = *remaining;
    unit.remove_suffix(remaining->size() - len);
    *remaining = remaining->substr(len);
    return unit;
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
  // Reads abbreviations until a terminating abbreviation is seen.
  string_view ReadAbbrevs(string_view data);

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

string_view AbbrevTable::ReadAbbrevs(string_view data) {
  while (true) {
    uint32_t code = ReadLEB128<uint32_t>(&data);

    if (code == 0) {
      return data;  // Terminator entry.
    }

    Abbrev& abbrev = abbrev_[code];

    if (abbrev.code) {
      THROW("DWARF data contained duplicate abbrev code");
    }

    uint8_t has_child;

    abbrev.code = code;
    abbrev.tag = ReadLEB128<uint16_t>(&data);
    has_child = ReadMemcpy<uint8_t>(&data);

    switch (has_child) {
      case DW_children_yes:
        abbrev.has_child = true;
        break;
      case DW_children_no:
        abbrev.has_child = false;
        break;
      default:
        THROW("DWARF has_child is neither true nor false.");
    }

    while (true) {
      Attribute attr;
      attr.name = ReadLEB128<uint16_t>(&data);
      attr.form = ReadLEB128<uint8_t>(&data);

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
  StringTable(string_view debug_str) : debug_str_(debug_str) {}

  // Read a string from the table.
  string_view ReadEntry(size_t ofs) const;

 private:
  string_view debug_str_;
};

string_view StringTable::ReadEntry(size_t ofs) const {
  string_view str = debug_str_;
  SkipBytes(ofs, &str);
  return ReadNullTerminated(&str);
}


// AddressRanges ///////////////////////////////////////////////////////////////

// Code for reading address ranges out of .debug_aranges.

class AddressRanges {
 public:
  AddressRanges(string_view data) : section_(data), next_unit_(data) {}

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
  string_view section_;
  string_view unit_remaining_;
  string_view next_unit_;
  uint64_t debug_info_offset_;
  uint64_t address_;
  uint64_t length_;
};

bool AddressRanges::NextRange() {
  if (unit_remaining_.empty()) {
    return false;
  }

  address_ = sizes_.ReadAddress(&unit_remaining_);
  length_ = sizes_.ReadAddress(&unit_remaining_);
  return true;
}

bool AddressRanges::NextUnit() {
  if (next_unit_.empty()) {
    return false;
  }

  unit_remaining_ = sizes_.ReadInitialLength(&next_unit_);
  uint16_t version = ReadMemcpy<uint16_t>(&unit_remaining_);

  if (version > 2) {
    THROW("DWARF data is too new for us");
  }

  debug_info_offset_ = sizes_.ReadDWARFOffset(&unit_remaining_);

  uint8_t segment_size;

  sizes_.address_size = ReadMemcpy<uint8_t>(&unit_remaining_);
  segment_size = ReadMemcpy<uint8_t>(&unit_remaining_);

  if (segment_size) {
    THROW("we don't know how to handle segmented addresses.");
  }

  size_t ofs = unit_remaining_.data() - section_.data();
  size_t aligned_ofs = AlignUpTo(ofs, sizes_.address_size * 2);
  SkipBytes(aligned_ofs - ofs, &unit_remaining_);
  return true;
}


// LocationList ////////////////////////////////////////////////////////////////

// Code for reading entries out of a location list.
// For the moment we only care about finding the bounds of a list given its
// offset, so we don't actually vend any of the data.

class LocationList {
 public:
  LocationList(CompilationUnitSizes sizes, string_view data)
      : sizes_(sizes), remaining_(data) {}

  const char* read_offset() const { return remaining_.data(); }
  bool NextEntry();

 private:
  CompilationUnitSizes sizes_;
  string_view remaining_;
};

bool LocationList::NextEntry() {
  uint64_t start, end;
  start = sizes_.ReadAddress(&remaining_);
  end = sizes_.ReadAddress(&remaining_);
  if (start == 0 && end == 0) {
    return false;
  } else if (start == UINT64_MAX ||
             (start == UINT32_MAX && sizes_.address_size == 4)) {
    // Base address selection, nothing more to do.
  } else {
    // Need to skip the location description.
    uint16_t length = ReadMemcpy<uint16_t>(&remaining_);
    SkipBytes(length, &remaining_);
  }
  return true;
}

string_view GetLocationListRange(CompilationUnitSizes sizes,
                                 string_view available) {
  LocationList list(sizes, available);
  while (list.NextEntry()) {}
  return available.substr(0, list.read_offset() - available.data());
}


// RangeList ///////////////////////////////////////////////////////////////////

// Code for reading entries out of a range list.
// For the moment we only care about finding the bounds of a list given its
// offset, so we don't actually vend any of the data.

class RangeList {
 public:
  RangeList(CompilationUnitSizes sizes, string_view data)
      : sizes_(sizes), remaining_(data) {}

  const char* read_offset() const { return remaining_.data(); }
  bool NextEntry();

 private:
  CompilationUnitSizes sizes_;
  string_view remaining_;
};

bool RangeList::NextEntry() {
  uint64_t start, end;
  start = sizes_.ReadAddress(&remaining_);
  end = sizes_.ReadAddress(&remaining_);
  if (start == 0 && end == 0) {
    return false;
  }
  return true;
}

string_view GetRangeListRange(CompilationUnitSizes sizes,
                              string_view available) {
  RangeList list(sizes, available);
  while (list.NextEntry()) {
  }
  return available.substr(0, list.read_offset() - available.data());
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

  // Returns true if we are at the end of DIEs for this compilation unit.
  bool IsEof() const { return state_ == State::kEof; }

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

  // Skips children of the current DIE, so that the next call to NextDIE()
  // will read the next sibling (or parent, if no sibling exists).
  bool SkipChildren();

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

  string_view unit_range() const { return unit_range_; }
  CompilationUnitSizes unit_sizes() const { return unit_sizes_; }
  uint32_t abbrev_version() const { return abbrev_version_; }
  uint64_t debug_abbrev_offset() const { return debug_abbrev_offset_; }

  // If both compileunit_name and strp_sink are set, this will automatically
  // call strp_sink->AddFileRange(compileunit_name, <string range>) for every
  // DW_FORM_strp attribute encountered.  These strings occur in the .debug_str
  // section.
  void set_compileunit_name(absl::string_view name) {
    unit_name_ = std::string(name);
  }
  void set_strp_sink(RangeSink* sink) { strp_sink_ = sink; }

  void AddIndirectString(string_view range) const {
    if (strp_sink_) {
      strp_sink_->AddFileRange("dwarf_strp", unit_name_, range);
    }
  }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(DIEReader);

  template<typename...> friend class FixedAttrReader;

  // APIs for our friends to use to update our state.

  // Call to get the current read head where attributes should be parsed.
  string_view ReadAttributesBegin() {
    assert(state_ == State::kReadyToReadAttributes);
    return remaining_;
  }

  // When some data has been parsed, this updates our read head.
  bool ReadAttributesEnd(string_view remaining, uint64_t sibling) {
    assert(state_ == State::kReadyToReadAttributes);
    if (remaining.data() == nullptr) {
      THROW("premature EOF reading DWARF attributes");
    } else {
      remaining_ = remaining;
      sibling_offset_ = sibling;
      state_ = State::kReadyToNext;
      return true;
    }
  }

  // Internal APIs.

  bool ReadCompilationUnitHeader();
  bool ReadCode();

  enum class State {
    kReadyToReadAttributes,
    kReadyToNext,
    kEof,
  } state_;

  std::string error_;

  const File& dwarf_;
  RangeSink* strp_sink_ = nullptr;

  // Abbreviation for the current entry.
  const AbbrevTable::Abbrev* current_abbrev_;

  // Our current read position.
  string_view remaining_;
  uint64_t sibling_offset_;
  int depth_ = 0;

  // Data for the next compilation unit.
  string_view next_unit_;

  // All of the AbbrevTables we've read from .debug_abbrev, indexed by their
  // offset within .debug_abbrev.
  std::unordered_map<uint64_t, AbbrevTable> abbrev_tables_;

  // Whether we are in .debug_types or .debug_info.
  Section section_;

  // Information about the current compilation unit.
  uint64_t debug_abbrev_offset_;
  std::string unit_name_;
  string_view unit_range_;
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
again:
  if (remaining_.empty()) {
    state_ = State::kEof;
    return false;
  }
  code = ReadLEB128<uint32_t>(&remaining_);
  if (code == 0) {
    // null entry terminates a chain of sibling entries.
    depth_--;
    goto again;
  }

  if (!unit_abbrev_->GetAbbrev(code, &current_abbrev_)) {
    THROW("couldn't find abbreviation for code");
  }
  state_ = State::kReadyToReadAttributes;
  sibling_offset_ = 0;

  if (HasChild()) {
    depth_++;
  }

  return true;
}

bool DIEReader::NextCompilationUnit() {
  return ReadCompilationUnitHeader();
}

bool DIEReader::NextDIE() {
  if (state_ == State::kEof) {
    return false;
  }

  assert(state_ == State::kReadyToNext);
  return ReadCode();
}

bool DIEReader::SeekToCompilationUnit(Section section, uint64_t offset) {
  section_ = section;

  if (section == Section::kDebugInfo) {
    next_unit_ = dwarf_.debug_info;
  } else {
    next_unit_ = dwarf_.debug_types;
  }

  SkipBytes(offset, &next_unit_);
  return ReadCompilationUnitHeader();
}

bool DIEReader::ReadCompilationUnitHeader() {
  if (next_unit_.empty()) {
    state_ = State::kEof;
    return false;
  }

  unit_range_ = next_unit_;
  remaining_ = unit_sizes_.ReadInitialLength(&next_unit_);
  unit_range_ = unit_range_.substr(
      0, remaining_.size() + (remaining_.data() - unit_range_.data()));

  uint16_t version = ReadMemcpy<uint16_t>(&remaining_);

  if (version > 4) {
    THROW("Data is in new DWARF format we don't understand");
  }

  debug_abbrev_offset_ = unit_sizes_.ReadDWARFOffset(&remaining_);
  unit_abbrev_ = &abbrev_tables_[debug_abbrev_offset_];

  // If we haven't already read abbreviations for this debug_abbrev_offset_, we
  // need to do so now.
  if (unit_abbrev_->IsEmpty()) {
    string_view abbrev_data = dwarf_.debug_abbrev;
    SkipBytes(debug_abbrev_offset_, &abbrev_data);
    unit_abbrev_->ReadAbbrevs(abbrev_data);
  }

  unit_sizes_.address_size = ReadMemcpy<uint8_t>(&remaining_);

  if (section_ == Section::kDebugTypes) {
    unit_type_signature_ = ReadMemcpy<uint64_t>(&remaining_);
    unit_type_offset_ = unit_sizes_.ReadDWARFOffset(&remaining_);
  }

  auto abbrev_id = std::make_pair(unit_abbrev_, unit_sizes_);
  auto insert_pair = abbrev_versions_.insert(
      std::make_pair(abbrev_id, abbrev_versions_.size()));

  // This will be either the newly inserted value or the existing one, if there
  // was one.
  abbrev_version_ = insert_pair.first->second;

  return ReadCode();
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
// string_view) even though DW_FORM_exprloc carries extra semantic meaning about
// the *interpretation* of those bytes.

// The type of the decoding function yielded from all GetFunctionForForm()
// functions.  The return value indicates the data that remains after we parsed
// our value out.
typedef string_view FormDecodeFunc(const DIEReader& reader, string_view data,
                                   void* val);

// Helper to get decoding function as a function pointer.
template <class T>
FormDecodeFunc* GetFormDecodeFunc(uint8_t form, CompilationUnitSizes sizes) {
  FormDecodeFunc* func = nullptr;
  FormReader<T>::GetFunctionForForm(sizes, form, [&func](FormDecodeFunc* f) {
    func = f;
  });
  return func;
}

template <class Derived>
class FormReaderBase {
 public:
  FormReaderBase(const DIEReader& reader, string_view data)
      : reader_(reader), data_(data) {}

  string_view data() const { return data_; }

 protected:
  const DIEReader& reader_;
  string_view data_;

  // Function for parsing a specific, known form.  This function compiles into
  // extremely tight/optimized code for parsing this specific form into one
  // specific C++ type.
  template <void (Derived::*mf)()>
  static string_view ReadAttr(const DIEReader& reader, string_view data,
                              void* val) {
    Derived form_reader(reader, data,
                        static_cast<typename Derived::type*>(val));
    (form_reader.*mf)();
    return form_reader.data();
  }

  // Function for parsing the "indirect" form, which only gives you the concrete
  // form when you see the data.  This compiles into a switch() statement based
  // on the form we parse.
  static string_view ReadIndirect(const DIEReader& reader, string_view data,
                                  void* value) {
    uint16_t form = ReadLEB128<uint16_t>(&data);
    if (form == DW_FORM_indirect) {
      THROW("indirect attribute has indirect form type");
    }
    Derived::GetFunctionForForm(
        reader.unit_sizes(), form,
        [&](FormDecodeFunc* func) { data = func(reader, data, value); });
    return data;
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

  FormReader(const DIEReader& reader, string_view data, void* /*val*/)
      : Base(reader, data) {}

  template <class Func>
  static void GetFunctionForForm(CompilationUnitSizes sizes, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_flag_present:
        func(&Base::template ReadAttr<&ME::DoNothing>);
        return;
      case DW_FORM_data1:
      case DW_FORM_ref1:
      case DW_FORM_flag:
        func(&Base::template ReadAttr<&ME::SkipFixed<1>>);
        return;
      case DW_FORM_data2:
      case DW_FORM_ref2:
        func(&Base::template ReadAttr<&ME::SkipFixed<2>>);
        return;
      case DW_FORM_data4:
      case DW_FORM_ref4:
        func(&Base::template ReadAttr<&ME::SkipFixed<4>>);
        return;
      case DW_FORM_data8:
      case DW_FORM_ref8:
      case DW_FORM_ref_sig8:
        func(&Base::template ReadAttr<&ME::SkipFixed<8>>);
        return;
      case DW_FORM_addr:
      case DW_FORM_ref_addr:
        if (sizes.address_size == 8) {
          func(&Base::template ReadAttr<&ME::SkipFixed<8>>);
        } else if (sizes.address_size == 4) {
          func(&Base::template ReadAttr<&ME::SkipFixed<4>>);
        } else {
          THROWF("don't know how to skip address size $0", sizes.address_size);
        }
        return;
      case DW_FORM_sec_offset:
        if (sizes.dwarf64) {
          func(&Base::template ReadAttr<&ME::SkipFixed<8>>);
        } else {
          func(&Base::template ReadAttr<&ME::SkipFixed<4>>);
        }
        return;
      case DW_FORM_strp:
        if (sizes.dwarf64) {
          func(&Base::template ReadAttr<&ME::SkipIndirectString<uint64_t>>);
        } else {
          func(&Base::template ReadAttr<&ME::SkipIndirectString<uint32_t>>);
        }
        return;
      case DW_FORM_sdata:
      case DW_FORM_udata:
      case DW_FORM_ref_udata:
        func(&Base::template ReadAttr<&ME::SkipVariable>);
        return;
      case DW_FORM_block1:
        func(&Base::template ReadAttr<&ME::SkipBlock<uint8_t>>);
        return;
      case DW_FORM_block2:
        func(&Base::template ReadAttr<&ME::SkipBlock<uint16_t>>);
        return;
      case DW_FORM_block4:
        func(&Base::template ReadAttr<&ME::SkipBlock<uint32_t>>);
        return;
      case DW_FORM_block:
      case DW_FORM_exprloc:
        func(&Base::template ReadAttr<&ME::SkipVariableBlock>);
        return;
      case DW_FORM_string:
        func(&Base::template ReadAttr<&ME::SkipString>);
        return;
      case DW_FORM_indirect:
        func(&ME::ReadIndirect);
        return;
      default:
        THROWF("don't know how to skip DWARF form $0", form);
    }
  }

 private:
  void DoNothing() {}

  template <size_t N>
  void SkipFixed() {
    SkipBytes(N, &data_);
  }

  void SkipVariable() {
    SkipLEB128(&data_);
  }

  template <class D>
  void SkipBlock() {
    D len = ReadMemcpy<D>(&data_);
    SkipBytes(len, &data_);
  }

  void SkipVariableBlock() {
    uint64_t len = ReadLEB128<uint64_t>(&data_);
    SkipBytes(len, &data_);
  }

  void SkipString() {
    SkipNullTerminated(&data_);
  }

  template <class D>
  void SkipIndirectString() {
    D ofs = ReadMemcpy<D>(&data_);
    StringTable table(reader_.dwarf().debug_str);
    string_view str = table.ReadEntry(ofs);
    reader_.AddIndirectString(str);
  }
};

// FormReader for string_view.  We accept the true string forms (DW_FORM_string
// and DW_FORM_strp) as well as a number of other forms that contain delimited
// string data.  We also accept the generic/opaque DW_FORM_data* types; the
// string_view can store the uninterpreted data which can then be interpreted by
// a higher layer.
template <>
class FormReader<string_view> : public FormReaderBase<FormReader<string_view>> {
 public:
  typedef FormReader ME;
  typedef FormReaderBase<ME> Base;
  typedef string_view type;
  using Base::data_;

  FormReader(const DIEReader& reader, string_view data, string_view* val)
      : Base(reader, data), val_(val) {}

  template <class Func>
  static void GetFunctionForForm(CompilationUnitSizes sizes, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_block1:
        func(&ReadAttr<&FormReader::ReadBlock<uint8_t>>);
        return;
      case DW_FORM_block2:
        func(&ReadAttr<&FormReader::ReadBlock<uint16_t>>);
        return;
      case DW_FORM_block4:
        func(&ReadAttr<&FormReader::ReadBlock<uint32_t>>);
        return;
      case DW_FORM_block:
      case DW_FORM_exprloc:
        func(&ReadAttr<&FormReader::ReadVariableBlock>);
        return;
      case DW_FORM_string:
        func(&ReadAttr<&FormReader::ReadString>);
        return;
      case DW_FORM_strp:
        if (sizes.dwarf64) {
          func(&ReadAttr<&FormReader::ReadIndirectString<uint64_t>>);
        } else {
          func(&ReadAttr<&FormReader::ReadIndirectString<uint32_t>>);
        }
        return;
      case DW_FORM_data1:
        func(&ReadAttr<&FormReader::ReadFixed<1>>);
        return;
      case DW_FORM_data2:
        func(&ReadAttr<&FormReader::ReadFixed<2>>);
        return;
      case DW_FORM_data4:
        func(&ReadAttr<&FormReader::ReadFixed<4>>);
        return;
      case DW_FORM_data8:
        func(&ReadAttr<&FormReader::ReadFixed<8>>);
        return;
      case DW_FORM_indirect:
        func(&FormReader::ReadIndirect);
        return;
      default:
        // Skip it.
        FormReader<void>::GetFunctionForForm(sizes, form, func);
        return;
    }
  }

 private:
  string_view* val_;

  template <size_t N>
  void ReadFixed() {
    *val_ = ReadPiece(N, &data_);
  }

  template <class D>
  void ReadBlock() {
    D len = ReadMemcpy<D>(&data_);
    *val_ = ReadPiece(len, &data_);
  }

  void ReadVariableBlock() {
    uint64_t len = ReadLEB128<uint64_t>(&data_);
    *val_ = ReadPiece(len, &data_);
  }

  void ReadString() {
    *val_ = ReadNullTerminated(&data_);
  }

  template <class D>
  void ReadIndirectString() {
    D ofs = ReadMemcpy<D>(&data_);
    StringTable table(reader_.dwarf().debug_str);
    *val_ = table.ReadEntry(ofs);
    reader_.AddIndirectString(*val_);
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

  FormReader(const DIEReader& reader, string_view data, T* val)
      : Base(reader, data), val_(val) {}

  template <class Func>
  static void GetFunctionForForm(CompilationUnitSizes sizes, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_data1:
      case DW_FORM_ref1:
        func(&Base::template ReadAttr<&ME::ReadFixed<int8_t>>);
        return;
      case DW_FORM_data2:
      case DW_FORM_ref2:
        if (sizeof(T) < 2) {
          THROW("can't fit data2/ref2 into this type");
        }
        func(&Base::template ReadAttr<&ME::ReadFixed<int16_t>>);
        return;
      case DW_FORM_data4:
      case DW_FORM_ref4:
        if (sizeof(T) < 4) {
          THROW("can't fit data4/ref4 into this type");
        }
        func(&Base::template ReadAttr<&ME::ReadFixed<int32_t>>);
        return;
      case DW_FORM_data8:
      case DW_FORM_ref8:
        if (sizeof(T) < 8) {
          THROW("can't fit data8/ref8 into this type");
        }
        func(&Base::template ReadAttr<&ME::ReadFixed<int64_t>>);
        return;
      case DW_FORM_addr:
        // We require FORM_addr to be parsed into 8 bytes, since there is always
        // the possibility of running into 64-bit files.
        if (sizeof(T) < 8 || std::is_signed<T>::value) {
          THROW("can't fit addr into this type");
        }
        if (sizes.address_size == 8) {
          func(&Base::template ReadAttr<&ME::ReadFixed<int64_t>>);
        } else if (sizes.address_size == 4) {
          func(&Base::template ReadAttr<&ME::ReadFixed<int32_t>>);
        } else {
          THROWF("don't know how to handle address size: $0",
                 sizes.address_size);
        }
        return;

      case DW_FORM_sec_offset:
        // We require FORM_addr to be parsed into 8 bytes, since there is always
        // the possibility of running into 64-bit files.
        if (sizeof(T) < 8 || std::is_signed<T>::value) {
          THROW("can't fit sec_offset into this type");
        }
        if (sizes.dwarf64) {
          func(&Base::template ReadAttr<&ME::ReadFixed<int64_t>>);
        } else {
          func(&Base::template ReadAttr<&ME::ReadFixed<int32_t>>);
        }
        return;
      case DW_FORM_sdata:
        if (!std::is_signed<T>::value) {
          THROW("sdata must be parsed into signed type");
        }
        func(&Base::template ReadAttr<&ME::ReadVariable>);
        return;
      case DW_FORM_udata:
        if (std::is_signed<T>::value) {
          THROW("udata must be parsed into unsigned type");
        }
        func(&Base::template ReadAttr<&ME::ReadVariable>);
        return;
      case DW_FORM_indirect:
        func(&Base::ReadIndirect);
        return;
      default:
        // Skip it.
        FormReader<void>::GetFunctionForForm(sizes, form, func);
        return;
    }
  }

 private:
  T* val_;

  template <class U>
  void ReadFixed() {
    U val = ReadMemcpy<U>(&data_);
    // If the user is reading as signed, then perform sign extension.
    // TODO(haberman): is this the right behavior?
    if (std::is_signed<T>::value) {
      *val_ = static_cast<typename std::make_signed<U>::type>(val);
    } else {
      *val_ = static_cast<typename std::make_unsigned<U>::type>(val);
    }
  }

  void ReadVariable() {
    *val_ = ReadLEB128<T>(&data_);
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

  FormReader(const DIEReader& reader, string_view data, bool* val)
      : Base(reader, data), val_(val) {}

  template <class Func>
  static void GetFunctionForForm(const DIEReader& /*reader*/, uint8_t form,
                                 Func func) {
    switch (form) {
      case DW_FORM_flag:
        func(&Base::template ReadAttr<&FormReader::ReadFlag>);
        return;
      case DW_FORM_flag_present:
        func(&Base::template ReadAttr<&FormReader::ReadFlagPresent>);
        return;
      case DW_FORM_indirect:
        func(&ME::ReadIndirect);
        return;
      default:
        THROWF("don't know how to translate DWARF form $0 into bool", form);
    }
  }

 private:
  bool* val_;

  void ReadFlag() {
    *val_ = ReadMemcpy<uint8_t>(&data_);
  }

  void ReadFlagPresent() {
    *val_ = true;
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

  string_view ReadAttributes(const DIEReader& reader, string_view data) const;

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
      THROWF("don't know how to skip DWARF form $0", attr.form);
    }

    action_list_.push_back(AttrAction(func, nullptr, nullptr));
  }

  // Overwrite any entries for attributes we actually want to store somewhere.
  for (const auto& action : indexed_actions) {
    const auto& attr = abbrev.attr[action.index];
    if (action.action.func &&
        action.action.func != GetFormDecodeFunc<void>(attr.form, sizes)) {
      assert(action.index < action_list_.size());
      if (action_list_[action.index].data) {
        THROW(
            "internal error, specified same DWARF attribute more "
            "than once");
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
        THROWF("don't know how to convert form $0 to type $1",
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
string_view ActionBuf::ReadAttributes(const DIEReader& reader,
                                      string_view data) const {
  for (const auto& action : action_list_) {
    data = action.func(reader, data, action.data);
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

  // Reads all attributes for this DIE, storing the ones we were expecting.
  void ReadAttributes(DIEReader* reader) {
    string_view data = reader->ReadAttributesBegin();

    // Clear all existing attributes.
    values_ = std::tuple<Args...>();
    memset(&has_attr_, 0, sizeof...(Args));

    // Parse all attributes.
    sibling_ = 0;
    data = GetActionBuf(*reader).ReadAttributes(*reader, data);
    reader->ReadAttributesEnd(data, sibling_);
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

// From DIEReader, defined here because it depends on FixedAttrReader.
bool DIEReader::SkipChildren() {
  assert(state_ == State::kReadyToNext);
  if (!HasChild()) {
    return true;
  }

  int target_depth = depth_ - 1;
  dwarf::FixedAttrReader<uint64_t> attr_reader(this, {DW_AT_sibling});
  while (depth_ > target_depth) {
    // TODO(haberman): use DW_AT_sibling to optimize skipping when it is
    // available.
    if (!NextDIE()) {
      return false;
    }
    attr_reader.ReadAttributes(this);
  }
  return true;
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
    string_view name;
    uint32_t directory_index;
    uint64_t modified_time;
    uint64_t file_size;
  };

  void SeekToOffset(uint64_t offset, uint8_t address_size);
  bool ReadLineInfo();
  const LineInfo& lineinfo() const { return info_; }
  const FileName& filename(size_t i) const { return filenames_[i]; }
  string_view include_directory(size_t i) const {
    return include_directories_[i];
  }

  const std::string& GetExpandedFilename(size_t index) {
    if (index >= filenames_.size()) {
      THROW("filename index out of range");
    }

    // Generate these lazily.
    if (expanded_filenames_.size() <= index) {
      expanded_filenames_.resize(filenames_.size());
    }

    std::string& ret = expanded_filenames_[index];
    if (ret.empty()) {
      const FileName& filename = filenames_[index];
      string_view directory = include_directories_[filename.directory_index];
      ret = std::string(directory);
      if (!ret.empty()) {
        ret += "/";
      }
      ret += std::string(filename.name);
    }
    return ret;
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
  std::vector<string_view> include_directories_;
  std::vector<FileName> filenames_;
  std::vector<uint8_t> standard_opcode_lengths_;
  std::vector<std::string> expanded_filenames_;

  string_view remaining_;

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

void LineInfoReader::SeekToOffset(uint64_t offset, uint8_t address_size) {
  string_view data = file_.debug_line;
  SkipBytes(offset, &data);

  sizes_.address_size = address_size;
  data = sizes_.ReadInitialLength(&data);
  uint16_t version = ReadMemcpy<uint16_t>(&data);
  uint64_t header_length = sizes_.ReadDWARFOffset(&data);
  string_view program = data;
  SkipBytes(header_length, &program);

  params_.minimum_instruction_length = ReadMemcpy<uint8_t>(&data);
  if (version == 4) {
    params_.maximum_operations_per_instruction = ReadMemcpy<uint8_t>(&data);
  } else {
    params_.maximum_operations_per_instruction = 1;
  }
  params_.default_is_stmt = ReadMemcpy<uint8_t>(&data);
  params_.line_base = ReadMemcpy<int8_t>(&data);
  params_.line_range = ReadMemcpy<uint8_t>(&data);
  params_.opcode_base = ReadMemcpy<uint8_t>(&data);
  if (params_.line_range == 0) {
    THROW("line_range of zero will cause divide by zero");
  }

  standard_opcode_lengths_.resize(params_.opcode_base);
  for (size_t i = 1; i < params_.opcode_base; i++) {
    standard_opcode_lengths_[i] = ReadMemcpy<uint8_t>(&data);
  }

  // Read include_directories.
  include_directories_.clear();

  // Implicit current directory entry.
  include_directories_.push_back(string_view());

  while (true) {
    string_view dir = ReadNullTerminated(&data);
    if (dir.empty()) {
      break;
    }
    include_directories_.push_back(dir);
  }

  // Read file_names.
  filenames_.clear();
  expanded_filenames_.clear();

  // Filename 0 is unused.
  filenames_.push_back(FileName());
  while (true) {
    FileName file_name;
    file_name.name = ReadNullTerminated(&data);
    if (file_name.name.empty()) {
      break;
    }
    file_name.directory_index = ReadLEB128<uint32_t>(&data);
    file_name.modified_time = ReadLEB128<uint64_t>(&data);
    file_name.file_size = ReadLEB128<uint64_t>(&data);
    if (file_name.directory_index >= include_directories_.size()) {
      THROW("directory index out of range");
    }
    filenames_.push_back(file_name);
  }

  info_ = LineInfo(params_.default_is_stmt);
  remaining_ = program;
  shadow_ = false;
}

bool LineInfoReader::ReadLineInfo() {
  // Final step of last DW_LNS_copy / special opcode.
  info_.discriminator = 0;
  info_.basic_block = false;
  info_.prologue_end = false;
  info_.epilogue_begin = false;

  // Final step of DW_LNE_end_sequence.
  info_.end_sequence = false;

  string_view data = remaining_;

  while (true) {
    if (data.empty()) {
      remaining_ = data;
      return false;
    }

    uint8_t op = ReadMemcpy<uint8_t>(&data);

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
          uint16_t len = ReadLEB128<uint16_t>(&data);
          uint8_t extended_op = ReadMemcpy<uint8_t>(&data);
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
              info_.address = sizes_.ReadAddress(&data);
              info_.op_index = 0;
              shadow_ = (info_.address == 0);
              break;
            case DW_LNE_define_file: {
              FileName file_name;
              file_name.name = ReadNullTerminated(&data);
              file_name.directory_index = ReadLEB128<uint32_t>(&data);
              file_name.modified_time = ReadLEB128<uint64_t>(&data);
              file_name.file_size = ReadLEB128<uint64_t>(&data);
              if (file_name.directory_index >= include_directories_.size()) {
                THROW("directory index out of range");
              }
              filenames_.push_back(file_name);
              break;
            }
            case DW_LNE_set_discriminator:
              info_.discriminator = ReadLEB128<uint32_t>(&data);
              break;
            default:
              // We don't understand this opcode, skip it.
              SkipBytes(len, &data);
              if (verbose_level > 0) {
                fprintf(stderr,
                        "bloaty: warning: unknown DWARF line table extended "
                        "opcode: %d\n",
                        extended_op);
              }
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
        case DW_LNS_advance_pc:
          Advance(ReadLEB128<uint64_t>(&data));
          break;
        case DW_LNS_advance_line:
          info_.line += ReadLEB128<int32_t>(&data);
          break;
        case DW_LNS_set_file:
          info_.file = ReadLEB128<uint32_t>(&data);
          if (info_.file >= filenames_.size()) {
            THROW("filename index too big");
          }
          break;
        case DW_LNS_set_column:
          info_.column = ReadLEB128<uint32_t>(&data);
          break;
        case DW_LNS_negate_stmt:
          info_.is_stmt = !info_.is_stmt;
          break;
        case DW_LNS_set_basic_block:
          info_.basic_block = true;
          break;
        case DW_LNS_const_add_pc:
          SpecialOpcodeAdvance(255);
          break;
        case DW_LNS_fixed_advance_pc:
          info_.address += ReadMemcpy<uint16_t>(&data);
          info_.op_index = 0;
          break;
        case DW_LNS_set_prologue_end:
          info_.prologue_end = true;
          break;
        case DW_LNS_set_epilogue_begin:
          info_.epilogue_begin = true;
          break;
        case DW_LNS_set_isa:
          info_.isa = ReadLEB128<uint8_t>(&data);
          break;
        default:
          // Unknown opcode, but we know its length so can skip it.
          SkipBytes(standard_opcode_lengths_[op], &data);
          if (verbose_level > 0) {
            fprintf(stderr,
                    "bloaty: warning: unknown DWARF line table opcode: %d\n",
                    op);
          }
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
      if (name.empty()) {
        name = LookupFilename(compilation_unit_offset);
      }
      return name;
    }

   private:
    std::string LookupFilename(uint64_t compilation_unit_offset) {
      auto section = dwarf::DIEReader::Section::kDebugInfo;
      if (die_reader_.SeekToCompilationUnit(section, compilation_unit_offset) &&
          die_reader_.GetTag() == DW_TAG_compile_unit &&
          (attr_reader_.ReadAttributes(&die_reader_),
           attr_reader_.HasAttribute<0>())) {
        return std::string(attr_reader_.GetAttribute<0>());
      } else {
        return missing_;
      }
    }

    dwarf::DIEReader die_reader_;
    dwarf::FixedAttrReader<string_view> attr_reader_;
    std::unordered_map<uint64_t, std::string> map_;
    std::string missing_;
  } map(file);

  dwarf::AddressRanges ranges(file.debug_aranges);

  while (ranges.NextUnit()) {
    std::string filename = map.GetFilename(ranges.debug_info_offset());

    while (ranges.NextRange()) {
      if (ranges.address() != 0) {
        sink->AddVMRangeIgnoreDuplicate("dwarf_aranges", ranges.address(),
                                        ranges.length(), filename);
      }
    }
  }

  return true;
}

void AddDIE(const dwarf::File& file, const std::string& name,
            const dwarf::FixedAttrReader<string_view, string_view, uint64_t,
                                         uint64_t, string_view, uint64_t,
                                         uint64_t, uint64_t, uint64_t>& attr,
            const SymbolTable& symtab, const DualMap& symbol_map,
            const dwarf::CompilationUnitSizes& sizes, RangeSink* sink) {
  uint64_t low_pc = attr.GetAttribute<2>();
  uint64_t high_pc = attr.GetAttribute<3>();

  // Some DIEs mark address ranges with high_pc/low_pc pairs (especially
  // functions).
  if (attr.HasAttribute<2>() && attr.HasAttribute<3>() && low_pc != 0) {
    // It appears that some compilers make high_pc a size, and others make it an
    // address.
    if (high_pc >= low_pc) {
      high_pc -= low_pc;
    }
    sink->AddVMRangeIgnoreDuplicate("dwarf_pcpair", low_pc, high_pc, name);
  }

  // Sometimes a DIE has a linkage_name, which we can look up in the symbol
  // table.
  if (attr.HasAttribute<1>()) {
    auto it = symtab.find(attr.GetAttribute<1>());
    if (it != symtab.end()) {
      sink->AddVMRangeIgnoreDuplicate("dwarf_linkagename", it->second.first,
                                      it->second.second, name);
    }
  }

  // Sometimes the DIE has a "location", which gives the location as an address.
  // This parses a very small subset of the overall DWARF expression grammar.
  if (attr.HasAttribute<4>()) {
    string_view location = attr.GetAttribute<4>();
    if (location.size() == sizes.address_size + 1 &&
        location[0] == DW_OP_addr) {
      location.remove_prefix(1);
      uint64_t addr;
      // TODO(haberman): endian?
      if (sizes.address_size == 4) {
        addr = dwarf::ReadMemcpy<uint32_t>(&location);
      } else if (sizes.address_size == 8) {
        addr = dwarf::ReadMemcpy<uint64_t>(&location);
      } else {
        THROW("Unexpected address size");
      }

      // Unfortunately the location doesn't include a size, so we look that part
      // up in the symbol map.
      uint64_t size;
      if (symbol_map.vm_map.TryGetSize(addr, &size)) {
        sink->AddVMRangeIgnoreDuplicate("dwarf_location", addr, size, name);
      } else {
        if (verbose_level > 0) {
          fprintf(stderr,
                  "bloaty: warning: couldn't find DWARF location in symbol "
                  "table, address: %" PRIx64 "\n",
                  addr);
        }
      }
    }
  }

  if (attr.HasAttribute<5>()) {
    absl::string_view loc_range = file.debug_loc.substr(attr.GetAttribute<5>());
    loc_range = GetLocationListRange(sizes, loc_range);
    sink->AddFileRange("dwarf_locrange", name, loc_range);
  }

  uint64_t ranges_offset = UINT64_MAX;

  if (attr.HasAttribute<7>()) {
    ranges_offset = attr.GetAttribute<7>();
  } else if (attr.HasAttribute<8>()) {
    ranges_offset = attr.GetAttribute<8>();
  }

  if (ranges_offset != UINT64_MAX) {
    absl::string_view ranges_range = file.debug_ranges.substr(ranges_offset);
    ranges_range = GetRangeListRange(sizes, ranges_range);
    sink->AddFileRange("dwarf_debugrange", name, ranges_range);
  }
}

static void ReadDWARFPubNames(const dwarf::File& file, string_view section,
                              RangeSink* sink) {
  dwarf::DIEReader die_reader(file);
  dwarf::FixedAttrReader<string_view> attr_reader(&die_reader, {DW_AT_name});
  string_view remaining = section;

  while (remaining.size() > 0) {
    dwarf::CompilationUnitSizes sizes;
    string_view full_unit = remaining;
    string_view unit = sizes.ReadInitialLength(&remaining);
    full_unit =
        full_unit.substr(0, unit.size() + (unit.data() - full_unit.data()));
    dwarf::SkipBytes(2, &unit);
    uint64_t debug_info_offset = sizes.ReadDWARFOffset(&unit);
    bool ok = die_reader.SeekToCompilationUnit(
        dwarf::DIEReader::Section::kDebugInfo, debug_info_offset);
    if (!ok) {
      THROW("Couldn't seek to debug_info section");
    }
    attr_reader.ReadAttributes(&die_reader);
    std::string compileunit_name = std::string(attr_reader.GetAttribute<0>());
    if (!compileunit_name.empty()) {
      sink->AddFileRange("dwarf_pubnames", compileunit_name, full_unit);
    }
  }
}

uint64_t ReadEncodedPointer(uint8_t encoding, bool is_64bit, string_view* data,
                            const char* data_base, RangeSink* sink) {
  uint64_t value;
  const char *ptr = data->data();
  uint8_t format = encoding & DW_EH_PE_FORMAT_MASK;

  switch (format) {
    case DW_EH_PE_omit:
      return 0;
    case DW_EH_PE_absptr:
      if (is_64bit) {
        value = dwarf::ReadMemcpy<uint64_t>(data);
      } else {
        value = dwarf::ReadMemcpy<uint32_t>(data);
      }
      break;
    case DW_EH_PE_uleb128:
      value = dwarf::ReadLEB128<uint64_t>(data);
      break;
    case DW_EH_PE_udata2:
      value = dwarf::ReadMemcpy<uint16_t>(data);
      break;
    case DW_EH_PE_udata4:
      value = dwarf::ReadMemcpy<uint32_t>(data);
      break;
    case DW_EH_PE_udata8:
      value = dwarf::ReadMemcpy<uint64_t>(data);
      break;
    case DW_EH_PE_sleb128:
      value = dwarf::ReadLEB128<int64_t>(data);
      break;
    case DW_EH_PE_sdata2:
      value = dwarf::ReadMemcpy<int16_t>(data);
      break;
    case DW_EH_PE_sdata4:
      value = dwarf::ReadMemcpy<int32_t>(data);
      break;
    case DW_EH_PE_sdata8:
      value = dwarf::ReadMemcpy<int64_t>(data);
      break;
    default:
      THROWF("Unexpected eh_frame format value: $0", format);
  }

  uint8_t application = encoding & DW_EH_PE_APPLICATION_MASK;

  switch(application) {
    case 0:
      break;
    case DW_EH_PE_pcrel:
      value += sink->TranslateFileToVM(ptr);
      break;
    case DW_EH_PE_datarel:
      if (data_base == nullptr) {
        THROW("datarel requested but no data_base provided");
      }
      value += sink->TranslateFileToVM(data_base);
      break;
    case DW_EH_PE_textrel:
    case DW_EH_PE_funcrel:
    case DW_EH_PE_aligned:
      THROWF("Unimplemented eh_frame application value: $0", application);
  }

  if (encoding & DW_EH_PE_indirect) {
    string_view location = sink->TranslateVMToFile(value);
    if (is_64bit) {
      value = dwarf::ReadMemcpy<uint64_t>(&location);
    } else {
      value = dwarf::ReadMemcpy<uint32_t>(&location);
    }
  }

  return value;
}

// Code to read the .eh_frame section.  This is not technically DWARF, but it
// is similar to .debug_frame (which is DWARF) so it's convenient to put it
// here.
//
// The best documentation I can find for this format comes from:
//
// * http://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
// * https://www.airs.com/blog/archives/460
//
// However these are both under-specified.  Some details are not mentioned in
// either of these (for example, the fact that the function length uses the FDE
// encoding, but always absolute).  libdwarf's implementation contains a comment
// saying "It is not clear if this is entirely correct".  Basically the only
// thing you can trust for some of these details is the code that actually
// implements unwinding in production:
//
// * libunwind http://www.nongnu.org/libunwind/
//   https://github.com/pathscale/libunwind/blob/master/src/dwarf/Gfde.c
// * LLVM libunwind (a different project!!)
//   https://github.com/llvm-mirror/libunwind/blob/master/src/DwarfParser.hpp
// * libgcc
//   https://github.com/gcc-mirror/gcc/blob/master/libgcc/unwind-dw2-fde.c
void ReadEhFrame(string_view data, RangeSink* sink) {
  string_view remaining = data;

  struct CIEInfo {
    int version = 0;
    uint32_t code_align = 0;
    int32_t data_align = 0;
    uint8_t fde_encoding = 0;
    uint8_t lsda_encoding = 0;
    bool is_signal_handler = false;
    bool has_augmentation_length = false;
    uint64_t personality_function = 0;
    uint32_t return_address_reg = 0;
  };

  std::unordered_map<const char*, CIEInfo> cie_map;

  while (remaining.size() > 0) {
    dwarf::CompilationUnitSizes sizes;
    string_view full_entry = remaining;
    string_view entry = sizes.ReadInitialLength(&remaining);
    if (entry.size() == 0 && remaining.size() == 0) {
      return;
    }
    full_entry =
        full_entry.substr(0, entry.size() + (entry.data() - full_entry.data()));
    uint32_t id = dwarf::ReadMemcpy<uint32_t>(&entry);
    if (id == 0) {
      // CIE, we don't attribute this yet.
      CIEInfo& cie_info = cie_map[full_entry.data()];
      cie_info.version = dwarf::ReadMemcpy<uint8_t>(&entry);
      string_view aug_string = dwarf::ReadNullTerminated(&entry);
      cie_info.code_align = dwarf::ReadLEB128<uint32_t>(&entry);
      cie_info.data_align = dwarf::ReadLEB128<int32_t>(&entry);
      switch (cie_info.version) {
        case 1:
          cie_info.return_address_reg = dwarf::ReadMemcpy<uint8_t>(&entry);
          break;
        case 3:
          cie_info.return_address_reg = dwarf::ReadLEB128<uint32_t>(&entry);
          break;
        default:
          THROW("Unexpected eh_frame CIE version");
      }
      while (aug_string.size() > 0) {
        switch (aug_string[0]) {
          case 'z':
            // Length until the end of augmentation data.
            cie_info.has_augmentation_length = true;
            dwarf::ReadLEB128<uint32_t>(&entry);
            break;
          case 'L':
            cie_info.lsda_encoding = dwarf::ReadMemcpy<uint8_t>(&entry);
            break;
          case 'R':
            cie_info.fde_encoding = dwarf::ReadMemcpy<uint8_t>(&entry);
            break;
          case 'S':
            cie_info.is_signal_handler = true;
            break;
          case 'P': {
            uint8_t encoding = dwarf::ReadMemcpy<uint8_t>(&entry);
            cie_info.personality_function =
                ReadEncodedPointer(encoding, true, &entry, nullptr, sink);
            break;
          }
          default:
            THROW("Unexepcted augmentation character");
        }
        aug_string.remove_prefix(1);
      }
    } else {
      auto iter = cie_map.find(entry.data() - id - 4);
      if (iter == cie_map.end()) {
        THROW("Couldn't find CIE for FDE");
      }
      const CIEInfo& cie_info = iter->second;
      // TODO(haberman): don't hard-code 64-bit.
      uint64_t address = ReadEncodedPointer(cie_info.fde_encoding, true, &entry,
                                            nullptr, sink);
      // TODO(haberman); Technically the FDE addresses could span a
      // function/compilation unit?  They can certainly span inlines.
      /*
      uint64_t length =
        ReadEncodedPointer(cie_info.fde_encoding & 0xf, true, &entry, sink);
      (void)length;

      if (cie_info.has_augmentation_length) {
        uint32_t augmentation_length = dwarf::ReadLEB128<uint32_t>(&entry);
        (void)augmentation_length;
      }

      uint64_t lsda =
          ReadEncodedPointer(cie_info.lsda_encoding, true, &entry, sink);
      if (lsda) {
      }
      */

      sink->AddFileRangeFor("dwarf_fde", address, full_entry);
    }
  }
}

// See documentation here:
//   http://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html#EHFRAME
void ReadEhFrameHdr(string_view data, RangeSink* sink) {
  const char* base = data.data();
  uint8_t version = dwarf::ReadMemcpy<uint8_t>(&data);
  uint8_t eh_frame_ptr_enc = dwarf::ReadMemcpy<uint8_t>(&data);
  uint8_t fde_count_enc = dwarf::ReadMemcpy<uint8_t>(&data);
  uint8_t table_enc = dwarf::ReadMemcpy<uint8_t>(&data);

  if (version != 1) {
    THROWF("Unknown eh_frame_hdr version: $0", version);
  }

  // TODO(haberman): don't hard-code 64-bit.
  uint64_t eh_frame_ptr =
      ReadEncodedPointer(eh_frame_ptr_enc, true, &data, base, sink);
  (void)eh_frame_ptr;
  uint64_t fde_count =
      ReadEncodedPointer(fde_count_enc, true, &data, base, sink);

  for (uint64_t i = 0; i < fde_count; i++) {
    string_view entry_data = data;
    uint64_t initial_location =
        ReadEncodedPointer(table_enc, true, &data, base, sink);
    uint64_t fde_addr = ReadEncodedPointer(table_enc, true, &data, base, sink);
    entry_data.remove_suffix(data.size());
    sink->AddFileRangeFor("dwarf_fde_table", initial_location, entry_data);

    // We could add fde_addr with an unknown length if we wanted to skip reading
    // eh_frame.  We can't count on this table being available though, so we
    // don't want to remove the eh_frame reading code altogether.
    (void)fde_addr;
  }
}

static void ReadDWARFStmtListRange(const dwarf::File& file, uint64_t offset,
                                   string_view unit_name, RangeSink* sink) {
  string_view data = file.debug_line;
  dwarf::SkipBytes(offset, &data);
  string_view data_with_length = data;
  dwarf::CompilationUnitSizes sizes;
  data = sizes.ReadInitialLength(&data);
  data = data_with_length.substr(
      0, data.size() + (data.data() - data_with_length.data()));
  sink->AddFileRange("dwarf_stmtlistrange", unit_name, data);
}

// The DWARF debug info can help us get compileunits info.  DIEs for compilation
// units, functions, and global variables often have attributes that will
// resolve to addresses.
static void ReadDWARFDebugInfo(
    const dwarf::File& file, dwarf::DIEReader::Section section,
    const SymbolTable& symtab, const DualMap& symbol_map, RangeSink* sink,
    std::unordered_map<uint64_t, std::string>* stmt_list_map) {
  dwarf::DIEReader die_reader(file);
  die_reader.set_strp_sink(sink);
  dwarf::FixedAttrReader<string_view, string_view, uint64_t, uint64_t,
                         string_view, uint64_t, uint64_t, uint64_t, uint64_t>
      attr_reader(&die_reader,
                  {DW_AT_name, DW_AT_linkage_name, DW_AT_low_pc, DW_AT_high_pc,
                   DW_AT_location, DW_AT_location, DW_AT_stmt_list,
                   DW_AT_ranges, DW_AT_start_scope});

  if (!die_reader.SeekToStart(section)) {
    return;
  }

  do {
    attr_reader.ReadAttributes(&die_reader);
    std::string compileunit_name = std::string(attr_reader.GetAttribute<0>());

    if (attr_reader.HasAttribute<6>()) {
      uint64_t stmt_list = attr_reader.GetAttribute<6>();
      if (compileunit_name.empty()) {
        auto iter = stmt_list_map->find(stmt_list);
        if (iter != stmt_list_map->end()) {
          compileunit_name = iter->second;
        }
      } else {
        (*stmt_list_map)[stmt_list] = compileunit_name;
      }
    }

    if (compileunit_name.empty()) {
      continue;
    }

    die_reader.set_compileunit_name(compileunit_name);
    sink->AddFileRange("dwarf_debuginfo", compileunit_name,
                       die_reader.unit_range());
    AddDIE(file, compileunit_name, attr_reader, symtab, symbol_map,
           die_reader.unit_sizes(), sink);

    if (attr_reader.HasAttribute<6>()) {
      uint64_t offset = attr_reader.GetAttribute<6>();
      ReadDWARFStmtListRange(file, offset, compileunit_name, sink);
    }

    string_view abbrev_data = file.debug_abbrev;
    dwarf::SkipBytes(die_reader.debug_abbrev_offset(), &abbrev_data);
    dwarf::AbbrevTable unit_abbrev;
    abbrev_data = unit_abbrev.ReadAbbrevs(abbrev_data);
    sink->AddFileRange("dwarf_abbrev", compileunit_name, abbrev_data);


    while (die_reader.NextDIE()) {
      attr_reader.ReadAttributes(&die_reader);

      // low_pc == 0 is a signal that this routine was stripped out of the
      // final binary.  Skip this DIE and all of its children.
      if (attr_reader.HasAttribute<2>() && attr_reader.GetAttribute<2>() == 0) {
        die_reader.SkipChildren();
      } else {
        AddDIE(file, compileunit_name, attr_reader, symtab, symbol_map,
               die_reader.unit_sizes(), sink);
      }
    }
  } while (die_reader.NextCompilationUnit());
}

void ReadDWARFCompileUnits(const dwarf::File& file, const SymbolTable& symtab,
                           const DualMap& symbol_map, RangeSink* sink) {
  if (!file.debug_info.size()) {
    THROW("missing debug info");
  }

  if (file.debug_aranges.size()) {
    ReadDWARFAddressRanges(file, sink);
  }

  std::unordered_map<uint64_t, std::string> stmt_list_map;
  ReadDWARFDebugInfo(file, dwarf::DIEReader::Section::kDebugInfo, symtab,
                     symbol_map, sink, &stmt_list_map);
  ReadDWARFDebugInfo(file, dwarf::DIEReader::Section::kDebugTypes, symtab,
                     symbol_map, sink, &stmt_list_map);
  ReadDWARFPubNames(file, file.debug_pubnames, sink);
  ReadDWARFPubNames(file, file.debug_pubtypes, sink);
}

static std::string LineInfoKey(const std::string& file, uint32_t line,
                               bool include_line) {
  if (include_line) {
    return file + ":" + std::to_string(line);
  } else {
    return file;
  }
}

static void ReadDWARFStmtList(bool include_line,
                              dwarf::LineInfoReader* line_info_reader,
                              RangeSink* sink) {
  uint64_t span_startaddr = 0;
  std::string last_source;

  while (line_info_reader->ReadLineInfo()) {
    const auto& line_info = line_info_reader->lineinfo();
    auto addr = line_info.address;
    auto number = line_info.line;
    auto name =
        line_info.end_sequence
            ? last_source
            : LineInfoKey(line_info_reader->GetExpandedFilename(line_info.file),
                          number, include_line);
    if (!span_startaddr) {
      span_startaddr = addr;
    } else if (line_info.end_sequence ||
        (!last_source.empty() && name != last_source)) {
      sink->AddVMRange("dwarf_stmtlist", span_startaddr, addr - span_startaddr,
                       last_source);
      if (line_info.end_sequence) {
        span_startaddr = 0;
      } else {
        span_startaddr = addr;
      }
    }
    last_source = name;
  }
}

void ReadDWARFInlines(const dwarf::File& file, RangeSink* sink,
                      bool include_line) {
  if (!file.debug_info.size() || !file.debug_line.size()) {
    THROW("no debug info");
  }

  dwarf::DIEReader die_reader(file);
  dwarf::LineInfoReader line_info_reader(file);
  dwarf::FixedAttrReader<uint64_t> attr_reader(&die_reader, {DW_AT_stmt_list});

  if (!die_reader.SeekToStart(dwarf::DIEReader::Section::kDebugInfo)) {
    THROW("debug info is present, but empty");
  }

  while (true) {
    attr_reader.ReadAttributes(&die_reader);

    if (attr_reader.HasAttribute<0>()) {
      uint64_t offset = attr_reader.GetAttribute<0>();
      line_info_reader.SeekToOffset(offset,
                                    die_reader.unit_sizes().address_size);
      ReadDWARFStmtList(include_line, &line_info_reader, sink);
    }

    if (!die_reader.NextCompilationUnit()) {
      return;
    }
  }
}

} // namespace bloaty
