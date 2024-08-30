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

absl::string_view ReadUntilConsuming(absl::string_view* data, char c) {
  absl::string_view ret = ReadUntil(data, c);

  if (data->empty() || data->front() != c) {
    // Nothing left, meaning we didn't find the terminating character.
    if (c == '\0') {
      THROW("string is not NULL-terminated");
    }
    THROWF("could not find terminating character '$0'", c);
  }

  data->remove_prefix(1);  // Remove the terminating character also.
  return ret;
}

absl::string_view ReadUntil(absl::string_view* data, char c) {
  const char* found =
      static_cast<const char*>(memchr(data->data(), c, data->size()));

  size_t len = (found == NULL) ? data->size() : (found - data->data());
  absl::string_view val = data->substr(0, len);
  data->remove_prefix(len);
  return val;
}

void SkipWhitespace(absl::string_view* data) {
  const char* c = data->data();
  const char* limit = c + data->size();
  while (c < limit) {
    if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
      c++;
    } else {
      break;
    }
  }
  data->remove_prefix(c - data->data());
}

}  // namespace bloaty
