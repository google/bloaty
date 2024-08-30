// Copyright 2024 Google Inc. All Rights Reserved.
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

#ifndef BLOATY_SOURCE_MAP_H_
#define BLOATY_SOURCE_MAP_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "bloaty.h"
#include "util.h"

#include "absl/strings/string_view.h"

namespace bloaty {
namespace sourcemap {

class SourceMapObjectFile : public ObjectFile {
 public:
  SourceMapObjectFile(std::unique_ptr<InputFile> file_data,
                      std::string build_id)
    : ObjectFile(std::move(file_data)), build_id_(build_id) {}

  std::string GetBuildId() const override {
    return build_id_;
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) const override {
    WARN("General processing not supported for source map files");
  }

  bool GetDisassemblyInfo(absl::string_view /*symbol*/,
                          DataSource /*symbol_source*/,
                          DisassemblyInfo* /*info*/) const override {
    WARN("Disassembly not supported for source map files");
    return false;
  }

  void ProcessFileToSink(RangeSink* sink) const;

 private:
  std::string build_id_;
};

}  // namespace sourcemap
}  // namespace bloaty

#endif  // BLOATY_SOURCE_MAP_H_

