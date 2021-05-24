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

#include "dwarf/dwarf_util.h"

using string_view = absl::string_view;

namespace bloaty {
namespace dwarf {

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

absl::string_view ReadDebugStrEntry(absl::string_view debug_str, size_t ofs) {
  SkipBytes(ofs, &debug_str);
  return ReadNullTerminated(&debug_str);
}

}  // namespace dwarf
}  // namespace bloaty
