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

#ifndef BLOATY_DWARF_UTIL_H_
#define BLOATY_DWARF_UTIL_H_

#include <cstdint>
#include <type_traits>

#include "absl/strings/string_view.h"
#include "util.h"

namespace bloaty {
namespace dwarf {

uint64_t ReadLEB128Internal(bool is_signed, absl::string_view* data);

// Reads a DWARF LEB128 varint, where high bits indicate continuation.
template <typename T>
T ReadLEB128(absl::string_view* data) {
  typedef typename std::conditional<std::is_signed<T>::value, int64_t,
                                    uint64_t>::type Int64Type;
  Int64Type val = ReadLEB128Internal(std::is_signed<T>::value, data);
  if (val > std::numeric_limits<T>::max() ||
      val < std::numeric_limits<T>::min()) {
    THROW("DWARF data contained larger LEB128 than we were expecting");
  }
  return static_cast<T>(val);
}

void SkipLEB128(absl::string_view* data);

bool IsValidDwarfAddress(uint64_t addr, uint8_t address_size);

inline int DivRoundUp(int n, int d) {
  return (n + (d - 1)) / d;
}

absl::string_view ReadDebugStrEntry(absl::string_view debug_str, size_t ofs);

}  // namepsace dwarf
}  // namepsace bloaty

#endif  // BLOATY_DWARF_UTIL_H_
