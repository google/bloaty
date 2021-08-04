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

#include "absl/strings/substitute.h"
#include "bloaty.h"
#include "util.h"

using absl::string_view;
using std::string;

namespace bloaty {
namespace pe {
constexpr uint16_t dos_magic = 0x5A4D;  // MZ

// From LIEF/include/LIEF/PE/Structures.hpp.in
//! Sizes in bytes of various things in the COFF format.
constexpr size_t kHeader16Size = 20;
constexpr size_t kHeader32Size = 56;
constexpr size_t kNameSize = 8;
constexpr size_t kSymbol16Size = 18;
constexpr size_t kSymbol32Size = 20;
constexpr size_t kSectionSize = 40;
constexpr size_t kRelocationSize = 10;
constexpr size_t kBaseRelocationBlockSize = 8;
constexpr size_t kImportDirectoryTableEntrySize = 20;
constexpr size_t kResourceDirectoryTableSize = 16;
constexpr size_t kResourceDirectoryEntriesSize = 8;
constexpr size_t kResourceDataEntrySize = 16;

#include "third_party/lief_pe/pe_enums.h"
#include "third_party/lief_pe/pe_structures.h"

static_assert(kSectionSize == sizeof(pe_section),
              "Compiler options broke LIEF struct layout");
static_assert(kRelocationSize == sizeof(pe_relocation),
              "Compiler options broke LIEF struct layout");
static_assert(kBaseRelocationBlockSize == sizeof(pe_base_relocation_block),
              "Compiler options broke LIEF struct layout");
static_assert(kImportDirectoryTableEntrySize == sizeof(pe_import),
              "Compiler options broke LIEF struct layout");
static_assert(kResourceDirectoryTableSize ==
                  sizeof(pe_resource_directory_table),
              "Compiler options broke LIEF struct layout");
static_assert(kResourceDirectoryEntriesSize ==
                  sizeof(pe_resource_directory_entries),
              "Compiler options broke LIEF struct layout");
static_assert(kResourceDataEntrySize == sizeof(pe_resource_data_entry),
              "Compiler options broke LIEF struct layout");
static_assert(kSymbol16Size == sizeof(pe_symbol),
              "Compiler options broke LIEF struct layout");

static_assert(sizeof(PE_TYPE) == sizeof(uint16_t),
              "Compiler options broke PE_TYPE size");

class PeFile {
 public:
  PeFile(string_view data) : data_(data) { ok_ = Initialize(); }

  bool IsOpen() const { return ok_; }

  string_view entire_file() const { return data_; }
  string_view pe_headers() const { return pe_headers_; }
  string_view section_headers() const { return section_headers_; }

  uint32_t section_count() const { return section_count_; }
  string_view section_header(size_t n) const {
    return StrictSubstr(section_headers_, n * sizeof(pe_section),
                        sizeof(pe_section));
  }

 private:
  bool Initialize();

  string_view GetRegion(uint64_t start, uint64_t n) const {
    return StrictSubstr(data_, start, n);
  }

  bool ok_;
  bool is_64bit_;
  const string_view data_;

  pe_dos_header dos_header_;
  pe_header pe_header_;

  string_view pe_headers_;
  string_view section_headers_;
  uint32_t section_count_;
};

bool PeFile::Initialize() {
  if (data_.size() < sizeof(dos_header_)) {
    return false;
  }

  memcpy(&dos_header_, data_.data(), sizeof(dos_header_));

  if (dos_header_.Magic != dos_magic) {
    // Not a PE file.
    return false;
  }

  PE_TYPE Magic;
  auto pe_end =
      CheckedAdd(dos_header_.AddressOfNewExeHeader, sizeof(pe_header_));
  if (CheckedAdd(pe_end, sizeof(Magic)) > data_.size()) {
    // Cannot fit the headers / magic from optional header
    return false;
  }

  memcpy(&pe_header_, data_.data() + dos_header_.AddressOfNewExeHeader,
         sizeof(pe_header_));

  if (!std::equal(pe_header_.signature, pe_header_.signature + sizeof(PE_Magic),
                  std::begin(PE_Magic))) {
    // Not a PE file.
    return false;
  }

  memcpy(&Magic, data_.data() + pe_end, sizeof(Magic));

  if (Magic != PE_TYPE::PE32 && Magic != PE_TYPE::PE32_PLUS) {
    // Unknown PE magic
    return false;
  }

  is_64bit_ = Magic == PE_TYPE::PE32_PLUS;

  section_count_ = pe_header_.NumberOfSections;

  // TODO(mj): Figure out if we should trust SizeOfOptionalHeader here
  const uint32_t sections_offset = dos_header_.AddressOfNewExeHeader +
                                   sizeof(pe_header_) +
                                   pe_header_.SizeOfOptionalHeader;

  auto sections_size = CheckedMul(section_count_, sizeof(pe_section));
  if ((sections_offset + sections_size) > data_.size()) {
    // Cannot fit the headers
    return false;
  }

  pe_headers_ = GetRegion(0, sections_offset);
  section_headers_ = GetRegion(sections_offset, sections_size);

  return true;
}

class Section {
 public:
  string name;

