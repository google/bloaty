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

namespace bloaty {

class PackFileHandler : public FileHandler {
  bool ProcessBaseMap(RangeSink* sink) override {
    return false;
  }

  bool ProcessFile(const std::vector<RangeSink*>& sinks,
                   std::string* filename) override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
        case DataSource::kSymbols:
        case DataSource::kArchiveMembers:
        case DataSource::kCompileUnits:
        case DataSource::kInlines:
        default:
          return false;
      }
    }

    return true;
  }
};

std::unique_ptr<FileHandler> TryOpenPackFile(const InputFile& file) {
  return nullptr;
}

}  // namespace
