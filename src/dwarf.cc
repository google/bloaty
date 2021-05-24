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
#include <limits>
#include <memory>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/macros.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/types/optional.h"
#include "bloaty.h"
#include "bloaty.pb.h"
#include "dwarf_constants.h"
#include "util.h"

using namespace dwarf2reader;
using absl::string_view;

namespace bloaty {

extern int verbose_level;

namespace dwarf {

int DivRoundUp(int n, int d) {
  return (n + (d - 1)) / d;
}

namespace {

// uint64/32 max is a tombstone value added by https://reviews.llvm.org/D81784.
bool IsValidDwarfAddress(uint64_t addr, uint8_t address_size) {
  if (addr == 0)
    return false;
  if (address_size == 4 && addr == std::numeric_limits<uint32_t>::max())
    return false;
  if (address_size == 8 && addr == std::numeric_limits<uint64_t>::max())
    return false;
  return true;
}

}  // namespace

// Low-level Parsing Routines //////////////////////////////////////////////////

// For parsing the low-level values found in DWARF files.  These are the only
// routines that touch the bytes of the input buffer directly.  Everything else
// is layered on top of these.

// Parses the LEB128 format defined by DWARF (both signed and unsigned
// versions).

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
      if (is_signed && shift < 64 && (byte & 0x40)) {
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
class CompilationUnitSizes {
 public:
  // When true, DWARF offsets are 64 bits, otherwise they are 32 bit.
  bool dwarf64() const { return dwarf64_; }

  // The size of addresses.  Guaranteed to be either 4 or 8.
  uint8_t address_size() const { return address_size_; }

  // DWARF version of this unit.
  uint8_t dwarf_version() const { return dwarf_version_; }

  void SetAddressSize(uint8_t address_size) {
    if (address_size != 4 && address_size != 8) {
      THROWF("Unexpected address size: $0", address_size);
    }
    address_size_ = address_size;
  }

  // To allow this as the key in a map.
  bool operator<(const CompilationUnitSizes& rhs) const {
    return std::tie(dwarf64_, address_size_) <
           std::tie(rhs.dwarf64_, rhs.address_size_);
  }

  // Reads a DWARF offset based on whether we are reading dwarf32 or dwarf64
  // format.
  uint64_t ReadDWARFOffset(string_view* data) const {
    if (dwarf64_) {
      return ReadFixed<uint64_t>(data);
    } else {
      return ReadFixed<uint32_t>(data);
    }
  }

  // Reads an address according to the expected address_size.
  uint64_t ReadAddress(string_view* data) const {
    if (address_size_ == 8) {
      return ReadFixed<uint64_t>(data);
    } else if (address_size_ == 4) {
      return ReadFixed<uint32_t>(data);
    } else {
      BLOATY_UNREACHABLE();
    }
  }

  uint64_t MaxAddress() const {
    if (address_size_ == 8) {
      return 0xffffffffffffffff;
    } else if (address_size_ == 4) {
      return 0xffffffff;
    } else {
      BLOATY_UNREACHABLE();
    }
  }

  // Reads an "initial length" as specified in many DWARF headers.  This
  // contains either a 32-bit or a 64-bit length, and signals whether we are
  // using the 32-bit or 64-bit DWARF format (so it sets dwarf64 appropriately).
  //
  // Returns the range for this section and stores the remaining data
  // in |remaining|.
  string_view ReadInitialLength(string_view* remaining) {
    uint64_t len = ReadFixed<uint32_t>(remaining);

    if (len == 0xffffffff) {
      dwarf64_ = true;
      len = ReadFixed<uint64_t>(remaining);
    } else {
      dwarf64_ = false;
    }

    if (remaining->size() < len) {
      THROW("short DWARF compilation unit");
    }

    string_view unit = *remaining;
    unit.remove_suffix(remaining->size() - len);
    *remaining = remaining->substr(len);
    return unit;
  }

  void ReadDWARFVersion(string_view* data) {
    dwarf_version_ = ReadFixed<uint16_t>(data);
  }

 private:
  uint16_t dwarf_version_;
  bool dwarf64_;
  uint8_t address_size_;
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
  void ReadAbbrevs(string_view data);

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
  string_view abbrev_data() const { return abbrev_data_; }

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
  string_view abbrev_data_;
};

void AbbrevTable::ReadAbbrevs(string_view data) {
  const char* start = data.data();
  while (true) {
    uint32_t code = ReadLEB128<uint32_t>(&data);

    if (code == 0) {
      abbrev_data_ = string_view(start, data.data() - start);
      return;
    }

    Abbrev& abbrev = abbrev_[code];

    if (abbrev.code) {
      THROW("DWARF data contained duplicate abbrev code");
    }

    uint8_t has_child;

    abbrev.code = code;
    abbrev.tag = ReadLEB128<uint16_t>(&data);
    has_child = ReadFixed<uint8_t>(&data);

    switch (has_child) {
      case DW_children_yes:
        abbrev.has_child = true;
        break;
      case DW_children_no:
        abbrev.has_child = false;
        break;
      default:
        THROWF("DWARF has_child is neither true nor false: $0, code=$1, tag=$2",
               has_child, abbrev.code, abbrev.tag);
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

  uint8_t address_size() const { return sizes_.address_size(); }

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
  sizes_.ReadDWARFVersion(&unit_remaining_);

  if (sizes_.dwarf_version() > 4) {
    THROW("DWARF data is too new for us");
  }

  debug_info_offset_ = sizes_.ReadDWARFOffset(&unit_remaining_);

  uint8_t segment_size;

  sizes_.SetAddressSize(ReadFixed<uint8_t>(&unit_remaining_));
  segment_size = ReadFixed<uint8_t>(&unit_remaining_);

  if (segment_size) {
    THROW("we don't know how to handle segmented addresses.");
  }

  size_t ofs = unit_remaining_.data() - section_.data();
  size_t aligned_ofs = AlignUp(ofs, sizes_.address_size() * 2);
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
             (start == UINT32_MAX && sizes_.address_size() == 4)) {
    // Base address selection, nothing more to do.
  } else {
    // Need to skip the location description.
    uint16_t length = ReadFixed<uint16_t>(&remaining_);
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

// DIEReader ///////////////////////////////////////////////////////////////////

class CUIter;
class CU;
class CUIter;
class DIEReader;

class InfoReader {
 public:
  InfoReader(const File& file) : dwarf_(file) {}
  InfoReader(const InfoReader&) = delete;
  InfoReader& operator=(const InfoReader&) = delete;

  const File& dwarf() const { return dwarf_; }

  // DIEs exist in both .debug_info and .debug_types.
  enum class Section {
    kDebugInfo,
    kDebugTypes
  };

  CUIter GetCUIter(Section section, uint64_t offset = 0);

 private:
  friend class CUIter;
  const File& dwarf_;

  std::unordered_map<uint64_t, std::string> stmt_list_map_;

  // All of the AbbrevTables we've read from .debug_abbrev, indexed by their
  // offset within .debug_abbrev.
  std::unordered_map<uint64_t, AbbrevTable> abbrev_tables_;
};

class CUIter {
 public:
  bool NextCU(InfoReader& reader, CU* cu);

 private:
  friend class InfoReader;
  CUIter(const File& dwarf, InfoReader::Section section, string_view next_unit)
      : dwarf_(dwarf), section_(section), next_unit_(next_unit) {}

  // Data for the next compilation unit.
  const File& dwarf_;
  InfoReader::Section section_;
  string_view next_unit_;
};

class CU {
 public:
  DIEReader GetDIEReader();

  const File& dwarf() const { return *dwarf_; }
  const CompilationUnitSizes& unit_sizes() const { return unit_sizes_; }
  const std::string& unit_name() const { return unit_name_; }
  string_view unit_range() const { return unit_range_; }
  uint64_t addr_base() const { return addr_base_; }
  uint64_t str_offsets_base() const { return str_offsets_base_; }
  uint64_t range_lists_base() const { return range_lists_base_; }
  const AbbrevTable& unit_abbrev() const { return *unit_abbrev_; }

  void AddIndirectString(string_view range) const {
    if (strp_sink_) {
      strp_sink_->AddFileRange("dwarf_strp", unit_name_, range);
    }
  }

  void set_strp_sink(RangeSink* strp_sink) { strp_sink_ = strp_sink; }

 private:
  friend class CUIter;
  friend class DIEReader;

  const File* dwarf_;

  // Info that comes from the CU header.
  string_view unit_range_;  // Range of the entire CU.
  string_view data_range_;  // Range of data (excludes CU header).
  CompilationUnitSizes unit_sizes_;
  AbbrevTable* unit_abbrev_;

  // Only for .debug_types
  uint64_t unit_type_signature_;
  uint64_t unit_type_offset_;

  // Info that comes from the top-level DIE.
  std::string unit_name_;
  uint64_t addr_base_ = 0;
  uint64_t str_offsets_base_ = 0;
  uint64_t range_lists_base_ = 0;

  RangeSink* strp_sink_ = nullptr;
};

class DIEReader {
 public:
  // Abbreviation for the current entry.
  const AbbrevTable::Abbrev* ReadCode(const CU& cu);

  template <class T>
  void ReadAttributes(const CU& cu, const AbbrevTable::Abbrev* code, T&& func);

  void SkipChildren(const CU& cu, const AbbrevTable::Abbrev* code);

 private:
  // Internal APIs.
  friend class CU;

  DIEReader(string_view data) : remaining_(data) {}

  void SkipNullEntries();

  // Our current read position.
  string_view remaining_;
  int depth_ = 0;
};

DIEReader CU::GetDIEReader() { return DIEReader(data_range_); }

class AttrValue {
 public:
  static AttrValue ParseAttr(const CU& cu, uint8_t form, string_view* data);

  AttrValue(const AttrValue &) = default;
  AttrValue &operator=(const AttrValue &) = default;

  bool IsUint() const {
    return type_ == Type::kUint || type_ == Type::kUnresolvedUint;
  }

  bool IsString() const {
    return type_ == Type::kString || type_ == Type::kUnresolvedString;
  }

  absl::optional<uint64_t> ToUint(const CU& cu) const {
    if (IsUint()) return GetUint(cu);
    string_view str = GetString(cu);
    switch (str.size()) {
      case 1:
        return ReadFixed<uint8_t>(&str);
      case 2:
        return ReadFixed<uint8_t>(&str);
      case 4:
        return ReadFixed<uint32_t>(&str);
      case 8:
        return ReadFixed<uint64_t>(&str);
    }
    return absl::nullopt;
  }

  uint64_t GetUint(const CU& cu) const {
    if (type_ == Type::kUnresolvedUint) {
      return ResolveIndirectAddress(cu);
    } else {
      assert(type_ == Type::kUint);
      return uint_;
    }
  }

  string_view GetString(const CU& cu) const {
    if (type_ == Type::kUnresolvedString) {
      return ResolveDoubleIndirectString(cu);
    } else {
      assert(type_ == Type::kString);
      return string_;
    }
  }

  void SetForm(uint16_t form) {
    form_ = form;
  }

  uint16_t form() const { return form_; }

  std::string DebugString() const {
    if (type_ == Type::kUint) {
      return absl::Substitute("AttrValue{type_=Type::kUint, form_=$0, uint_=$1}", form_, uint_);
    } else {
      return absl::Substitute("AttrValue{type_=Type::kString, form_=$0, string_=$1}", form_, string_);
    }
  }

 private:
  explicit AttrValue(uint64_t val) : uint_(val), type_(Type::kUint) {}
  explicit AttrValue(string_view val) : string_(val), type_(Type::kString) {}

  // Some attribute values remain unresolved after being parsed.
  // We have to delay the resolution of some indirect values because they are
  // dependent on bases that come after it in the sequence of attributes, eg.
  //
  // $ dwarfdump -i bloaty
  //   COMPILE_UNIT<header overall offset = 0x00000000>:
  // < 0><0x0000000c>  DW_TAG_compile_unit
  //                     DW_AT_producer              (indexed string: 0x00000000)Debian clang version 11.0.1-2
  //                     DW_AT_language              DW_LANG_C_plus_plus_14
  //                     DW_AT_name                  (indexed string: 0x00000001)../src/main.cc
  //                     DW_AT_str_offsets_base      0x00000008
  //
  // Note that DW_AT_name comes before DW_AT_str_offset_base, but the latter
  // value is required to resolve the name attribute.
  enum class Type {
    kUint,
    kString,
    kUnresolvedUint,
    kUnresolvedString
  };

  Type type() const { return type_; }

  static AttrValue UnresolvedUint(uint64_t val) {
     AttrValue ret(val);
     ret.type_ = Type::kUnresolvedUint;
     return ret;
  }

  static AttrValue UnresolvedString(uint64_t val) {
     AttrValue ret(val);
     ret.type_ = Type::kUnresolvedString;
     return ret;
  }

  union {
    uint64_t uint_;
    string_view string_;
  };

  Type type_;
  uint16_t form_;

  template <class D>
  static string_view ReadBlock(string_view* data);
  static string_view ReadVariableBlock(string_view* data);
  template <class D>
  static string_view ReadIndirectString(const CU& cu, string_view* data);
  static string_view ResolveIndirectString(const CU& cu, uint64_t ofs);

  string_view ResolveDoubleIndirectString(const CU &cu) const;
  uint64_t ResolveIndirectAddress(const CU& cu) const;
};

uint64_t ReadIndirectAddress(const CU& cu, uint64_t val) {
  string_view addrs = cu.dwarf().debug_addr;
  const dwarf::CompilationUnitSizes& sizes = cu.unit_sizes();
  switch (sizes.address_size()) {
    case 4:
      SkipBytes((val * 4) + cu.addr_base(), &addrs);
      return ReadFixed<uint32_t>(&addrs);
    case 8:
      SkipBytes((val * 8) + cu.addr_base(), &addrs);
      return ReadFixed<uint64_t>(&addrs);
    default:
      BLOATY_UNREACHABLE();
  }
}

CUIter InfoReader::GetCUIter(Section section, uint64_t offset) {
  string_view data;

  if (section == Section::kDebugInfo) {
    data = dwarf_.debug_info;
  } else {
    data = dwarf_.debug_types;
  }

  SkipBytes(offset, &data);
  return CUIter(dwarf_, section, data);
}

bool CUIter::NextCU(InfoReader& reader, CU* cu) {
  if (next_unit_.empty()) return false;

  string_view unit_range = next_unit_;
  string_view data_range = cu->unit_sizes_.ReadInitialLength(&next_unit_);
  cu->unit_range_ = unit_range.substr(
      0, data_range.size() + (data_range.data() - unit_range.data()));

  CompilationUnitSizes unit_sizes;
  unit_sizes.ReadDWARFVersion(&data_range);

  if (unit_sizes.dwarf_version() > 5) {
    THROWF("Data is in DWARF $1 format which we don't understand",
           unit_sizes.dwarf_version());
  }

  uint64_t debug_abbrev_offset;

  if (unit_sizes.dwarf_version() == 5) {
    uint8_t unit_type = ReadFixed<uint8_t>(&data_range);
    (void)unit_type;  // We don't use this currently.
    unit_sizes.SetAddressSize(ReadFixed<uint8_t>(&data_range));
    debug_abbrev_offset = unit_sizes.ReadDWARFOffset(&data_range);
  } else {
    debug_abbrev_offset = unit_sizes.ReadDWARFOffset(&data_range);
    unit_sizes.SetAddressSize(ReadFixed<uint8_t>(&data_range));

    if (section_ == InfoReader::Section::kDebugTypes) {
      cu->unit_type_signature_ = ReadFixed<uint64_t>(&data_range);
      cu->unit_type_offset_ = unit_sizes.ReadDWARFOffset(&data_range);
    }
  }

  cu->unit_abbrev_ = &reader.abbrev_tables_[debug_abbrev_offset];

  // If we haven't already read abbreviations for this debug_abbrev_offset_, we
  // need to do so now.
  if (cu->unit_abbrev_->IsEmpty()) {
    string_view abbrev_data = reader.dwarf_.debug_abbrev;
    SkipBytes(debug_abbrev_offset, &abbrev_data);
    cu->unit_abbrev_->ReadAbbrevs(abbrev_data);
  }

  cu->dwarf_ = &reader.dwarf_;
  cu->unit_sizes_ = unit_sizes;
  cu->data_range_ = data_range;

  // We now read the root-level DIE in order to populate these base addresses
  // on which other attributes depend.
  DIEReader die_reader = cu->GetDIEReader();
  const auto* abbrev = die_reader.ReadCode(*cu);
  absl::optional<uint64_t> stmt_list;
  cu->unit_name_.clear();
  die_reader.ReadAttributes(
      *cu, abbrev, [cu, &stmt_list](uint16_t tag, dwarf::AttrValue value) {
        switch (tag) {
          case DW_AT_name:
            cu->unit_name_ = std::string(value.GetString(*cu));
            break;
          case DW_AT_stmt_list:
            stmt_list = value.ToUint(*cu);
            break;
          case DW_AT_addr_base:
            cu->addr_base_ = value.GetUint(*cu);
            break;
          case DW_AT_str_offsets_base:
            cu->str_offsets_base_ = value.GetUint(*cu);
            break;
          case DW_AT_rnglists_base:
            cu->range_lists_base_ = value.GetUint(*cu);
            break;
        }
      });

  if (stmt_list) {
    if (cu->unit_name_.empty()) {
      auto iter = reader.stmt_list_map_.find(*stmt_list);
      if (iter != reader.stmt_list_map_.end()) {
        cu->unit_name_ = iter->second;
      }
    } else {
      (reader.stmt_list_map_)[*stmt_list] = cu->unit_name_;
    }
  }

  return true;
}

void DIEReader::SkipNullEntries() {
  while (!remaining_.empty() && remaining_[0] == 0) {
    // null entry terminates a chain of sibling entries.
    remaining_.remove_prefix(1);
    depth_--;
  }
}

const AbbrevTable::Abbrev* DIEReader::ReadCode(const CU& cu) {
  SkipNullEntries();
  if (remaining_.empty()) {
    return nullptr;
  }
  uint32_t code = ReadLEB128<uint32_t>(&remaining_);
  const AbbrevTable::Abbrev* ret;
  if (!cu.unit_abbrev_->GetAbbrev(code, &ret)) {
    THROW("couldn't find abbreviation for code");
  }
  if (ret->has_child) {
    depth_++;
  }
  return ret;
}

void DIEReader::SkipChildren(const CU& cu, const AbbrevTable::Abbrev* abbrev) {
  if (!abbrev->has_child) {
    return;
  }

  int target_depth = depth_ - 1;
  SkipNullEntries();
  while (depth_ > target_depth) {
    // TODO(haberman): use DW_AT_sibling to optimize skipping when it is
    // available.
    abbrev = ReadCode(cu);
    if (!abbrev) {
      return;
    }
    ReadAttributes(cu, abbrev, [](uint16_t, dwarf::AttrValue) {});
    SkipNullEntries();
  }
}

// Reads all attributes for this DIE, storing the ones we were expecting.
template <class T>
void DIEReader::ReadAttributes(const CU& cu, const AbbrevTable::Abbrev* abbrev,
                               T&& func) {
  for (auto attr : abbrev->attr) {
    AttrValue value = AttrValue::ParseAttr(cu, attr.form, &remaining_);
    value.SetForm(attr.form);
    func(attr.name, value);
  }
}

template <class D>
string_view AttrValue::ReadBlock(string_view* data) {
  D len = ReadFixed<D>(data);
  return ReadBytes(len, data);
}

string_view AttrValue::ReadVariableBlock(string_view* data) {
  uint64_t len = ReadLEB128<uint64_t>(data);
  return ReadBytes(len, data);
}

string_view AttrValue::ResolveIndirectString(const CU& cu, uint64_t ofs) {
  StringTable table(cu.dwarf().debug_str);
  string_view ret = table.ReadEntry(ofs);
  cu.AddIndirectString(ret);
  return ret;
}

template <class D>
string_view AttrValue::ReadIndirectString(const CU& cu, string_view* data) {
  return ResolveIndirectString(cu, ReadFixed<D>(data));
}

string_view
AttrValue::ResolveDoubleIndirectString(const CU &cu) const {
  uint64_t ofs = uint_;
  string_view offsets = cu.dwarf().debug_str_offsets;
  uint64_t ofs2;
  if (cu.unit_sizes().dwarf64()) {
    SkipBytes((ofs * 8) + cu.str_offsets_base(), &offsets);
    ofs2 = ReadFixed<uint64_t>(&offsets);
  } else {
    SkipBytes((ofs * 4) + cu.str_offsets_base(), &offsets);
    ofs2 = ReadFixed<uint32_t>(&offsets);
  }
  StringTable table(cu.dwarf().debug_str);
  string_view ret = table.ReadEntry(ofs2);
  cu.AddIndirectString(ret);
  return ret;
}

uint64_t AttrValue::ResolveIndirectAddress(const CU& cu) const {
  return ReadIndirectAddress(cu, uint_);
}

AttrValue AttrValue::ParseAttr(const CU& cu, uint8_t form, string_view* data) {
  switch (form) {
    case DW_FORM_indirect: {
      uint16_t indirect_form = ReadLEB128<uint16_t>(data);
      if (indirect_form == DW_FORM_indirect) {
        THROW("indirect attribute has indirect form type");
      }
      return ParseAttr(cu, indirect_form, data);
    }
    case DW_FORM_ref1:
      return AttrValue(ReadFixed<uint8_t>(data));
    case DW_FORM_ref2:
      return AttrValue(ReadFixed<uint16_t>(data));
    case DW_FORM_ref4:
      return AttrValue(ReadFixed<uint32_t>(data));
    case DW_FORM_ref_sig8:
    case DW_FORM_ref8:
      return AttrValue(ReadFixed<uint64_t>(data));
    case DW_FORM_ref_udata:
    case DW_FORM_strx1:
      return AttrValue::UnresolvedString(ReadFixed<uint8_t>(data));
    case DW_FORM_strx2:
      return AttrValue::UnresolvedString(ReadFixed<uint16_t>(data));
    case DW_FORM_strx4:
      return AttrValue::UnresolvedString(ReadFixed<uint32_t>(data));
    case DW_FORM_strx:
      return AttrValue::UnresolvedString(ReadLEB128<uint64_t>(data));
    case DW_FORM_addrx1:
      return AttrValue::UnresolvedUint(ReadFixed<uint8_t>(data));
    case DW_FORM_addrx2:
      return AttrValue::UnresolvedUint(ReadFixed<uint16_t>(data));
    case DW_FORM_addrx3:
      return AttrValue::UnresolvedUint(ReadFixed<uint32_t, 3>(data));
    case DW_FORM_addrx4:
      return AttrValue::UnresolvedUint(ReadFixed<uint32_t>(data));
    case DW_FORM_addrx:
      return AttrValue::UnresolvedUint(ReadLEB128<uint64_t>(data));
    case DW_FORM_addr:
    address_size:
      switch (cu.unit_sizes().address_size()) {
        case 4:
          return AttrValue(ReadFixed<uint32_t>(data));
        case 8:
          return AttrValue(ReadFixed<uint64_t>(data));
        default:
          BLOATY_UNREACHABLE();
      }
    case DW_FORM_ref_addr:
      if (cu.unit_sizes().dwarf_version() <= 2) {
        goto address_size;
      }
      ABSL_FALLTHROUGH_INTENDED;
    case DW_FORM_sec_offset:
      if (cu.unit_sizes().dwarf64()) {
        return AttrValue(ReadFixed<uint64_t>(data));
      } else {
        return AttrValue(ReadFixed<uint32_t>(data));
      }
    case DW_FORM_udata:
      return AttrValue(ReadLEB128<uint64_t>(data));
    case DW_FORM_block1:
      return AttrValue(ReadBlock<uint8_t>(data));
    case DW_FORM_block2:
      return AttrValue(ReadBlock<uint16_t>(data));
    case DW_FORM_block4:
      return AttrValue(ReadBlock<uint32_t>(data));
    case DW_FORM_block:
    case DW_FORM_exprloc:
      return AttrValue(ReadVariableBlock(data));
    case DW_FORM_string:
      return AttrValue(ReadNullTerminated(data));
    case DW_FORM_strp:
      if (cu.unit_sizes().dwarf64()) {
        return AttrValue(ReadIndirectString<uint64_t>(cu, data));
      } else {
        return AttrValue(ReadIndirectString<uint32_t>(cu, data));
      }
    case DW_FORM_data1:
      return AttrValue(ReadBytes(1, data));
    case DW_FORM_data2:
      return AttrValue(ReadBytes(2, data));
    case DW_FORM_data4:
      return AttrValue(ReadBytes(4, data));
    case DW_FORM_data8:
      return AttrValue(ReadBytes(8, data));
    case DW_FORM_rnglistx: {
      auto val = AttrValue(ReadLEB128<uint64_t>(data));
      return val;
    }

    // Bloaty doesn't currently care about any bool or signed data.
    // So we fudge it a bit and just stuff these in a uint64.
    case DW_FORM_flag_present:
      return AttrValue(1);
    case DW_FORM_flag:
      return AttrValue(ReadFixed<uint8_t>(data));
    case DW_FORM_sdata:
      return AttrValue(ReadLEB128<uint64_t>(data));
    default:
      THROWF("Don't know how to parse DWARF form: $0", form);
  }
}

// RangeList ///////////////////////////////////////////////////////////////////

void ReadRangeList(const CU& cu, uint64_t low_pc, string_view name,
                   RangeSink* sink, string_view* data) {
  std::string name_str(name);
  while (true) {
    uint64_t start, end;
    start = cu.unit_sizes().ReadAddress(data);
    end = cu.unit_sizes().ReadAddress(data);
    if (start == 0 && end == 0) {
      return;
    } else if (start == cu.unit_sizes().MaxAddress()) {
      low_pc = end;
    } else {
      uint64_t size = end - start;
      sink->AddVMRangeIgnoreDuplicate("dwarf_rangelist", low_pc + start, size,
                                      name_str);
    }
  }
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

  // Whether we are in a "shadow" part of the bytecode program.  Sometimes
  // parts of the line info program make it into the final binary even though
  // the corresponding code was stripped.  We can tell when this happened by
  // looking for DW_LNE_set_address ops where the operand is 0.  This
  // indicates that a relocation for that argument never got applied, which
  // probably means that the code got stripped.
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
      // This is by far the common case (only false on VLIW architectuers),
      // and this inlining/specialization avoids a costly division.
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

  sizes_.SetAddressSize(address_size);
  data = sizes_.ReadInitialLength(&data);
  sizes_.ReadDWARFVersion(&data);
  uint64_t header_length = sizes_.ReadDWARFOffset(&data);
  string_view program = data;
  SkipBytes(header_length, &program);

  params_.minimum_instruction_length = ReadFixed<uint8_t>(&data);
  if (sizes_.dwarf_version() == 4) {
    params_.maximum_operations_per_instruction = ReadFixed<uint8_t>(&data);

    if (params_.maximum_operations_per_instruction == 0) {
      THROW("DWARF line info had maximum_operations_per_instruction=0");
    }
  } else {
    params_.maximum_operations_per_instruction = 1;
  }
  params_.default_is_stmt = ReadFixed<uint8_t>(&data);
  params_.line_base = ReadFixed<int8_t>(&data);
  params_.line_range = ReadFixed<uint8_t>(&data);
  params_.opcode_base = ReadFixed<uint8_t>(&data);
  if (params_.line_range == 0) {
    THROW("line_range of zero will cause divide by zero");
  }

  standard_opcode_lengths_.resize(params_.opcode_base);
  for (size_t i = 1; i < params_.opcode_base; i++) {
    standard_opcode_lengths_[i] = ReadFixed<uint8_t>(&data);
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

    uint8_t op = ReadFixed<uint8_t>(&data);

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
          uint8_t extended_op = ReadFixed<uint8_t>(&data);
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
          info_.address += ReadFixed<uint16_t>(&data);
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

string_view* File::GetFieldByName(string_view name) {
  if (name == "aranges") {
    return &debug_aranges;
  } else if (name == "addr") {
    return &debug_addr;
  } else if (name == "str") {
    return &debug_str;
  } else if (name == "str_offsets") {
    return &debug_str_offsets;
  } else if (name == "info") {
    return &debug_info;
  } else if (name == "types") {
    return &debug_types;
  } else if (name == "abbrev") {
    return &debug_abbrev;
  } else if (name == "line") {
    return &debug_line;
  } else if (name == "loc") {
    return &debug_loc;
  } else if (name == "pubnames") {
    return &debug_pubnames;
  } else if (name == "pubtypes") {
    return &debug_pubtypes;
  } else if (name == "ranges") {
    return &debug_ranges;
  } else if (name == "rnglists") {
    return &debug_rnglists;
  } else {
    return nullptr;
  }
}

}  // namespace dwarf

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
        : info_reader_(file),
          missing_("[DWARF is missing filename]") {}

    std::string GetFilename(uint64_t compilation_unit_offset) {
      auto& name = map_[compilation_unit_offset];
      if (name.empty()) {
        name = LookupFilename(compilation_unit_offset);
      }
      return name;
    }

   private:
    bool ReadName(std::string* name, uint64_t offset) {
      auto sec = dwarf::InfoReader::Section::kDebugInfo;
      dwarf::CUIter iter = info_reader_.GetCUIter(sec, offset);
      dwarf::CU cu;
      if (!iter.NextCU(info_reader_, &cu)) {
          return false;
      }
      *name = cu.unit_name();
      return true;
    }

    std::string LookupFilename(uint64_t compilation_unit_offset) {
      std::string name;
      if (ReadName(&name, compilation_unit_offset)) {
        return name;
      } else {
        return missing_;
      }
    }

    dwarf::InfoReader info_reader_;
    std::unordered_map<uint64_t, std::string> map_;
    std::string missing_;
  } map(file);

  dwarf::AddressRanges ranges(file.debug_aranges);

  while (ranges.NextUnit()) {
    std::string filename = map.GetFilename(ranges.debug_info_offset());

    while (ranges.NextRange()) {
      if (dwarf::IsValidDwarfAddress(ranges.address(), ranges.address_size())) {
        sink->AddVMRangeIgnoreDuplicate("dwarf_aranges", ranges.address(),
                                        ranges.length(), filename);
      }
    }
  }

  return true;
}

struct GeneralDIE {
  absl::optional<string_view> name;
  absl::optional<string_view> linkage_name;
  absl::optional<string_view> location_string;
  absl::optional<uint64_t> location_uint64;
  absl::optional<uint64_t> low_pc;
  absl::optional<uint64_t> high_pc;
  absl::optional<uint64_t> stmt_list;
  absl::optional<uint64_t> rnglistx;
  absl::optional<uint64_t> ranges;
  absl::optional<uint64_t> start_scope;
};

void ReadGeneralDIEAttr(uint16_t tag, dwarf::AttrValue val, const dwarf::CU& cu,
                        GeneralDIE* die) {
  switch (tag) {
    case DW_AT_name:
      if (val.IsString()) {
        die->name = val.GetString(cu);
      }
      break;
    case DW_AT_linkage_name:
      if (val.IsString()) die->linkage_name = val.GetString(cu);
      break;
    case DW_AT_location:
      if (val.IsString()) {
        die->location_string = val.GetString(cu);
      } else {
        die->location_uint64 = val.GetUint(cu);
      }
      break;
    case DW_AT_low_pc:
      if (auto uint = val.ToUint(cu)) {
        die->low_pc = *uint;
      }
      break;
    case DW_AT_high_pc:
      if (auto uint = val.ToUint(cu)) {
        die->high_pc = *uint;
      }
      break;
    case DW_AT_stmt_list:
      if (auto uint = val.ToUint(cu)) {
        die->stmt_list = *uint;
      }
      break;
    case DW_AT_ranges:
      if (auto uint = val.ToUint(cu)) {
        if (val.form() == DW_FORM_rnglistx) {
          die->rnglistx = *uint;
        } else {
          die->ranges = *uint;
        }
      }
      break;
    case DW_AT_start_scope:
      if (auto uint = val.ToUint(cu)) {
        die->start_scope = *uint;
      }
      break;
  }
}

// To view DIEs for a given file, try:
//   readelf --debug-dump=info foo.bin
void AddDIE(const dwarf::CU& cu, const GeneralDIE& die,
            const SymbolTable& symtab, const DualMap& symbol_map,
            RangeSink* sink) {
  uint64_t low_pc = 0;
  // Some DIEs mark address ranges with high_pc/low_pc pairs (especially
  // functions).
  if (die.low_pc && die.high_pc &&
      dwarf::IsValidDwarfAddress(*die.low_pc, cu.unit_sizes().address_size())) {
    uint64_t high_pc = *die.high_pc;
    low_pc = *die.low_pc;

    // It appears that some compilers make high_pc a size, and others make it an
    // address.
    if (high_pc >= low_pc) {
      high_pc -= low_pc;
    }

    sink->AddVMRangeIgnoreDuplicate("dwarf_pcpair", low_pc, high_pc, cu.unit_name());
  }

  // Sometimes a DIE has a linkage_name, which we can look up in the symbol
  // table.
  if (die.linkage_name) {
    auto it = symtab.find(*die.linkage_name);
    if (it != symtab.end()) {
      sink->AddVMRangeIgnoreDuplicate("dwarf_linkagename", it->second.first,
                                      it->second.second, cu.unit_name());
    }
  }

  // Sometimes the DIE has a "location", which gives the location as an address.
  // This parses a very small subset of the overall DWARF expression grammar.
  if (die.location_string) {
    string_view location = *die.location_string;
    if (location.size() == cu.unit_sizes().address_size() + 1 &&
        location[0] == DW_OP_addr) {
      location.remove_prefix(1);
      uint64_t addr;
      // TODO(haberman): endian?
      if (cu.unit_sizes().address_size() == 4) {
        addr = ReadFixed<uint32_t>(&location);
      } else if (cu.unit_sizes().address_size() == 8) {
        addr = ReadFixed<uint64_t>(&location);
      } else {
        BLOATY_UNREACHABLE();
      }

      // Unfortunately the location doesn't include a size, so we look that part
      // up in the symbol map.
      uint64_t size;
      if (symbol_map.vm_map.TryGetSize(addr, &size)) {
        sink->AddVMRangeIgnoreDuplicate("dwarf_location", addr, size,
                                        cu.unit_name());
      } else {
        if (verbose_level > 0) {
          fprintf(stderr,
                  "bloaty: warning: couldn't find DWARF location in symbol "
                  "table, address: %" PRIx64 ", name: %s\n",
                  addr, cu.unit_name().c_str());
        }
      }
    }
  }

  // Sometimes a location is given as an offset into debug_loc.
  if (die.location_uint64) {
    uint64_t location = *die.location_uint64;;
    if (location < cu.dwarf().debug_loc.size()) {
      absl::string_view loc_range = cu.dwarf().debug_loc.substr(location);
      loc_range = GetLocationListRange(cu.unit_sizes(), loc_range);
      sink->AddFileRange("dwarf_locrange", cu.unit_name(), loc_range);
    } else if (verbose_level > 0) {
      fprintf(stderr,
              "bloaty: warning: DWARF location out of range, location=%" PRIx64
              "\n",
              location);
    }
  }

  // DWARF 5 range list is the same information as "ranges" but in a different
  // format.
  if (die.rnglistx) {
    uint64_t range_list = *die.rnglistx;
    string_view offset_data =
        StrictSubstr(cu.dwarf().debug_rnglists,
                     cu.range_lists_base() + range_list);
    uint64_t offset = cu.unit_sizes().ReadDWARFOffset(&offset_data);
    string_view data = StrictSubstr(
        cu.dwarf().debug_rnglists, cu.range_lists_base() + offset);
    const char* start = data.data();
    bool done = false;
    uint64_t base_address = cu.addr_base();
    while (!done) {
      switch (ReadFixed<uint8_t>(&data)) {
        case DW_RLE_end_of_list:
          done = true;
          break;
        case DW_RLE_base_addressx:
          base_address =
              ReadIndirectAddress(cu, dwarf::ReadLEB128<uint64_t>(&data));
          break;
        case DW_RLE_startx_endx: {
          uint64_t start =
              ReadIndirectAddress(cu, dwarf::ReadLEB128<uint64_t>(&data));
          uint64_t end =
              ReadIndirectAddress(cu, dwarf::ReadLEB128<uint64_t>(&data));
          sink->AddVMRangeIgnoreDuplicate("dwarf_rangelst", start, end - start,
                                          cu.unit_name());
          break;
        }
        case DW_RLE_startx_length: {
          uint64_t start =
              ReadIndirectAddress(cu, dwarf::ReadLEB128<uint64_t>(&data));
          uint64_t length = dwarf::ReadLEB128<uint64_t>(&data);
          sink->AddVMRangeIgnoreDuplicate("dwarf_rangelst", start, length,
                                          cu.unit_name());
          break;
        }
        case DW_RLE_offset_pair: {
          uint64_t start = dwarf::ReadLEB128<uint64_t>(&data) + base_address;
          uint64_t end = dwarf::ReadLEB128<uint64_t>(&data) + base_address;
          sink->AddVMRangeIgnoreDuplicate("dwarf_rangelst", start, end - start,
                                          cu.unit_name());
          break;
        }
        case DW_RLE_base_address:
        case DW_RLE_start_end:
        case DW_RLE_start_length:
          THROW("NYI");
          break;
      }
    }
    string_view all(start, data.data() - start);
    sink->AddFileRange("dwarf_rangelst_addrs", cu.unit_name(), all);
  } else {
    uint64_t ranges_offset = UINT64_MAX;

    // There are two different attributes that sometimes contain an offset into
    // debug_ranges.
    if (die.ranges) {
      ranges_offset = *die.ranges;
    } else if (die.start_scope) {
      ranges_offset = *die.start_scope;
    }

    if (ranges_offset != UINT64_MAX) {
      if (ranges_offset < cu.dwarf().debug_ranges.size()) {
        absl::string_view data = cu.dwarf().debug_ranges.substr(ranges_offset);
        const char* start = data.data();
        ReadRangeList(cu, low_pc, cu.unit_name(), sink, &data);
        string_view all(start, data.data() - start);
        sink->AddFileRange("dwarf_debugrange", cu.unit_name(), all);
      } else if (verbose_level > 0) {
        fprintf(stderr,
                "bloaty: warning: DWARF debug range out of range, "
                "ranges_offset=%" PRIx64 "\n",
                ranges_offset);
      }
    }
  }
}

static void ReadDWARFPubNames(dwarf::InfoReader& reader, string_view section,
                              RangeSink* sink) {
  string_view remaining = section;

  while (remaining.size() > 0) {
    dwarf::CompilationUnitSizes sizes;
    string_view full_unit = remaining;
    string_view unit = sizes.ReadInitialLength(&remaining);
    full_unit =
        full_unit.substr(0, unit.size() + (unit.data() - full_unit.data()));
    sizes.ReadDWARFVersion(&unit);
    uint64_t debug_info_offset = sizes.ReadDWARFOffset(&unit);

    dwarf::CUIter iter = reader.GetCUIter(
        dwarf::InfoReader::Section::kDebugInfo, debug_info_offset);
    dwarf::CU cu;
    if (iter.NextCU(reader, &cu) && !cu.unit_name().empty()) {
      sink->AddFileRange("dwarf_pubnames", cu.unit_name(), full_unit);
    }
  }
}

uint64_t ReadEncodedPointer(uint8_t encoding, bool is_64bit, string_view* data,
                            const char* data_base, RangeSink* sink) {
  uint64_t value;
  const char* ptr = data->data();
  uint8_t format = encoding & DW_EH_PE_FORMAT_MASK;

  switch (format) {
    case DW_EH_PE_omit:
      return 0;
    case DW_EH_PE_absptr:
      if (is_64bit) {
        value = ReadFixed<uint64_t>(data);
      } else {
        value = ReadFixed<uint32_t>(data);
      }
      break;
    case DW_EH_PE_uleb128:
      value = dwarf::ReadLEB128<uint64_t>(data);
      break;
    case DW_EH_PE_udata2:
      value = ReadFixed<uint16_t>(data);
      break;
    case DW_EH_PE_udata4:
      value = ReadFixed<uint32_t>(data);
      break;
    case DW_EH_PE_udata8:
      value = ReadFixed<uint64_t>(data);
      break;
    case DW_EH_PE_sleb128:
      value = dwarf::ReadLEB128<int64_t>(data);
      break;
    case DW_EH_PE_sdata2:
      value = ReadFixed<int16_t>(data);
      break;
    case DW_EH_PE_sdata4:
      value = ReadFixed<int32_t>(data);
      break;
    case DW_EH_PE_sdata8:
      value = ReadFixed<int64_t>(data);
      break;
    default:
      THROWF("Unexpected eh_frame format value: $0", format);
  }

  uint8_t application = encoding & DW_EH_PE_APPLICATION_MASK;

  switch (application) {
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
      value = ReadFixed<uint64_t>(&location);
    } else {
      value = ReadFixed<uint32_t>(&location);
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
// *
// http://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
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

  std::unordered_map<const void*, CIEInfo> cie_map;

  while (remaining.size() > 0) {
    dwarf::CompilationUnitSizes sizes;
    string_view full_entry = remaining;
    string_view entry = sizes.ReadInitialLength(&remaining);
    if (entry.size() == 0 && remaining.size() == 0) {
      return;
    }
    full_entry =
        full_entry.substr(0, entry.size() + (entry.data() - full_entry.data()));
    uint32_t id = ReadFixed<uint32_t>(&entry);
    if (id == 0) {
      // CIE, we don't attribute this yet.
      CIEInfo& cie_info = cie_map[full_entry.data()];
      cie_info.version = ReadFixed<uint8_t>(&entry);
      string_view aug_string = ReadNullTerminated(&entry);
      cie_info.code_align = dwarf::ReadLEB128<uint32_t>(&entry);
      cie_info.data_align = dwarf::ReadLEB128<int32_t>(&entry);
      switch (cie_info.version) {
        case 1:
          cie_info.return_address_reg = ReadFixed<uint8_t>(&entry);
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
            cie_info.lsda_encoding = ReadFixed<uint8_t>(&entry);
            break;
          case 'R':
            cie_info.fde_encoding = ReadFixed<uint8_t>(&entry);
            break;
          case 'S':
            cie_info.is_signal_handler = true;
            break;
          case 'P': {
            uint8_t encoding = ReadFixed<uint8_t>(&entry);
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

      sink->AddFileRangeForVMAddr("dwarf_fde", address, full_entry);
    }
  }
}

// See documentation here:
//   http://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html#EHFRAME
void ReadEhFrameHdr(string_view data, RangeSink* sink) {
  const char* base = data.data();
  uint8_t version = ReadFixed<uint8_t>(&data);
  uint8_t eh_frame_ptr_enc = ReadFixed<uint8_t>(&data);
  uint8_t fde_count_enc = ReadFixed<uint8_t>(&data);
  uint8_t table_enc = ReadFixed<uint8_t>(&data);

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
    sink->AddFileRangeForVMAddr("dwarf_fde_table", initial_location,
                                entry_data);

    // We could add fde_addr with an unknown length if we wanted to skip reading
    // eh_frame.  We can't count on this table being available though, so we
    // don't want to remove the eh_frame reading code altogether.
    (void)fde_addr;
  }
}

static void ReadDWARFStmtListRange(const dwarf::CU& cu, uint64_t offset,
                                   RangeSink* sink) {
  string_view data = cu.dwarf().debug_line;
  SkipBytes(offset, &data);
  string_view data_with_length = data;
  dwarf::CompilationUnitSizes sizes;
  data = sizes.ReadInitialLength(&data);
  data = data_with_length.substr(
      0, data.size() + (data.data() - data_with_length.data()));
  sink->AddFileRange("dwarf_stmtlistrange", cu.unit_name(), data);
}

// The DWARF debug info can help us get compileunits info.  DIEs for compilation
// units, functions, and global variables often have attributes that will
// resolve to addresses.
static void ReadDWARFDebugInfo(dwarf::InfoReader& reader,
                               dwarf::InfoReader::Section section,
                               const SymbolTable& symtab,
                               const DualMap& symbol_map, RangeSink* sink) {
  dwarf::CUIter iter = reader.GetCUIter(section);
  dwarf::CU cu;
  cu.set_strp_sink(sink);

  while (iter.NextCU(reader, &cu)) {
    dwarf::DIEReader die_reader = cu.GetDIEReader();
    GeneralDIE compileunit_die;
    auto* abbrev = die_reader.ReadCode(cu);
    die_reader.ReadAttributes(
        cu, abbrev,
        [&cu, &compileunit_die](uint16_t tag, dwarf::AttrValue value) {
          ReadGeneralDIEAttr(tag, value, cu, &compileunit_die);
        });

    if (cu.unit_name().empty()) {
      continue;
    }

    sink->AddFileRange("dwarf_debuginfo", cu.unit_name(), cu.unit_range());
    AddDIE(cu, compileunit_die, symtab, symbol_map, sink);

    if (compileunit_die.stmt_list) {
      ReadDWARFStmtListRange(cu, *compileunit_die.stmt_list, sink);
    }

    sink->AddFileRange("dwarf_abbrev", cu.unit_name(), cu.unit_abbrev().abbrev_data());

    while (auto abbrev = die_reader.ReadCode(cu)) {
      GeneralDIE die;
      die_reader.ReadAttributes(
          cu, abbrev, [&cu, &die](uint16_t tag, dwarf::AttrValue value) {
            ReadGeneralDIEAttr(tag, value, cu, &die);
          });

      // low_pc == 0 is a signal that this routine was stripped out of the
      // final binary.  Skip this DIE and all of its children.
      if (die.low_pc && *die.low_pc == 0) {
        die_reader.SkipChildren(cu, abbrev);
      } else {
        AddDIE(cu, die, symtab, symbol_map, sink);
      }
    }
  }
}

void ReadDWARFCompileUnits(const dwarf::File& file, const SymbolTable& symtab,
                           const DualMap& symbol_map, RangeSink* sink) {
  if (!file.debug_info.size()) {
    THROW("missing debug info");
  }

  if (file.debug_aranges.size()) {
    ReadDWARFAddressRanges(file, sink);
  }

  // Share a reader to avoid re-parsing debug abbreviations.
  dwarf::InfoReader reader(file);

  ReadDWARFDebugInfo(reader, dwarf::InfoReader::Section::kDebugInfo, symtab,
                     symbol_map, sink);
  ReadDWARFDebugInfo(reader, dwarf::InfoReader::Section::kDebugTypes, symtab,
                     symbol_map, sink);
  ReadDWARFPubNames(reader, file.debug_pubnames, sink);
  ReadDWARFPubNames(reader, file.debug_pubtypes, sink);
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

  dwarf::InfoReader reader(file);
  dwarf::CUIter iter = reader.GetCUIter(dwarf::InfoReader::Section::kDebugInfo);
  dwarf::CU cu;
  dwarf::DIEReader die_reader = cu.GetDIEReader();
  dwarf::LineInfoReader line_info_reader(file);

  if (!iter.NextCU(reader, &cu)) {
    THROW("debug info is present, but empty");
  }

  while (auto abbrev = die_reader.ReadCode(cu)) {
    absl::optional<uint64_t> stmt_list;
    die_reader.ReadAttributes(
        cu, abbrev, [&stmt_list, &cu](uint16_t tag, dwarf::AttrValue val) {
          if (tag == DW_AT_stmt_list) {
            stmt_list = val.ToUint(cu);
          }
        });

    if (stmt_list) {
      line_info_reader.SeekToOffset(*stmt_list, cu.unit_sizes().address_size());
      ReadDWARFStmtList(include_line, &line_info_reader, sink);
    }
  }
}

} // namespace bloaty