  uint32_t raw_offset() const { return header_.PointerToRawData; }
  uint32_t raw_size() const { return header_.SizeOfRawData; }

  uint32_t virtual_addr() const { return header_.VirtualAddress; }
  uint32_t virtual_size() const { return header_.VirtualSize; }

  Section(string_view header_data) {
    assert(header_data.size() == sizeof(header_));
    memcpy(&header_, header_data.data(), sizeof(header_));

    // TODO(mj): Handle long section names:
    // For longer names, this member contains a forward slash (/) followed by an
    // ASCII representation of a decimal number that is an offset into the
    // string table.
    name = string(header_.Name, strnlen(header_.Name, kNameSize));
  }

 private:
  pe_section header_;
};

template <class Func>
void ForEachSection(const PeFile& pe, Func&& section_func) {
  for (auto n = 0; n < pe.section_count(); ++n) {
    Section section(pe.section_header(n));
    section_func(section);
  }
}

void ParseSections(const PeFile& pe, RangeSink* sink) {
  assert(pe.IsOpen());
  ForEachSection(pe, [sink, &pe](const Section& section) {
    uint64_t vmaddr = section.virtual_addr();
    uint64_t vmsize = section.virtual_size();
    absl::string_view section_data = StrictSubstr(
        pe.entire_file(), section.raw_offset(), section.raw_size());

    sink->AddRange("pe_sections", section.name, vmaddr, vmsize, section_data);
  });
}

void AddCatchAll(const PeFile& pe, RangeSink* sink) {
  assert(pe.IsOpen());

  auto begin = pe.pe_headers().data() - sink->input_file().data().data();
  sink->AddRange("pe_catchall", "[PE Headers]", begin, pe.pe_headers().size(),
                 pe.pe_headers());

  begin = pe.section_headers().data() - sink->input_file().data().data();
  sink->AddRange("pe_catchall", "[PE Section Headers]", begin,
                 pe.section_headers().size(), pe.section_headers());

  // The last-line fallback to make sure we cover the entire file.
  sink->AddFileRange("pe_catchall", "[Unmapped]", sink->input_file().data());
}

class PEObjectFile : public ObjectFile {
 public:
  PEObjectFile(std::unique_ptr<InputFile> file_data,
               std::unique_ptr<pe::PeFile> pe)
      : ObjectFile(std::move(file_data)), pe_file(std::move(pe)) {}

  std::string GetBuildId() const override {
    // TODO(mj): Read from pe_pdb_??
    return std::string();
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) const override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
          // TODO(mj): sections: list out imports and other stuff!
        case DataSource::kSections:
          ParseSections(*pe_file, sink);
          break;
        case DataSource::kSymbols:
        case DataSource::kRawSymbols:
        case DataSource::kShortSymbols:
        case DataSource::kFullSymbols:
          // TODO(mj): Generate symbols from debug info, exports, imports, tls
          // data, relocations, resources ...
        case DataSource::kArchiveMembers:
        case DataSource::kCompileUnits:
        case DataSource::kInlines:
        default:
          THROW("PE doesn't support this data source");
      }
      AddCatchAll(*pe_file, sink);
    }
  }

  bool GetDisassemblyInfo(string_view /*symbol*/, DataSource /*symbol_source*/,
                          DisassemblyInfo* /*info*/) const override {
    WARN("PE files do not support disassembly yet");
    return false;
  }

 protected:
  std::unique_ptr<pe::PeFile> pe_file;
};

bool ReadMagic(const string_view& data) {
  // If the size is smaller than a dos header, it cannot be a PE file, right?
  if (data.size() < sizeof(pe_dos_header)) {
    return false;
  }

  uint16_t magic;
  memcpy(&magic, data.data(), sizeof(magic));

  return magic == dos_magic;
}
}  // namespace pe

std::unique_ptr<ObjectFile> TryOpenPEFile(std::unique_ptr<InputFile>& file) {
  // Do not bother creating an object if the first magic is not even there
  if (pe::ReadMagic(file->data())) {
    std::unique_ptr<pe::PeFile> pe(new pe::PeFile(file->data()));

    if (pe->IsOpen()) {
      return std::unique_ptr<ObjectFile>(
          new pe::PEObjectFile(std::move(file), std::move(pe)));
    }
  }

  return nullptr;
}

}  // namespace bloaty
