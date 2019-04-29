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

#ifndef BLOATY_TESTS_STRARR_H_
#define BLOATY_TESTS_STRARR_H_

#include <memory>
#include <string>
#include <vector>

// For constructing arrays of strings in the slightly peculiar format
// required by execve().
class StrArr {
 public:
  explicit StrArr(const std::vector<std::string>& strings)
      : size_(strings.size()), array_(new char*[size_ + 1]) {
    array_[size_] = NULL;
    for (size_t i = 0; i < strings.size(); i++) {
      // Can't use c_str() directly because array_ is not const char*.
      array_[i] = strdup(strings[i].c_str());
    }
  }

  ~StrArr() {
    // unique_ptr frees the array of pointers but not the pointed-to strings.
    for (int i = 0; i < size_; i++) {
      free(array_[i]);
    }
  }

  char **get() const { return array_.get(); }

  size_t size() const { return size_; }

 private:
  size_t size_;
  // Can't use vector<char*> because execve() takes ptr to non-const array.
  std::unique_ptr<char*[]> array_;
};

#endif // BLOATY_TESTS_STRARR_H_
