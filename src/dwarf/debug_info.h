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
//
// Classes for reading .debug_info and .debug_types.
//
// Usage overview:
//
//   // Stores/caches abbreviation info and CU names.
//   dwarf::InfoReader reader;
//
//   // Iterator type for enumerating CUs. Initially positioned at the beginning
//   // of the given section unless you pass an explicit offset.
//   dwarf::CUIter iter = reader.GetCUIter(
//       dwarf::InfoReader::Section::kDebugInfo);
//
//   // Represents a single CU and vends a lot of useful data about it, like its
//   // name. Starts out empty/undefined until you call NextCU().
//   dwarf::CU cu;
//
//   while (iter.NextCU(reader, &cu)) {
//     std::cout << "Parsing CU with name=" << cu.unit_name() << "\n";
//
//     // Iterator for enumerating DIEs in a given CU.
//     dwarf::DIEReader die_reader = cu.GetDIEReader();
//     while (auto abbrev = die_reader.ReadCode(cu)) {
//       if (IsInteresting(abbrev->tag)) {
//         die_reader.ReadAttributes(
//             cu, abbrev, [](uint16_t tag, dwarf::AttrValue val) {
//            // Process attribute.
//         });
//       } else {
//         die_reader.SkipChildren(cu, abbrev);
//       }
//     }
//   }

#ifndef BLOATY_DWARF_DEBUG_INFO_H_
#define BLOATY_DWARF_DEBUG_INFO_H_

#include <functional>
#include <unordered_map>

#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "dwarf/attr.h"
#include "dwarf/dwarf_util.h"
#include "dwarf_constants.h"
#include "util.h"

namespace bloaty {
namespace dwarf {

struct File {
  absl::string_view debug_abbrev;
  absl::string_view debug_addr;
  absl::string_view debug_aranges;
  absl::string_view debug_info;
  absl::string_view debug_line;
  absl::string_view debug_loc;
  absl::string_view debug_pubnames;
  absl::string_view debug_pubtypes;
  absl::string_view debug_ranges;
  absl::string_view debug_rnglists;
  absl::string_view debug_str;
  absl::string_view debug_str_offsets;
  absl::string_view debug_types;

  absl::string_view* GetFieldByName(absl::string_view name);
  void SetFieldByName(absl::string_view name, absl::string_view contents) {
    absl::string_view *member = GetFieldByName(name);
    if (member) *member = contents;
  }
};

// A class that represents the DWARF version and address sizes for a given
// compilation unit.
class CompilationUnitSizes {
 public:
  // When true, DWARF offsets are 64 bits, otherwise they are 32 bit.
  bool dwarf64() const { return dwarf64_; }

  // The size of addresses.  Guaranteed to be either 4 or 8.
  uint8_t address_size() const { return addr8_ ? 8 : 4; }

  // DWARF version of this unit.
  uint8_t dwarf_version() const { return dwarf_version_; }

  void SetAddressSize(uint8_t address_size) {
    if (address_size != 4 && address_size != 8) {
      THROWF("Unexpected address size: $0", address_size);
    }
    addr8_ = address_size == 8;
  }

  // Reads a DWARF offset based on whether we are reading dwarf32 or dwarf64
  // format.
  uint64_t ReadDWARFOffset(absl::string_view* data) const {
    return dwarf64_ ? ReadFixed<uint64_t>(data) : ReadFixed<uint32_t>(data);
  }

  // Reads an address according to the expected address_size.
  uint64_t ReadAddress(absl::string_view* data) const {
    return addr8_ ? ReadFixed<uint64_t>(data) : ReadFixed<uint32_t>(data);
  }

  uint64_t MaxAddress() const {
    return addr8_ ? 0xffffffffffffffff : 0xffffffff;
  }

  // Reads an "initial length" as specified in many DWARF headers.  This
  // contains either a 32-bit or a 64-bit length, and signals whether we are
  // using the 32-bit or 64-bit DWARF format (so it sets dwarf64 appropriately).
  //
  // Returns the range for this section and stores the remaining data
  // in |remaining|.
  absl::string_view ReadInitialLength(absl::string_view* remaining);

