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

#ifndef BLOATY_TESTS_TEST_H_
#define BLOATY_TESTS_TEST_H_

#include "bloaty.h"

#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <tuple>
#include <vector>
#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "strarr.h"

bool GetFileSize(const std::string& filename, uint64_t* size) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Couldn't get file size for: " << filename << "\n";
    return false;
  }
  fseek(file, 0L, SEEK_END);
  *size = ftell(file);
  fclose(file);
  return true;
}

std::string GetTestDirectory() {
  char pathbuf[PATH_MAX];
  if (!getcwd(pathbuf, sizeof(pathbuf))) {
    return "";
  }
  std::string path(pathbuf);
  size_t pos = path.rfind('/');
  return path.substr(pos + 1);
}

#define NONE_STRING "[None]"

// Testing Bloaty requires a delicate balance.  Bloaty's output is by its
// nature very compiler and platform dependent.  So we want to verify correct
// operation without overspecifying how the platform should behave.

class BloatyTest : public ::testing::Test {
 protected:
  void CheckConsistencyForRow(const bloaty::RollupRow& row, bool is_toplevel) {
    // If any children exist, they should sum up to this row's values.
    // Also none of the children should have the same name.
    std::unordered_set<std::string> names;

    if (row.sorted_children.size() > 0) {
      uint64_t vmtotal = 0;
      uint64_t filetotal = 0;
      for (const auto& child : row.sorted_children) {
        vmtotal += child.vmsize;
        filetotal += child.filesize;
        CheckConsistencyForRow(child, false);
        ASSERT_TRUE(names.insert(child.name).second);
        ASSERT_FALSE(child.vmsize == 0 && child.filesize == 0);
      }

      if (!row.diff_mode) {
        ASSERT_EQ(vmtotal, row.vmsize);
        ASSERT_EQ(filetotal, row.filesize);
      }
    }

    if (!is_toplevel && row.sorted_children.size() == 1) {
      ASSERT_NE(NONE_STRING, row.sorted_children[0].name);
    }
  }

  void CheckConsistency() {
    CheckConsistencyForRow(*top_row_, true);
    ASSERT_EQ("TOTAL", top_row_->name);
  }

  std::string JoinStrings(const std::vector<std::string>& strings) {
    std::string ret = strings[0];
    for (size_t i = 1; i < strings.size(); i++) {
      ret += " " + strings[i];
    }
    return ret;
  }

  bool TryRunBloaty(const std::vector<std::string>& strings) {
    std::cerr << "Running bloaty: " << JoinStrings(strings) << "\n";
    output_.reset(new bloaty::RollupOutput);
    top_row_ = &output_->toplevel_row();
    bloaty::MmapInputFileFactory factory;
    if (bloaty::BloatyMain(strings.size(), StrArr(strings).get(), factory,
                           output_.get())) {
      CheckConsistency();
      output_->Print(&std::cerr);
      return true;
    } else {
      std::cerr << "Bloaty returned error." << "\n";
      return false;
    }
  }

  void RunBloaty(const std::vector<std::string>& strings) {
    ASSERT_TRUE(TryRunBloaty(strings));
  }

  void AssertBloatyFails(const std::vector<std::string>& strings,
                         const std::string& /*msg_regex*/) {
    // TODO(haberman): verify msg_regex by making all errors logged to a
    // standard place.
    ASSERT_FALSE(TryRunBloaty(strings));
  }

  // Special constants for asserting of children.
  static constexpr int kUnknown = -1;
  static constexpr int kSameAsVM = -2;  // Only for file size.

  void AssertChildren(
      const bloaty::RollupRow& row,
      const std::vector<std::tuple<std::string, int, int>>& children) {
    size_t i = 0;
    for (const auto& child : row.sorted_children) {
      std::string expected_name;
      int expected_vm, expected_file;
      std::tie(expected_name, expected_vm, expected_file) = children[i];

      // Excluding leading '_' is kind of a hack to exclude symbols
      // automatically inserted by the compiler, like __x86.get_pc_thunk.bx
      // for 32-bit x86 builds or _IO_stdin_used in binaries.
      //
      // Excluding leading '[' is for things like this:
      //
      //   [None]
      //   [ELF Headers]
      //   [AR Headers]
      //   etc.
      if (child.name[0] == '[' || child.name[0] == '_') {
        continue;
      }
      EXPECT_EQ(expected_name, child.name);

      // <0 indicates that we don't know what the exact size should be (for
      // example for functions).
      if (expected_vm == kUnknown) {
        // Always pass.
      } else if (expected_vm > 0) {
        EXPECT_EQ(expected_vm, child.vmsize);
      } else {
        ASSERT_TRUE(false);
      }

      if (expected_file == kUnknown) {
        // Always pass.
      } else if (expected_file == kSameAsVM) {
        EXPECT_EQ(child.vmsize, child.filesize);
      } else {
        EXPECT_EQ(expected_file, child.filesize);
      }

      if (++i == children.size()) {
        // We allow the actual data to have excess elements.
        break;
      }
    }

    // All expected elements must be present.
    ASSERT_EQ(i, children.size());
  }

  const bloaty::RollupRow* FindRow(const std::string& name) {
    for (const auto& child : top_row_->sorted_children) {
      if (child.name == name) {
        return &child;
      }
    }
    EXPECT_TRUE(false) << name;
    return nullptr;
  }

  std::unique_ptr<bloaty::RollupOutput> output_;
  const bloaty::RollupRow* top_row_;
};

constexpr int BloatyTest::kUnknown;
constexpr int BloatyTest::kSameAsVM;

#endif  // BLOATY_TESTS_TEST_H_
