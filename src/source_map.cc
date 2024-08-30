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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "bloaty.h"
#include "source_map.h"
#include "util.h"

#include "absl/strings/string_view.h"

namespace bloaty {
namespace sourcemap {

static bool ReadOpeningBrace(absl::string_view* data) {
  return ReadFixed<char>(data) == '{';
}

static absl::string_view ReadQuotedString(absl::string_view* data) {
  RequireChar(data, '\"');
  // Simply read until the next '\"'. We currently do not handle escaped
  // characters. Field names never contain quotes and file names are unlikely to
  // contain quotes.
  return ReadUntilConsuming(data, '\"');
}

// Finds the field with the given name in the source map. Any fields encountered
// before the field are skipped.
static void FindField(absl::string_view* data, const char* name) {
  while (!data->empty()) {
    SkipWhitespace(data);
    auto field_name = ReadQuotedString(data);
    if (field_name == name) {
      SkipWhitespace(data);
      RequireChar(data, ':');
      SkipWhitespace(data);
      return;
    }

    // Skip until the next quote. We don't expect any structures involving
    // quotes in the fields that we skip.
    ReadUntil(data, '\"');
  }
  THROWF("field \"$0\" not found in source map", name);
}

static int32_t ReadBase64VLQ(absl::string_view* data) {
  uint32_t value = 0;
  uint32_t shift = 0;
  const char* ptr = data->data();
  const char* limit = ptr + data->size();
  while (ptr < limit) {
    auto ch = *(ptr++);
    // Base64 characters A-Z, a-f do not have the continuation bit set and are
    // the last digit.
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch < 'g')) {
      uint32_t digit = ch < 'a' ? ch - 'A' : ch - 'a' + 26;
      value |= digit << shift;
      data->remove_prefix(ptr - data->data());
      return value & 1
          ? -static_cast<int32_t>(value >> 1)
          : static_cast<int32_t>(value >> 1);
    }
    if (!(ch >= 'g' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '+' &&
        ch != '/') {
      THROWF("Invalid Base64VLQ digit $0", ch);
    }
    // Base64 characters g-z, 0-9, + and / have the continuation bit set and
    // must be followed by another digit.
    uint32_t digit =
      ch > '9' ? ch - 'g' : (ch >= '0' ? ch - '0' + 20 : (ch == '+' ? 30 : 31));
    value |= digit << shift;
    shift += 5;
  }

  THROW("Unterminated Base64VLQ");
}

static bool IsBase64Digit(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
         (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
}

static int ReadBase64VLQSegment(absl::string_view* data, int32_t (&values)[5]) {
  for (int i = 0; i < 5; i++) {
    values[i] = ReadBase64VLQ(data);
    if (data->empty() || !IsBase64Digit(data->front())) {
      if (i != 0 && i != 3 && i != 4) {
        THROWF("Invalid source map VLQ segment length $0", i + 1);
      }
      return i + 1;
    }
  }
  THROW("Unterminated Base64VLQ segment");
}

class VlqSegment {
 public:
  int32_t col;
  int32_t length;

  std::string source_file;
  int32_t source_line;
  int32_t source_col;

  VlqSegment(int32_t col, int32_t length,
             absl::string_view source_file,
             int32_t source_line, int32_t source_col)
      : col(col), length(length),
        source_file(source_file),
        source_line(source_line), source_col(source_col) {}

  void addToSink(RangeSink* sink) const {
    auto name = sink->data_source() == DataSource::kInlines
        ? source_file + ":" + std::to_string(source_line)
        : source_file;
    sink->AddFileRange("sourcemap", name, col, length);
  }
};

template <class Func>
void ForEachVLQSegment(absl::string_view* data,
                       const std::vector<absl::string_view>& sources,
                       Func&& segment_func) {
  if (data->empty() || data->front() == '\"') {
    return;
  }

  // Read the first segment. We don't generate the `VlqSegment` until the next
  // one is encountered. This one only points to a particular byte. The next
  // segment is required to determine the length.
  int32_t values[5];
  int values_count = ReadBase64VLQSegment(data, values);
  if (values_count < 4) {
    THROW("Source file info expected in first VLQ segment");
  }
  int32_t col = values[0];
  int32_t source_file = values[1];
  int32_t source_line = values[2];
  int32_t source_col = values[3];

  while (!data->empty() && data->front() != '\"') {
    if (data->front() == ',') {
      data->remove_prefix(1);
      continue;
    }

    // We don't support line separators in the source map for now.
    if (data->front() == ';') {
      THROW("Unsupported line separator in source map");
    }

    int new_values_count = ReadBase64VLQSegment(data, values);
    if (values_count >= 4) {
      segment_func(VlqSegment(col, values[0],
                              sources[source_file], source_line, source_col));
    }
    values_count = new_values_count;
    col += values[0];
    if (values_count >= 4) {
      source_file += values[1];
      source_line += values[2];
      source_col += values[3];
    }
  }
}

static void ProcessToSink(absl::string_view data, RangeSink* sink) {
  ReadOpeningBrace(&data);

  std::vector<absl::string_view> sources;
  FindField(&data, "sources");
  RequireChar(&data, '[');
  while (!data.empty()) {
    SkipWhitespace(&data);
    if (data.empty()) {
      break;
    }
    if (data.front() == ']') {
      data.remove_prefix(1);
      break;
    }
    if (data.front() == ',') {
      data.remove_prefix(1);
    }
    auto source = ReadQuotedString(&data);
    sources.push_back(source);
  }
  SkipWhitespace(&data);
  RequireChar(&data, ',');

  FindField(&data, "mappings");
  RequireChar(&data, '\"');

  ForEachVLQSegment(&data, sources, [&sink](const VlqSegment& segment) {
    segment.addToSink(sink);
  });

  RequireChar(&data, '\"');
}

void SourceMapObjectFile::ProcessFileToSink(RangeSink* sink) const {
  if (sink->data_source() != DataSource::kCompileUnits
      && sink->data_source() != DataSource::kInlines) {
    THROW("Source map doesn't support this data source");
  }

  ProcessToSink(file_data().data(), sink);
}

}  // namespace sourcemap

std::unique_ptr<ObjectFile> TryOpenSourceMapFile(
    std::unique_ptr<InputFile>& file, std::string build_id) {
  absl::string_view data = file->data();
  if (sourcemap::ReadOpeningBrace(&data)) {
    return std::unique_ptr<ObjectFile>(
        new sourcemap::SourceMapObjectFile(std::move(file), build_id));
  }

  return nullptr;
}

}  // namespace bloaty

