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
#include "dwarf/attr.h"
#include "dwarf/dwarf_util.h"
#include "dwarf/line_info.h"

using namespace dwarf2reader;
using absl::string_view;

namespace bloaty {

extern int verbose_level;

namespace dwarf {

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
  // The range of the file where this data occurs.
  string_view data() { return data_; }

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
  string_view data_;
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

  const char* start = unit_remaining_.data();
  address_ = sizes_.ReadAddress(&unit_remaining_);
  length_ = sizes_.ReadAddress(&unit_remaining_);
  data_ = string_view(start, unit_remaining_.data() - start);
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

// RangeList ///////////////////////////////////////////////////////////////////

void ReadRangeList(const CU& cu, uint64_t low_pc, string_view name,
                   RangeSink* sink, string_view* data) {
  std::string name_str(name);
  uint64_t max_address = cu.unit_sizes().MaxAddress();
  while (true) {
    uint64_t start, end;
    start = cu.unit_sizes().ReadAddress(data);
    end = cu.unit_sizes().ReadAddress(data);
    if (start == 0 && end == 0) {
      return;
    } else if (start == max_address) {
      low_pc = end;
    } else {
      uint64_t size = end - start;
      sink->AddVMRangeIgnoreDuplicate("dwarf_rangelist", low_pc + start, size,
                                      name_str);
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
      sink->AddFileRange("dwarf_aranges_data", filename, ranges.data());
    }
  }

  return true;
}

struct GeneralDIE {
  absl::optional<string_view> name;
  absl::optional<string_view> location_string;
  absl::optional<uint64_t> location_uint64;
  absl::optional<uint64_t> low_pc;
  absl::optional<uint64_t> high_pc_addr;
  absl::optional<uint64_t> high_pc_size;
  absl::optional<uint64_t> stmt_list;
  absl::optional<uint64_t> rnglistx;
  absl::optional<uint64_t> ranges;
  absl::optional<uint64_t> start_scope;
  bool declaration = false;
};

void ReadGeneralDIEAttr(uint16_t tag, dwarf::AttrValue val, const dwarf::CU& cu,
                        GeneralDIE* die) {
  switch (tag) {
    case DW_AT_name:
      if (val.IsString()) {
        die->name = val.GetString(cu);
      }
      break;
    case DW_AT_declaration:
      if (auto uint = val.ToUint(cu)) {
        die->declaration = *uint;
      }
      break;
    case DW_AT_location:
      if (val.IsString()) {
        die->location_string = val.GetString(cu);
      } else if (val.form() == DW_FORM_sec_offset) {
        die->location_uint64 = val.GetUint(cu);
      }
      break;
    case DW_AT_low_pc:
      if (auto uint = val.ToUint(cu)) {
        die->low_pc = *uint;
      }
      break;
    case DW_AT_high_pc:
      switch (val.form()) {
        case DW_FORM_addr:
        case DW_FORM_addrx:
        case DW_FORM_addrx1:
        case DW_FORM_addrx2:
        case DW_FORM_addrx3:
        case DW_FORM_addrx4:
          // high_pc is absolute.
          die->high_pc_addr = val.GetUint(cu);
          break;
        case DW_FORM_data1:
        case DW_FORM_data2:
        case DW_FORM_data4:
        case DW_FORM_data8:
          // high_pc is a size.
          die->high_pc_size = val.ToUint(cu);
          break;
        default:
          if (verbose_level > 0) {
            fprintf(stderr, "Unexpected form for high_pc: %d\n", val.form());
          }
          break;
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

uint64_t TryReadPcPair(const dwarf::CU& cu, const GeneralDIE& die,
                       RangeSink* sink) {
  uint64_t addr;
  uint64_t size;

  if (!die.low_pc) return 0;
  addr = *die.low_pc;

  if (die.high_pc_addr) {
    size = *die.high_pc_addr - addr;
  } else if (die.high_pc_size) {
    size = *die.high_pc_size;
  } else{
    return 0;
  }

  sink->AddVMRangeIgnoreDuplicate("dwarf_pcpair", addr, size, cu.unit_name());
  return addr;
}

// To view DIEs for a given file, try:
//   readelf --debug-dump=info foo.bin
void AddDIE(const dwarf::CU& cu, const GeneralDIE& die,
            const DualMap& symbol_map, RangeSink* sink) {
  uint64_t low_pc = TryReadPcPair(cu, die, sink);

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
    size_t offset_size = cu.unit_sizes().dwarf64() ? 8 : 4;
    string_view offset_data =
        StrictSubstr(cu.dwarf().debug_rnglists,
                     cu.range_lists_base() + (range_list * offset_size));
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
                               const DualMap& symbol_map, RangeSink* sink) {
  dwarf::CUIter iter = reader.GetCUIter(section);
  dwarf::CU cu;
  cu.SetIndirectStringCallback([sink, &cu](string_view str) {
    sink->AddFileRange("dwarf_strp", cu.unit_name(), str);
  });

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

    sink->AddFileRange("dwarf_debuginfo", cu.unit_name(), cu.entire_unit());
    AddDIE(cu, compileunit_die, symbol_map, sink);

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
      // final binary. Also any declaration should be skipped.
      if ((die.low_pc && !cu.IsValidDwarfAddress(*die.low_pc)) ||
          die.declaration) {
        die_reader.SkipChildren(cu, abbrev);
      } else {
        AddDIE(cu, die, symbol_map, sink);
      }
    }
  }
}

void ReadDWARFCompileUnits(const dwarf::File& file, const DualMap& symbol_map,
                           RangeSink* sink) {
  if (!file.debug_info.size()) {
    THROW("missing debug info");
  }

  if (file.debug_aranges.size()) {
    ReadDWARFAddressRanges(file, sink);
  }

  // Share a reader to avoid re-parsing debug abbreviations.
  dwarf::InfoReader reader(file);

  ReadDWARFDebugInfo(reader, dwarf::InfoReader::Section::kDebugInfo, symbol_map,
                     sink);
  ReadDWARFDebugInfo(reader, dwarf::InfoReader::Section::kDebugTypes,
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
