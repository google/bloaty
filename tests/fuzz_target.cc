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

#include "bloaty.h"
#include "bloaty.pb.h"
#include "strarr.h"

#include "absl/strings/string_view.h"

using absl::string_view;

namespace bloaty {

class StringPieceInputFile : public InputFile {
 public:
  StringPieceInputFile(string_view data)
      : InputFile("fake_StringPieceInputFile_file") {
    data_ = data;
  }
};

class StringPieceInputFileFactory : public InputFileFactory {
 public:
  StringPieceInputFileFactory(string_view data) : data_(data) {}
 private:
  string_view data_;
  std::unique_ptr<InputFile> OpenFile(
      const std::string& /* filename */) const override {
    return std::unique_ptr<InputFile>(new StringPieceInputFile(data_));
  }
};

void RunBloaty(const InputFileFactory& factory,
               const std::string& data_source) {
  bloaty::RollupOutput output;
  bloaty::Options options;
  std::string error;
  options.add_data_source(data_source);
  options.add_filename("dummy_filename");
  bloaty::BloatyMain(options, factory, &output, &error);
}

}  // namespace bloaty

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char *data2 = reinterpret_cast<const char*>(data);
  bloaty::StringPieceInputFileFactory factory(string_view(data2, size));

  // Try all of the data sources.
  RunBloaty(factory, "segments");
  RunBloaty(factory, "sections");
  RunBloaty(factory, "symbols");
  RunBloaty(factory, "compileunits");
  RunBloaty(factory, "inlines");
  RunBloaty(factory, "armembers");

  return 0;
}
