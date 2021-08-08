// Copyright 2020 Google Inc. All Rights Reserved.
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

#ifndef BLOATY_RE_H_
#define BLOATY_RE_H_

#include <string>

#ifdef USE_RE2
#include "re2/re2.h"
#endif

#include "absl/base/attributes.h"

namespace bloaty {

#ifdef USE_RE2
class ReImpl {
 public:
  ReImpl(const char* pattern) : re2_(pattern){};
  ReImpl(const std::string& pattern) : re2_(pattern){};
  bool ok() { return re2_.ok(); }

  static bool Extract(std::string text, const ReImpl& re, std::string rewrite,
                      std::string* out) {
    return RE2::Extract(text, re.re2_, rewrite, out);
  }
  template <typename... A>
  static bool PartialMatch(const std::string& text, const ReImpl& re,
                           A&&... a) {
    return RE2::PartialMatch(text, re.re2_, a...);
  }

  static int GlobalReplace(std::string* str, const ReImpl& re,
                           std::string rewrite) {
    return RE2::GlobalReplace(str, re.re2_, rewrite);
  }
  static bool Replace(std::string* str, const ReImpl& re, std::string rewrite) {
    return RE2::Replace(str, re.re2_, rewrite);
  }

 private:
  RE2 re2_;
};
#else
}

ABSL_ATTRIBUTE_NORETURN
static void _abort() { throw "No support for regular expressions"; }

namespace bloaty {
class ReImpl {
 public:
  ReImpl(const char*) { _abort(); }
  ReImpl(const std::string&) { _abort(); }
  bool ok() { _abort(); }

  ABSL_ATTRIBUTE_NORETURN
  static bool Extract(std::string, const ReImpl&, std::string, std::string*) {
    _abort();
  }
  template <typename... A>
  ABSL_ATTRIBUTE_NORETURN static bool PartialMatch(const std::string&,
                                                   const ReImpl&, A&&...) {
    _abort();
  }
  ABSL_ATTRIBUTE_NORETURN
  static int GlobalReplace(std::string*, const ReImpl&, std::string) {
    _abort();
  }
  ABSL_ATTRIBUTE_NORETURN
  static bool Replace(std::string*, const ReImpl&, std::string) { _abort(); }

 private:
};
#endif

}  // namespace bloaty

#endif  // BLOATY_RE_H_
