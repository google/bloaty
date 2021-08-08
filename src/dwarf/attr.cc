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

#include "dwarf/attr.h"

#include "dwarf/debug_info.h"
#include "dwarf/dwarf_util.h"
#include "dwarf_constants.h"
#include "util.h"

using string_view = absl::string_view;
using namespace dwarf2reader;

namespace bloaty {
namespace dwarf {

absl::optional<uint64_t> AttrValue::ToUint(const CU& cu) const {
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

uint64_t AttrValue::GetUint(const CU& cu) const {
  if (type_ == Type::kUnresolvedUint) {
    return ResolveIndirectAddress(cu);
  } else {
    assert(type_ == Type::kUint);
    return uint_;
  }
}

string_view AttrValue::GetString(const CU& cu) const {
  if (type_ == Type::kUnresolvedString) {
    return ResolveDoubleIndirectString(cu);
  } else {
    assert(type_ == Type::kString);
    return string_;
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
  string_view ret = ReadDebugStrEntry(cu.dwarf().debug_str, ofs);
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
  string_view ret = ReadDebugStrEntry(cu.dwarf().debug_str, ofs2);
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
      return AttrValue(form, ReadFixed<uint8_t>(data));
    case DW_FORM_ref2:
      return AttrValue(form, ReadFixed<uint16_t>(data));
    case DW_FORM_ref4:
      return AttrValue(form, ReadFixed<uint32_t>(data));
    case DW_FORM_ref_sig8:
    case DW_FORM_ref8:
      return AttrValue(form, ReadFixed<uint64_t>(data));
    case DW_FORM_ref_udata:
    case DW_FORM_strx1:
      return AttrValue::UnresolvedString(form, ReadFixed<uint8_t>(data));
    case DW_FORM_strx2:
      return AttrValue::UnresolvedString(form, ReadFixed<uint16_t>(data));
    case DW_FORM_strx4:
      return AttrValue::UnresolvedString(form, ReadFixed<uint32_t>(data));
    case DW_FORM_strx:
      return AttrValue::UnresolvedString(form, ReadLEB128<uint64_t>(data));
    case DW_FORM_addrx1:
      return AttrValue::UnresolvedUint(form, ReadFixed<uint8_t>(data));
    case DW_FORM_addrx2:
      return AttrValue::UnresolvedUint(form, ReadFixed<uint16_t>(data));
    case DW_FORM_addrx3:
      return AttrValue::UnresolvedUint(form, ReadFixed<uint32_t, 3>(data));
    case DW_FORM_addrx4:
      return AttrValue::UnresolvedUint(form, ReadFixed<uint32_t>(data));
    case DW_FORM_addrx:
      return AttrValue::UnresolvedUint(form, ReadLEB128<uint64_t>(data));
    case DW_FORM_addr:
    address_size:
      switch (cu.unit_sizes().address_size()) {
        case 4:
          return AttrValue(form, ReadFixed<uint32_t>(data));
        case 8:
          return AttrValue(form, ReadFixed<uint64_t>(data));
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
        return AttrValue(form, ReadFixed<uint64_t>(data));
      } else {
        return AttrValue(form, ReadFixed<uint32_t>(data));
      }
    case DW_FORM_udata:
      return AttrValue(form, ReadLEB128<uint64_t>(data));
    case DW_FORM_block1:
      return AttrValue(form, ReadBlock<uint8_t>(data));
    case DW_FORM_block2:
      return AttrValue(form, ReadBlock<uint16_t>(data));
    case DW_FORM_block4:
      return AttrValue(form, ReadBlock<uint32_t>(data));
    case DW_FORM_block:
    case DW_FORM_exprloc:
      return AttrValue(form, ReadVariableBlock(data));
    case DW_FORM_string:
      return AttrValue(form, ReadNullTerminated(data));
    case DW_FORM_strp:
      if (cu.unit_sizes().dwarf64()) {
        return AttrValue(form, ReadIndirectString<uint64_t>(cu, data));
      } else {
        return AttrValue(form, ReadIndirectString<uint32_t>(cu, data));
      }
    case DW_FORM_data1:
      return AttrValue(form, ReadBytes(1, data));
    case DW_FORM_data2:
      return AttrValue(form, ReadBytes(2, data));
    case DW_FORM_data4:
      return AttrValue(form, ReadBytes(4, data));
    case DW_FORM_data8:
      return AttrValue(form, ReadBytes(8, data));
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
      return AttrValue(form, ReadLEB128<uint64_t>(data));

    // Bloaty doesn't currently care about any bool or signed data.
    // So we fudge it a bit and just stuff these in a uint64.
    case DW_FORM_flag_present:
      return AttrValue(form, 1);
    case DW_FORM_flag:
      return AttrValue(form, ReadFixed<uint8_t>(data));
    case DW_FORM_sdata:
      return AttrValue(form, ReadLEB128<uint64_t>(data));
    default:
      THROWF("Don't know how to parse DWARF form: $0", form);
  }
}

}  // namepsace dwarf
}  // namespace bloaty
