// Copyright 2018 Google Inc. All Rights Reserved.
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
#include "util.h"

#include "absl/strings/substitute.h"

using absl::string_view;

namespace bloaty {
namespace wasm {

uint64_t ReadLEB128Internal(bool is_signed, size_t size, string_view* data) {
  uint64_t ret = 0;
  int shift = 0;
  int maxshift = 70;
  const char* ptr = data->data();
  const char* limit = ptr + data->size();

  while (ptr < limit && shift < maxshift) {
    char byte = *(ptr++);
    ret |= static_cast<uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) {
      data->remove_prefix(ptr - data->data());
      if (is_signed && shift < size && (byte & 0x40)) {
        ret |= -(1ULL << shift);
      }
      return ret;
    }
  }

  THROW("corrupt wasm data, unterminated LEB128");
}

bool ReadVarUInt1(string_view* data) {
  return static_cast<bool>(ReadLEB128Internal(false, 1, data));
}

uint8_t ReadVarUInt7(string_view* data) {
  return static_cast<char>(ReadLEB128Internal(false, 7, data));
}

uint32_t ReadVarUInt32(string_view* data) {
  return static_cast<uint32_t>(ReadLEB128Internal(false, 32, data));
}

int8_t ReadVarint7(string_view* data) {
  return static_cast<int8_t>(ReadLEB128Internal(true, 7, data));
}

string_view ReadPiece(size_t bytes, string_view* data) {
  if(data->size() < bytes) {
    THROW("premature EOF reading variable-length DWARF data");
  }
  string_view ret = data->substr(0, bytes);
  data->remove_prefix(bytes);
  return ret;
}

bool ReadMagic(string_view* data) {
  const uint32_t wasm_magic = 0x6d736100;
  auto magic = ReadFixed<uint32_t>(data);

  if (magic != wasm_magic) {
    return false;
  }

  // TODO(haberman): do we need to fail if this is >1?
  auto version = ReadFixed<uint32_t>(data);
  (void)version;

  return true;
}

class Section {
 public:
  uint32_t id;
  std::string name;
  string_view data;
  string_view contents;

  static Section Read(string_view* data_param) {
    Section ret;
    string_view data = *data_param;
    string_view section_data = data;

    ret.id = ReadVarUInt7(&data);
    uint32_t size = ReadVarUInt32(&data);
    ret.contents = ReadPiece(size, &data);
    size_t header_size = ret.contents.data() - section_data.data();
    ret.data = ReadPiece(size + header_size, &section_data);

    if (ret.id == 0) {
      uint32_t name_len = ReadVarUInt32(&ret.contents);
      ret.name = std::string(ReadPiece(name_len, &ret.contents));
    } else if (ret.id <= 13) {
      ret.name = names[ret.id];
    } else {
      THROWF("Unknown section id: $0", ret.id);
    }

    *data_param = data;
    return ret;
  }

  enum Name {
    kType      = 1,
    kImport    = 2,
    kFunction  = 3,
    kTable     = 4,
    kMemory    = 5,
    kGlobal    = 6,
    kExport    = 7,
    kStart     = 8,
    kElement   = 9,
    kCode      = 10,
    kData      = 11,
    kDataCount = 12,
    kEvent     = 13,
  };

  static const char* names[];
};

const char* Section::names[] = {
  "<none>",    // 0
  "Type",      // 1
  "Import",    // 2
  "Function",  // 3
  "Table",     // 4
  "Memory",    // 5
  "Global",    // 6
  "Export",    // 7
  "Start",     // 8
  "Element",   // 9
  "Code",      // 10
  "Data",      // 11
  "DataCount", // 12
  "Event",     // 13
};

struct ExternalKind {
  enum Kind {
    kFunction = 0,
    kTable = 1,
    kMemory = 2,
    kGlobal = 3,
  };
};

template <class Func>
void ForEachSection(string_view file, Func&& section_func) {
  string_view data = file;
  ReadMagic(&data);

  while (!data.empty()) {
    Section section = Section::Read(&data);
    section_func(section);
  }
}

void ParseSections(RangeSink* sink) {
  ForEachSection(sink->input_file().data(), [sink](const Section& section) {
    sink->AddFileRange("wasm_sections", section.name, section.data);
  });
}

typedef std::unordered_map<int, std::string> FuncNames;

void ReadFunctionNames(const Section& section, FuncNames* names,
                       RangeSink* sink) {
  enum class NameType {
    kModule = 0,
    kFunction = 1,
    kLocal = 2,
  };

  string_view data = section.contents;

  while (!data.empty()) {
    char type = ReadVarUInt7(&data);
    uint32_t size = ReadVarUInt32(&data);
    string_view section = ReadPiece(size, &data);

    if (static_cast<NameType>(type) == NameType::kFunction) {
      uint32_t count = ReadVarUInt32(&section);
      for (uint32_t i = 0; i < count; i++) {
        string_view entry = section;
        uint32_t index = ReadVarUInt32(&section);
        uint32_t name_len = ReadVarUInt32(&section);
        string_view name = ReadPiece(name_len, &section);
        entry = StrictSubstr(entry, 0, name.data() - entry.data() + name.size());
        sink->AddFileRange("wasm_funcname", name, entry);
        (*names)[index] = std::string(name);
      }
    }
  }
}

int ReadValueType(string_view* data) {
  return ReadVarint7(data);
}

int ReadElemType(string_view* data) {
  return ReadVarint7(data);
}

void ReadResizableLimits(string_view* data) {
  auto flags = ReadVarUInt1(data);
  ReadVarUInt32(data);
  if (flags) {
    ReadVarUInt32(data);
  }
}

void ReadGlobalType(string_view* data) {
  ReadValueType(data);
  ReadVarUInt1(data);
}

void ReadTableType(string_view* data) {
  ReadElemType(data);
  ReadResizableLimits(data);
}

void ReadMemoryType(string_view* data) {
  ReadResizableLimits(data);
}

uint32_t GetNumFunctionImports(const Section& section) {
  assert(section.id == Section::kImport);
  string_view data = section.contents;

  uint32_t count = ReadVarUInt32(&data);
  uint32_t func_count = 0;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t module_len = ReadVarUInt32(&data);
    ReadPiece(module_len, &data);
    uint32_t field_len = ReadVarUInt32(&data);
    ReadPiece(field_len, &data);
    auto kind = ReadFixed<uint8_t>(&data);

    switch (kind) {
      case ExternalKind::kFunction:
        func_count++;
        ReadVarUInt32(&data);
        break;
      case ExternalKind::kTable:
        ReadTableType(&data);
        break;
      case ExternalKind::kMemory:
        ReadMemoryType(&data);
        break;
      case ExternalKind::kGlobal:
        ReadGlobalType(&data);
        break;
      default:
        THROWF("Unrecognized import kind: $0", kind);
    }
  }

