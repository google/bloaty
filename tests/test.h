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

#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <tuple>
#include <vector>
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"

#include "strarr.h"
#include "bloaty.h"
#include "bloaty.pb.h"

#if defined(_MSC_VER)
#define PATH_MAX  4096
#endif

inline bool GetFileSize(const std::string& filename, uint64_t* size) {
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

inline std::string GetTestDirectory() {
  char pathbuf[PATH_MAX];
  if (!getcwd(pathbuf, sizeof(pathbuf))) {
    return "";
  }
  std::string path(pathbuf);
  size_t pos = path.rfind('/');
  return path.substr(pos + 1);
}

inline std::string DebugString(const google::protobuf::Message& message) {
  std::string ret;
  google::protobuf::TextFormat::PrintToString(message, &ret);
  return ret;
}

#define NONE_STRING "[None]"

// Testing Bloaty requires a delicate balance.  Bloaty's output is by its
// nature very compiler and platform dependent.  So we want to verify correct
// operation without overspecifying how the platform should behave.

class BloatyTest : public ::testing::Test {
 protected:
  void CheckConsistencyForRow(const bloaty::RollupRow& row, bool is_toplevel,
                             bool diff_mode, int* count) {
    // If any children exist, they should sum up to this row's values.
    // Also none of the children should have the same name.
    std::unordered_set<std::string> names;

    if (row.sorted_children.size() > 0) {
      uint64_t vmtotal = 0;
      uint64_t filetotal = 0;
      for (const auto& child : row.sorted_children) {
        vmtotal += child.vmsize;
        filetotal += child.filesize;
        CheckConsistencyForRow(child, false, diff_mode, count);
        ASSERT_TRUE(names.insert(child.name).second);
        ASSERT_FALSE(child.vmsize == 0 && child.filesize == 0);
      }

      if (!diff_mode) {
        ASSERT_EQ(vmtotal, row.vmsize);
        ASSERT_EQ(filetotal, row.filesize);
      }
    } else {
      // Count leaf rows.
      *count += 1;
    }

    if (!is_toplevel && row.sorted_children.size() == 1) {
      ASSERT_NE(NONE_STRING, row.sorted_children[0].name);
    }
  }

  void CheckCSVConsistency(int row_count) {
    std::ostringstream stream;
    bloaty::OutputOptions options;
    options.output_format = bloaty::OutputFormat::kCSV;
    output_->Print(options, &stream);
    std::string csv_output = stream.str();

    std::vector<std::string> rows = absl::StrSplit(csv_output, '\n');
    // Output ends with a final '\n', trim this.
    ASSERT_EQ("", rows[rows.size() - 1]);
    rows.pop_back();

    ASSERT_GT(rows.size(), 0);  // There should be a header row.

    ASSERT_EQ(rows.size() - 1, row_count);
    bool first = true;
    for (const auto& row : rows) {
      std::vector<std::string> cols = absl::StrSplit(row, ',');
      if (first) {
        // header row should be: header1,header2,...,vmsize,filesize
        std::vector<std::string> expected_headers(output_->source_names());
        expected_headers.push_back("vmsize");
        expected_headers.push_back("filesize");
        ASSERT_EQ(cols, expected_headers);
        first = false;
      } else {
        // Final two columns should parse as integer.
        int out;
        ASSERT_EQ(output_->source_names().size() + 2, cols.size());
        ASSERT_TRUE(absl::SimpleAtoi(cols[cols.size() - 1], &out));
        ASSERT_TRUE(absl::SimpleAtoi(cols[cols.size() - 2], &out));
      }
    }
  }

  void CheckConsistency(const bloaty::Options& options) {
    ASSERT_EQ(options.base_filename_size() > 0, output_->diff_mode());

    if (!output_->diff_mode()) {
      size_t total_input_size = 0;
      for (const auto& filename : options.filename()) {
        uint64_t size;
        ASSERT_TRUE(GetFileSize(filename, &size));
        total_input_size += size;
      }
      ASSERT_EQ(top_row_->filesize, total_input_size);
    }

    int rows = 0;
    CheckConsistencyForRow(*top_row_, true, output_->diff_mode(), &rows);
    CheckCSVConsistency(rows);
    ASSERT_EQ("TOTAL", top_row_->name);
  }

  std::string JoinStrings(const std::vector<std::string>& strings) {
    std::string ret = strings[0];
    for (size_t i = 1; i < strings.size(); i++) {
      ret += " " + strings[i];
    }
    return ret;
  }

  bool TryRunBloatyWithOptions(const bloaty::Options& options,
                               const bloaty::OutputOptions& output_options) {
    output_.reset(new bloaty::RollupOutput);
    top_row_ = &output_->toplevel_row();
    std::string error;
    bloaty::MmapInputFileFactory factory;
    if (bloaty::BloatyMain(options, factory, output_.get(), &error)) {
      CheckConsistency(options);
      output_->Print(output_options, &std::cerr);
      return true;
    } else {
      std::cerr << "Bloaty returned error:" << error << "\n";
      return false;
    }
  }

  bool TryRunBloaty(const std::vector<std::string>& strings) {
    bloaty::Options options;
    bloaty::OutputOptions output_options;
    std::string error;
    StrArr str_arr(strings);
    int argc = strings.size();
    char** argv = str_arr.get();
    bool ok = bloaty::ParseOptions(false, &argc, &argv, &options,
                                   &output_options, &error);
    if (!ok) {
      std::cerr << "Error parsing options: " << error;
      return false;
    }

    return TryRunBloatyWithOptions(options, output_options);
  }

  void RunBloaty(const std::vector<std::string>& strings) {
    std::cerr << "Running bloaty: " << JoinStrings(strings) << "\n";
    ASSERT_TRUE(TryRunBloaty(strings));
  }

  void RunBloatyWithOptions(const bloaty::Options& options,
                            const bloaty::OutputOptions& output_options) {
    std::cerr << "Running bloaty, options: " << DebugString(options) << "\n";
    ASSERT_TRUE(TryRunBloatyWithOptions(options, output_options));
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
        EXPECT_GE(child.vmsize, expected_vm);
        // Allow some overhead.
        EXPECT_LE(child.vmsize, (expected_vm * 1.1) + 100);
      } else {
        ASSERT_TRUE(false);
      }

      if (expected_file == kSameAsVM) {
        expected_file = child.vmsize;
      }

      if (expected_file != kUnknown) {
        EXPECT_GE(child.filesize, expected_file);
        // Allow some overhead.
        EXPECT_LE(child.filesize, (expected_file * 1.2) + 180);
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
