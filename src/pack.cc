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

// A pack file stores the parsed results from reading one or more input files.
// This lets users store the results of parsing in a way that is easy to do
// further processing on later.
//
// We use an SSTable file to store the data.  This provides random access so
// that we only need to scan the parts of the file corresponding to the "-d"
// options the user passes.

#include "bloaty.h"

namespace bloaty {

// We pack several values into the key and value.
struct Key {
  DataSource data_source;
  std::string filename;
  uint64_t start_address;

  void Pack(std::string* serialized) const {}
  void Unpack(StringPiece serialized) {}
};

struct Value {
  int64_t range_size;
  std::string label;

  // Only for DataSource::kSegments and DataSource::kSections.
  uint64_t file_start_address;
  int64_t file_size;

  void Pack(std::string* serialized) const {}
  void Unpack(StringPiece serialized) {}
};


class PackFileHandler : public FileHandler {
  bool ProcessFile(const std::vector<RangeSink*>& sinks) override {
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

bool WritePackFile(std::vector<MemoryMap*> maps) {
  return false;
}

}  // namespace
