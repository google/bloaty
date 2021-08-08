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

#include "test.h"

TEST_F(BloatyTest, EmptyObjectFile) {
  std::string file = "01-empty.o";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  // Empty .c file should result in a .o file with no vmsize.
  RunBloaty({"bloaty", file});
  EXPECT_EQ(top_row_->vmsize, 0);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // Same with segments (we fake segments on .o files).
  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_EQ(top_row_->vmsize, 0);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // Same with symbols.
  RunBloaty({"bloaty", "-d", "symbols", file});
  EXPECT_EQ(top_row_->vmsize, 0);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // We can't run any of these targets against object files.
  std::string errmsg = "can't use data source";
  AssertBloatyFails({"bloaty", "-d", "compileunits", file}, errmsg);
  AssertBloatyFails({"bloaty", "-d", "inlines", file}, errmsg);
}

TEST_F(BloatyTest, SimpleObjectFile) {
  std::string file = "02-simple.o";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  // Test "-n 0" which should return an unlimited number of rows.
  RunBloaty({"bloaty", "-n", "0", file});
  EXPECT_GT(top_row_->vmsize, 64);
  EXPECT_LT(top_row_->vmsize, 300);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // Same with segments (we fake segments on .o files).
  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 64);
  EXPECT_LT(top_row_->vmsize, 300);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // For inputfiles we should get everything attributed to the input file.
  RunBloaty({"bloaty", "-d", "inputfiles", file});
  AssertChildren(*top_row_, {
    std::make_tuple("02-simple.o", kUnknown, kUnknown)
  });

  // For symbols we should get entries for all our expected symbols.
  RunBloaty({"bloaty", "-d", "symbols", "-n", "40", "-s", "vm", file});
  AssertChildren(*top_row_, {
    std::make_tuple("func1", kUnknown, kSameAsVM),
    std::make_tuple("func2", kUnknown, kSameAsVM),
    std::make_tuple("bss_a", 8, 0),
    std::make_tuple("data_a", 8, 8),
    std::make_tuple("rodata_a", 8, 8),
    std::make_tuple("bss_b", 4, 0),
    std::make_tuple("data_b", 4, 4),
    std::make_tuple("rodata_b", 4, 4),
  });

  RunBloaty({"bloaty", "-d", "sections,symbols", "-n", "50", file});

  auto row = FindRow(".bss");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("bss_a", 8, 0),
    std::make_tuple("bss_b", 4, 0),
  });

  row = FindRow(".data");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("data_a", 8, 8),
    std::make_tuple("data_b", 4, 4),
  });

  row = FindRow(".rodata");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("rodata_a", 8, 8),
    std::make_tuple("rodata_b", 4, 4),
  });
}

TEST_F(BloatyTest, SimpleArchiveFile) {
  std::string file = "03-simple.a";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  //EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 3);

  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  //EXPECT_EQ(top_row_->filesize, size);

  RunBloaty({"bloaty", "-d", "symbols", "-n", "40", "-s", "vm", file});
  AssertChildren(*top_row_, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("long_filename_x", 12, 12),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
    std::make_tuple("foo_y", 4, 0),
    std::make_tuple("long_filename_y", 4, 4),
  });

  RunBloaty({"bloaty", "-d", "armembers,symbols", file});
  AssertChildren(*top_row_,
                 {
                     std::make_tuple("bar.o", kUnknown, kUnknown),
                     std::make_tuple("foo.o", kUnknown, kUnknown),
                     std::make_tuple("a_filename_longer_than_sixteen_chars.o",
                                     kUnknown, kUnknown),
                 });

  auto row = FindRow("bar.o");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
  });

  row = FindRow("foo.o");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_y", 4, 0),
  });

  row = FindRow("a_filename_longer_than_sixteen_chars.o");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("long_filename_x", 12, 12),
    std::make_tuple("long_filename_y", 4, 4),
  });
}

TEST_F(BloatyTest, SimpleSharedObjectFile) {
  std::string file = "04-simple.so";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 3);

  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);

  RunBloaty({"bloaty", "-d", "symbols", "-n", "50", file});
  AssertChildren(*top_row_, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("foo_x", 4000, kUnknown),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
  });
}

TEST_F(BloatyTest, SimpleBinary) {
  std::string file = "05-binary.bin";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 3);

  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);

  RunBloaty({"bloaty", "-d", "symbols", "-n", "50", "-s", "vm", file});
  AssertChildren(*top_row_, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("main", kUnknown, kSameAsVM),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
    std::make_tuple("foo_y", 4, 0)
  });

  RunBloaty({"bloaty", "-d", "compileunits,symbols", file});
  auto row = FindRow("bar.o.c");
  ASSERT_TRUE(row != nullptr);

  // This only includes functions (not data) for now.
  AssertChildren(*row, {
    std::make_tuple("bar_x", 4000, kSameAsVM),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("bar_y", kUnknown, kSameAsVM),
    std::make_tuple("bar_z", kUnknown, kSameAsVM),
  });

  row = FindRow("foo.o.c");
  ASSERT_TRUE(row != nullptr);

  // This only includes functions (not data) for now.
  AssertChildren(*row, {
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_y", kUnknown, kSameAsVM),
  });

  RunBloaty({"bloaty", "-d", "sections,inlines", file});
}

TEST_F(BloatyTest, InputFiles) {
  std::string file1 = "05-binary.bin";
  std::string file2 = "07-binary-stripped.bin";
  uint64_t size1, size2;
  ASSERT_TRUE(GetFileSize(file1, &size1));
  ASSERT_TRUE(GetFileSize(file2, &size2));
  RunBloaty({"bloaty", file1, file2, "-d", "inputfiles"});
  AssertChildren(*top_row_,
                 {std::make_tuple(file1, kUnknown, static_cast<int>(size1)),
                  std::make_tuple(file2, kUnknown, static_cast<int>(size2))});

  // Should work with custom data sources.
  bloaty::Options options;
  google::protobuf::TextFormat::ParseFromString(R"(
    filename: "05-binary.bin"
    filename: "07-binary-stripped.bin"
    custom_data_source {
      name: "rewritten_inputfiles"
      base_data_source: "inputfiles"
      rewrite: {
        pattern: "binary"
        replacement: "binary"
      }
    }
    data_source: "rewritten_inputfiles"
  )", &options);

  RunBloatyWithOptions(options, bloaty::OutputOptions());
  AssertChildren(*top_row_, {std::make_tuple("binary", kUnknown,
                                             static_cast<int>(size1 + size2))});
}

TEST_F(BloatyTest, DiffMode) {
  RunBloaty({"bloaty", "06-diff.a", "--", "03-simple.a", "-d", "symbols"});
  AssertChildren(*top_row_, {
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_y", 4, 0)
  });
}

TEST_F(BloatyTest, SeparateDebug) {
  RunBloaty({"bloaty", "--debug-file=05-binary.bin", "07-binary-stripped.bin",
             "-d", "symbols"});
}
