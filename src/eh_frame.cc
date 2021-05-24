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

#include "bloaty.h"
#include "util.h"
#include "absl/strings/string_view.h"
#include "dwarf_constants.h"
#include "dwarf/dwarf_util.h"

using absl::string_view;
using namespace dwarf2reader;

namespace bloaty {

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

}  // namespace bloaty
