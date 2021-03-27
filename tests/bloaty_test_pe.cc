// Copyright 2021 Google Inc. All Rights Reserved.
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

struct BloatyTestEntry
{
  std::string name;
  std::vector<std::string> commandline;
  std::string input_file;
  std::string result_file;
};

std::string TestEntryName(const testing::TestParamInfo<struct BloatyTestEntry>& entry) {
  return entry.param.name;
}

std::ostream& operator<<(std::ostream& os, const BloatyTestEntry& entry) {
  os << "{ ";
  for (const auto& str: entry.commandline) {
    os << str << ", ";
  }
  os << entry.input_file << ", " << entry.result_file << " }";
  return os;
}

// Strip all trailing whitespace (including \r)
void Normalize(std::string& contents) {
  std::stringstream buffer(contents);
  contents.clear();
  std::string tmp;
  while (std::getline(buffer, tmp)) {
    auto end = tmp.find_last_not_of("\t \r");
    if (end != std::string::npos) {
      tmp = tmp.substr(0, end + 1);
    }
    else {
      tmp.clear();
    }
    if (!contents.empty()) {
      contents += "\n";
    }
    contents += tmp;
  }
}

inline bool GetFileContents(const std::string& filename, std::string& contents) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Couldn't get file size for: " << filename << "\n";
    return false;
  }
  fseek(file, 0L, SEEK_END);
  size_t size = ftell(file);
  fseek(file, 0L, SEEK_SET);
  contents.resize(size);
  size_t result = fread(&contents[0], 1, size, file);
  fclose(file);
  contents.resize(result);
  Normalize(contents);
  return result == size;
}

class BloatyOutputTest: public BloatyTest,
  public testing::WithParamInterface<BloatyTestEntry>
{
public:
  BloatyOutputTest()
    : commandline(GetParam().commandline)
    , input_file(GetParam().input_file)
    , result_file(GetParam().result_file)
  {
  }

  const std::vector<std::string>& commandline;
  const std::string& input_file;
  const std::string& result_file;
};


TEST_P(BloatyOutputTest, CheckOutput) {
  uint64_t size;
  ASSERT_TRUE(GetFileSize(input_file, &size));
  std::string expect_result;
  ASSERT_TRUE(GetFileContents(result_file, expect_result));

  std::vector<std::string> cmdline = { "bloaty" };
  cmdline.insert(cmdline.end(), commandline.begin(), commandline.end());
  cmdline.push_back(input_file);
  RunBloaty(cmdline);

  bloaty::OutputOptions output_options;
  std::stringstream output_stream;
  output_options.output_format = bloaty::OutputFormat::kTSV;
  output_->Print(output_options, &output_stream);
  std::string tmp = output_stream.str();
  Normalize(tmp);
  EXPECT_EQ(tmp, expect_result);
}

static BloatyTestEntry  tests[] = {
  { "MSVCR15DLL", {}, "msvc-15.0-foo-bar.dll", "msvc-15.0-foo-bar.dll.txt" },
  { "MSVCR15DLLSEG", {"-d", "segments"}, "msvc-15.0-foo-bar.dll", "msvc-15.0-foo-bar.dll.seg.txt" },
  { "MSVC15EXE", {}, "msvc-15.0-foo-bar-main-cv.bin", "msvc-15.0-foo-bar-main-cv.bin.txt" },
  { "MSVC15EXESEG", {"-d", "segments"}, "msvc-15.0-foo-bar-main-cv.bin", "msvc-15.0-foo-bar-main-cv.bin.seg.txt" },

  { "MSVCR16DLL", {}, "msvc-16.0-foo-bar.dll", "msvc-16.0-foo-bar.dll.txt" },
  { "MSVCR16DLLSEG", {"-d", "segments"}, "msvc-16.0-foo-bar.dll", "msvc-16.0-foo-bar.dll.seg.txt" },
  { "MSVC16EXE", {}, "msvc-16.0-foo-bar-main-cv.bin", "msvc-16.0-foo-bar-main-cv.bin.txt" },
  { "MSVC16EXESEG", {"-d", "segments"}, "msvc-16.0-foo-bar-main-cv.bin", "msvc-16.0-foo-bar-main-cv.bin.seg.txt" },
};

INSTANTIATE_TEST_SUITE_P(BloatyTest,
  BloatyOutputTest,
  testing::ValuesIn(tests),
  TestEntryName);
