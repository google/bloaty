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

#include "util.h"

using absl::string_view;

namespace bloaty {

ABSL_ATTRIBUTE_NORETURN
void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

absl::string_view ReadNullTerminated(absl::string_view* data) {
  const char* nullz =
      static_cast<const char*>(memchr(data->data(), '\0', data->size()));

  // Return false if not NULL-terminated.
  if (nullz == NULL) {
    THROW("DWARF string was not NULL-terminated");
  }

  size_t len = nullz - data->data();
  absl::string_view val = data->substr(0, len);
  data->remove_prefix(len + 1);  // Remove NULL also.
  return val;
}

}  // namespace bloaty
