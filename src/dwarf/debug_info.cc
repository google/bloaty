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

#include "dwarf/debug_info.h"
#include "dwarf_constants.h"
#include "dwarf/dwarf_util.h"

using namespace dwarf2reader;

using absl::string_view;

namespace bloaty {
namespace dwarf {

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

absl::string_view CompilationUnitSizes::ReadInitialLength(
    absl::string_view* remaining) {
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

  absl::string_view unit = *remaining;
  unit.remove_suffix(remaining->size() - len);
  *remaining = remaining->substr(len);
  return unit;
}

CUIter InfoReader::GetCUIter(Section section, uint64_t offset) {
  string_view data;

  if (section == Section::kDebugInfo) {
    data = dwarf_.debug_info;
  } else {
    data = dwarf_.debug_types;
  }

  SkipBytes(offset, &data);
  return CUIter(section, data);
}

bool CUIter::NextCU(InfoReader& reader, CU* cu) {
  if (next_unit_.empty()) return false;

  // Read initial length and calculate entire_unit/data.
  string_view entire_unit = next_unit_;
  string_view data = cu->unit_sizes_.ReadInitialLength(&next_unit_);
  size_t initial_length_len = data.data() - entire_unit.data();
  entire_unit = entire_unit.substr(0, data.size() + initial_length_len);

  // Delegate to CU to read the unit header.
  cu->ReadHeader(entire_unit, data, section_, reader);
  return true;
}

// Reads the header of this CU from |data|, updating our member variables
// according to what was parsed.
void CU::ReadHeader(string_view entire_unit, string_view data,
                    InfoReader::Section section, InfoReader& reader) {
  entire_unit_ = entire_unit;
  dwarf_ = &reader.dwarf_;
  unit_sizes_.ReadDWARFVersion(&data);

  if (unit_sizes_.dwarf_version() > 5) {
    THROWF("Data is in DWARF $0 format which we don't understand",
           unit_sizes_.dwarf_version());
  }

  uint64_t debug_abbrev_offset;

  if (unit_sizes_.dwarf_version() == 5) {
    unit_type_ = ReadFixed<uint8_t>(&data);
    unit_sizes_.SetAddressSize(ReadFixed<uint8_t>(&data));
    debug_abbrev_offset = unit_sizes_.ReadDWARFOffset(&data);

    switch (unit_type_) {
      case DW_UT_skeleton:
      case DW_UT_split_compile:
      case DW_UT_split_type:
        dwo_id_ = ReadFixed<uint64_t>(&data);
        break;
      case DW_UT_type:
        unit_type_signature_ = ReadFixed<uint64_t>(&data);
        unit_type_offset_ = unit_sizes_.ReadDWARFOffset(&data);
        break;
      case DW_UT_compile:
      case DW_UT_partial:
        break;
      default:
        fprintf(stderr, "warning: Unknown DWARF Unit Type in user defined range\n");
        break;
    }

  } else {
    debug_abbrev_offset = unit_sizes_.ReadDWARFOffset(&data);
    unit_sizes_.SetAddressSize(ReadFixed<uint8_t>(&data));

    if (section == InfoReader::Section::kDebugTypes) {
      unit_type_signature_ = ReadFixed<uint64_t>(&data);
      unit_type_offset_ = unit_sizes_.ReadDWARFOffset(&data);
    }
  }

  unit_abbrev_ = &reader.abbrev_tables_[debug_abbrev_offset];

  // If we haven't already read abbreviations for this debug_abbrev_offset_, we
  // need to do so now.
  if (unit_abbrev_->IsEmpty()) {
    string_view abbrev_data = dwarf_->debug_abbrev;
    SkipBytes(debug_abbrev_offset, &abbrev_data);
    unit_abbrev_->ReadAbbrevs(abbrev_data);
  }

  data_ = data;
  ReadTopLevelDIE(reader);
}

// Read the root-level DIE in order to populate some member variables on which
// other attributes depend. In particular, we may re-parse this DIE later and
// read attributes that are relative to these base addresses.
void CU::ReadTopLevelDIE(InfoReader& reader) {
  DIEReader die_reader = GetDIEReader();
  const auto* abbrev = die_reader.ReadCode(*this);
  absl::optional<uint64_t> stmt_list;
  unit_name_.clear();
  die_reader.ReadAttributes(
      *this, abbrev, [this, &stmt_list](uint16_t tag, dwarf::AttrValue value) {
        switch (tag) {
          case DW_AT_name:
            if (value.IsString()) {
              unit_name_ = std::string(value.GetString(*this));
            }
            break;
          case DW_AT_stmt_list:
            if (value.form() == DW_FORM_sec_offset) {
              stmt_list = value.GetUint(*this);
            }
            break;
          case DW_AT_addr_base:
            if (value.form() == DW_FORM_sec_offset) {
              addr_base_ = value.GetUint(*this);
            }
            break;
          case DW_AT_str_offsets_base:
            if (value.form() == DW_FORM_sec_offset) {
              str_offsets_base_ = value.GetUint(*this);
            }
            break;
          case DW_AT_rnglists_base:
            if (value.form() == DW_FORM_sec_offset) {
              range_lists_base_ = value.GetUint(*this);
            }
            break;
        }
      });

  if (stmt_list) {
    if (unit_name_.empty()) {
      auto iter = reader.stmt_list_map_.find(*stmt_list);
      if (iter != reader.stmt_list_map_.end()) {
        unit_name_ = iter->second;
      }
    } else {
      (reader.stmt_list_map_)[*stmt_list] = unit_name_;
    }
  }
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

}  // namespace dwarf
}  // namespace bloaty