  void ReadDWARFVersion(absl::string_view* data) {
    dwarf_version_ = ReadFixed<uint16_t>(data);
  }

 private:
  uint16_t dwarf_version_;
  bool dwarf64_;
  bool addr8_;
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
  void ReadAbbrevs(absl::string_view data);

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
  absl::string_view abbrev_data() const { return abbrev_data_; }

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
  absl::string_view abbrev_data_;
};

class CUIter;
class CU;
class DIEReader;

// Stores/caches abbreviation info and CU names.
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
  friend class CU;
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
  CUIter(InfoReader::Section section, absl::string_view next_unit)
      : section_(section), next_unit_(next_unit) {}

  // Data for the next compilation unit.
  InfoReader::Section section_;
  absl::string_view next_unit_;
};

// CompilationUnit: stores info about a single compilation unit in .debug_info
// or .debug_types.
class CU {
 public:
  DIEReader GetDIEReader();

  const File& dwarf() const { return *dwarf_; }
  const CompilationUnitSizes& unit_sizes() const { return unit_sizes_; }
  const std::string& unit_name() const { return unit_name_; }
  absl::string_view entire_unit() const { return entire_unit_; }
  uint64_t addr_base() const { return addr_base_; }
  uint64_t str_offsets_base() const { return str_offsets_base_; }
  uint64_t range_lists_base() const { return range_lists_base_; }
  const AbbrevTable& unit_abbrev() const { return *unit_abbrev_; }

  void AddIndirectString(absl::string_view range) const {
    if (strp_callback_) {
      strp_callback_(range);
    }
  }

  void SetIndirectStringCallback(
      std::function<void(absl::string_view)> strp_sink) {
    strp_callback_ = strp_sink;
  }

  bool IsValidDwarfAddress(uint64_t addr) const {
    return dwarf::IsValidDwarfAddress(addr, unit_sizes_.address_size());
  }

 private:
  friend class CUIter;
  friend class DIEReader;

  void ReadHeader(absl::string_view entire_unit, absl::string_view data,
                  InfoReader::Section section, InfoReader& reader);
  void ReadTopLevelDIE(InfoReader& reader);

  const File* dwarf_;

  // Info that comes from the CU header.
  absl::string_view entire_unit_;  // Entire CU's range.
  absl::string_view data_;         // Entire unit excluding CU header.
  CompilationUnitSizes unit_sizes_;
  AbbrevTable* unit_abbrev_;

  // Only for skeleton and split CUs.
  uint8_t unit_type_;
  uint64_t dwo_id_;

  // Only for .debug_types
  uint64_t unit_type_signature_;
  uint64_t unit_type_offset_;

  // Info that comes from the top-level DIE.
  std::string unit_name_;
  uint64_t addr_base_ = 0;
  uint64_t str_offsets_base_ = 0;
  uint64_t range_lists_base_ = 0;

  std::function<void(absl::string_view)> strp_callback_;
};

// DIEReader: for reading a sequence of Debugging Information Entries in a
// compilation unit.
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

  DIEReader(absl::string_view data) : remaining_(data) {}
  void SkipNullEntries();

  // Our current read position.
  absl::string_view remaining_;
  int depth_ = 0;
};

inline uint64_t ReadIndirectAddress(const CU& cu, uint64_t val) {
  absl::string_view addrs = cu.dwarf().debug_addr;
  switch (cu.unit_sizes().address_size()) {
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

// Reads all attributes for this DIE, calling the given function for each one.
template <class T>
void DIEReader::ReadAttributes(const CU& cu, const AbbrevTable::Abbrev* abbrev,
                               T&& func) {
  for (auto attr : abbrev->attr) {
    AttrValue value = AttrValue::ParseAttr(cu, attr.form, &remaining_);
    func(attr.name, value);
  }
}

inline DIEReader CU::GetDIEReader() { return DIEReader(data_); }

}  // namespace dwarf
}  // namespace bloaty

#endif  // BLOATY_DWARF_DEBUG_INFO_H_
