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

#include "dwarf/line_info.h"

#include "dwarf/dwarf_util.h"
#include "dwarf_constants.h"

using namespace dwarf2reader;
using absl::string_view;

namespace bloaty {

extern int verbose_level;

namespace dwarf {

const std::string& LineInfoReader::GetExpandedFilename(size_t index) {
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
    absl::string_view directory = include_directories_[filename.directory_index];
    ret = std::string(directory);
    if (!ret.empty()) {
      ret += "/";
    }
    ret += std::string(filename.name);
  }
  return ret;
}

void LineInfoReader::Advance(uint64_t amount) {
  if (params_.maximum_operations_per_instruction == 1) {
    // This is by far the common case (only false on VLIW architectuers),
    // and this inlining/specialization avoids a costly division.
    DoAdvance(amount, 1);
  } else {
    DoAdvance(amount, params_.maximum_operations_per_instruction);
  }
}

void LineInfoReader::DoAdvance(uint64_t advance, uint8_t max_per_instr) {
  info_.address += params_.minimum_instruction_length *
                    ((info_.op_index + advance) / max_per_instr);
  info_.op_index = (info_.op_index + advance) % max_per_instr;
}

void LineInfoReader::SpecialOpcodeAdvance(uint8_t op) {
  Advance(AdjustedOpcode(op) / params_.line_range);
}

uint8_t LineInfoReader::AdjustedOpcode(uint8_t op) {
  return op - params_.opcode_base;
}

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

}  // namespace dwarf
}  // namespace bloaty
