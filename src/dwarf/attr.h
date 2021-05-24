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

#ifndef BLOATY_DWARF_ATTR_H_
#define BLOATY_DWARF_ATTR_H_

#include <cstdint>

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

namespace bloaty {
namespace dwarf {

class CU;

class AttrValue {
 public:
  static AttrValue ParseAttr(const CU& cu, uint8_t form, absl::string_view* data);

  AttrValue(const AttrValue &) = default;
  AttrValue &operator=(const AttrValue &) = default;

  uint16_t form() const { return form_; }

  bool IsUint() const {
    return type_ == Type::kUint || type_ == Type::kUnresolvedUint;
  }

  bool IsString() const {
    return type_ == Type::kString || type_ == Type::kUnresolvedString;
  }

  // Attempts to coerce to uint, returning nullopt if this is not possible.
  absl::optional<uint64_t> ToUint(const CU& cu) const;

  // REQUIRES: IsUint().
  uint64_t GetUint(const CU& cu) const;

  // REQUIRES: IsString().
  absl::string_view GetString(const CU& cu) const;

 private:
  explicit AttrValue(uint16_t form, uint64_t val)
      : uint_(val), form_(form), type_(Type::kUint) {}
  explicit AttrValue(uint16_t form, absl::string_view val)
      : string_(val), form_(form), type_(Type::kString) {}

  // We delay the resolution of indirect strings and addresses, both to avoid
  // unnecessary work and because they may depend on base values that occur
  // after them in the sequence of attributes, eg.
  //
  // $ dwarfdump -i bloaty
  //   COMPILE_UNIT<header overall offset = 0x00000000>:
  // < 0><0x0000000c>  DW_TAG_compile_unit
  //                     DW_AT_producer              (indexed string: 0x00000000)Debian clang version 11.0.1-2
  //                     DW_AT_language              DW_LANG_C_plus_plus_14
  //                     DW_AT_name                  (indexed string: 0x00000001)../src/main.cc
  //                     DW_AT_str_offsets_base      0x00000008
  //
  // Note that DW_AT_name comes before DW_AT_str_offset_base, but the latter
  // value is required to resolve the name attribute.
  enum class Type {
    kUint,
    kString,
    kUnresolvedUint,
    kUnresolvedString
  };

  Type type() const { return type_; }

  static AttrValue UnresolvedUint(uint16_t form, uint64_t val) {
     AttrValue ret(form, val);
     ret.type_ = Type::kUnresolvedUint;
     return ret;
  }

  static AttrValue UnresolvedString(uint16_t form, uint64_t val) {
     AttrValue ret(form, val);
     ret.type_ = Type::kUnresolvedString;
     return ret;
  }

  union {
    uint64_t uint_;
    absl::string_view string_;
  };

  uint16_t form_;
  Type type_;

  template <class D>
  static absl::string_view ReadBlock(absl::string_view* data);
  static absl::string_view ReadVariableBlock(absl::string_view* data);
  template <class D>
  static absl::string_view ReadIndirectString(const CU& cu,
                                              absl::string_view* data);
  static absl::string_view ResolveIndirectString(const CU& cu, uint64_t ofs);

  absl::string_view ResolveDoubleIndirectString(const CU &cu) const;
  uint64_t ResolveIndirectAddress(const CU& cu) const;
};

}  // namespace dwarf
}  // namespace bloaty

#endif
