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

TEST_F(BloatyTest, NoSections) {
  RunBloaty({"bloaty", "01-no-sections.bin"});
}

TEST_F(BloatyTest, SectionCountOverflow) {
  RunBloaty({"bloaty", "02-section-count-overflow.o"});
}

TEST_F(BloatyTest, InlinesOnSmallFile) {
  RunBloaty(
      {"bloaty", "-d", "compileunits", "03-small-binary-that-crashed-inlines.bin"});
  RunBloaty(
      {"bloaty", "-d", "inlines", "03-small-binary-that-crashed-inlines.bin"});
  EXPECT_EQ(top_row_->vmsize, 2340);
}

TEST_F(BloatyTest, GoBinary) {
  RunBloaty(
      {"bloaty", "-d", "compileunits", "04-go-binary-with-ref-addr.bin"});
  RunBloaty(
      {"bloaty", "-d", "inlines", "04-go-binary-with-ref-addr.bin"});
}

TEST_F(BloatyTest, MultiThreaded) {
  RunBloaty({"bloaty", "02-section-count-overflow.o"});
  size_t file_size = top_row_->filesize;

  // Bloaty doesn't know or care that you are passing the same file multiple
  // times.
  std::vector<std::string> args{"bloaty"};
  const int count = 100;
  for (int i = 0; i < count; i++) {
    args.push_back("02-section-count-overflow.o");
  }
  RunBloaty(args);  // Heavily multithreaded test.
  EXPECT_EQ(top_row_->filesize, file_size * 100);
}
