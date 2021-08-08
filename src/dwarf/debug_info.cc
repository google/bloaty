
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

  CompilationUnitSizes unit_sizes;
  string_view unit_range = next_unit_;
  string_view data_range = unit_sizes.ReadInitialLength(&next_unit_);
  size_t initial_length_len = data_range.data() - unit_range.data();
  unit_range = unit_range.substr(0, data_range.size() + initial_length_len);

  unit_sizes.ReadDWARFVersion(&data_range);

  if (unit_sizes.dwarf_version() > 5) {
    THROWF("Data is in DWARF $0 format which we don't understand",
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
  cu->unit_range_ = unit_range;

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
            if (value.form() == DW_FORM_sec_offset) {
              stmt_list = value.GetUint(*cu);
            }
            break;
          case DW_AT_addr_base:
            if (value.form() == DW_FORM_sec_offset) {
              cu->addr_base_ = value.GetUint(*cu);
            }
            break;
          case DW_AT_str_offsets_base:
            if (value.form() == DW_FORM_sec_offset) {
              cu->str_offsets_base_ = value.GetUint(*cu);
            }
            break;
          case DW_AT_rnglists_base:
            if (value.form() == DW_FORM_sec_offset) {
              cu->range_lists_base_ = value.GetUint(*cu);
            }
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

}  // namespace dwarf
}  // namespace bloaty
