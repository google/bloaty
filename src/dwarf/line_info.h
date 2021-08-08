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

#ifndef BLOATY_DWARF_LINE_INFO_H_
#define BLOATY_DWARF_LINE_INFO_H_

#include <cstdint>

#include "absl/strings/string_view.h"
#include "dwarf/debug_info.h"

// Code to read the .line_info programs in a DWARF file.  Currently we use this
// for the "inlines" data source, but I think we should probably use the
// inlining info in debug_info instead.
//
// Usage overview:
//   dwarf::LineInfoReader reader(file);
//   
//   reader.SeekToOffset(ofs, cu.unit_sizes().address_size());
//   while (reader->ReadLineInfo()) {
//     const dwarf::LineInfo& info = reader->lineinfo();
//     // ...
//   }

namespace bloaty {
namespace dwarf {

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
    absl::string_view name;
    uint32_t directory_index;
    uint64_t modified_time;
    uint64_t file_size;
  };

  void SeekToOffset(uint64_t offset, uint8_t address_size);
  bool ReadLineInfo();
  const LineInfo& lineinfo() const { return info_; }
  const FileName& filename(size_t i) const { return filenames_[i]; }
  absl::string_view include_directory(size_t i) const {
    return include_directories_[i];
  }

  const std::string& GetExpandedFilename(size_t index);

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
  std::vector<absl::string_view> include_directories_;
  std::vector<FileName> filenames_;
  std::vector<uint8_t> standard_opcode_lengths_;
  std::vector<std::string> expanded_filenames_;

  absl::string_view remaining_;

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

  void DoAdvance(uint64_t advance, uint8_t max_per_instr);
  void Advance(uint64_t amount);
  uint8_t AdjustedOpcode(uint8_t op);
  void SpecialOpcodeAdvance(uint8_t op);
};

}  // namespace dwarf
}  // namespace bloaty

#endif  // BLOATY_DWARF_LINE_INFO_H_