  return func_count;
}

void ReadCodeSection(const Section& section, const FuncNames& names,
                     uint32_t num_imports, RangeSink* sink) {
  string_view data = section.contents;

  uint32_t count = ReadVarUInt32(&data);

  for (uint32_t i = 0; i < count; i++) {
    string_view func = data;
    uint32_t size = ReadVarUInt32(&data);
    uint32_t total_size = size + (data.data() - func.data());

    func = StrictSubstr(func, 0, total_size);
    data = StrictSubstr(data, size);

    auto iter = names.find(num_imports + i);

    if (iter == names.end()) {
      std::string name = "func[" + std::to_string(i) + "]";
      sink->AddFileRange("wasm_function", name, func);
    } else {
      sink->AddFileRange("wasm_function", ItaniumDemangle(iter->second, sink->data_source()), func);
    }
  }
}

void ParseSymbols(RangeSink* sink) {
  // First pass: read the custom naming section to get function names.
  std::unordered_map<int, std::string> func_names;
  uint32_t num_imports = 0;

  ForEachSection(sink->input_file().data(),
                 [&func_names, sink](const Section& section) {
                   if (section.name == "name") {
                     ReadFunctionNames(section, &func_names, sink);
                   }
                 });

  // Second pass: read the function/code sections.
  ForEachSection(sink->input_file().data(),
                 [&func_names, &num_imports, sink](const Section& section) {
                   if (section.id == Section::kImport) {
                     num_imports = GetNumFunctionImports(section);
                   } else if (section.id == Section::kCode) {
                     ReadCodeSection(section, func_names, num_imports, sink);
                   }
                 });
}

void AddWebAssemblyFallback(RangeSink* sink) {
  ForEachSection(sink->input_file().data(), [sink](const Section& section) {
    std::string name2 =
        std::string("[section ") + std::string(section.name) + std::string("]");
    sink->AddFileRange("wasm_overhead", name2, section.data);
  });
  sink->AddFileRange("wasm_overhead", "[WASM Header]",
                     StrictSubstr(sink->input_file().data(), 0, 8));
}

class WebAssemblyObjectFile : public ObjectFile {
 public:
  WebAssemblyObjectFile(std::unique_ptr<InputFile> file_data)
      : ObjectFile(std::move(file_data)) {}

  std::string GetBuildId() const override {
    // TODO(haberman): does WebAssembly support this?
    return std::string();
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) const override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
          ParseSections(sink);
          break;
        case DataSource::kSymbols:
        case DataSource::kRawSymbols:
        case DataSource::kShortSymbols:
        case DataSource::kFullSymbols:
          ParseSymbols(sink);
          break;
        case DataSource::kArchiveMembers:
        case DataSource::kCompileUnits:
        case DataSource::kInlines:
        default:
          THROW("WebAssembly doesn't support this data source");
      }
      AddWebAssemblyFallback(sink);
    }
  }

  bool GetDisassemblyInfo(absl::string_view /*symbol*/,
                          DataSource /*symbol_source*/,
                          DisassemblyInfo* /*info*/) const override {
    WARN("WebAssembly files do not support disassembly yet");
    return false;
  }
};

}  // namespace wasm

std::unique_ptr<ObjectFile> TryOpenWebAssemblyFile(
    std::unique_ptr<InputFile>& file) {
  string_view data = file->data();
  if (wasm::ReadMagic(&data)) {
    return std::unique_ptr<ObjectFile>(
        new wasm::WebAssemblyObjectFile(std::move(file)));
  }

  return nullptr;
}

}  // namespace bloaty
