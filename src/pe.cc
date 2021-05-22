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

#include "bloaty.h"
#include "absl/strings/substitute.h"
#include "util.h"

using absl::string_view;

namespace bloaty {
namespace pe {
const uint16_t dos_magic = 0x5A4D;  // MZ

//! Sizes in bytes of various things in the COFF format.
namespace STRUCT_SIZES {
enum {
  Header16Size = 20,
  Header32Size = 56,
  NameSize = 8,
  Symbol16Size = 18,
  Symbol32Size = 20,
  SectionSize = 40,
  RelocationSize = 10,
  BaseRelocationBlockSize = 8,
  ImportDirectoryTableEntrySize = 20,
  ResourceDirectoryTableSize = 16,
  ResourceDirectoryEntriesSize = 8,
  ResourceDataEntrySize = 16
};
}

#include "third_party/lief_pe/pe_structures.h"

static_assert(STRUCT_SIZES::SectionSize == sizeof(pe_section), "Compiler options broke LIEF struct layout");
static_assert(STRUCT_SIZES::RelocationSize == sizeof(pe_relocation), "Compiler options broke LIEF struct layout");
static_assert(STRUCT_SIZES::BaseRelocationBlockSize ==
                  sizeof(pe_base_relocation_block),
              "Compiler options broke LIEF struct layout");
static_assert(STRUCT_SIZES::ImportDirectoryTableEntrySize == sizeof(pe_import),
              "Compiler options broke LIEF struct layout");
static_assert(STRUCT_SIZES::ResourceDirectoryTableSize ==
                  sizeof(pe_resource_directory_table),
              "Compiler options broke LIEF struct layout");
static_assert(STRUCT_SIZES::ResourceDirectoryEntriesSize ==
                  sizeof(pe_resource_directory_entries),
              "Compiler options broke LIEF struct layout");
static_assert(STRUCT_SIZES::ResourceDataEntrySize ==
                  sizeof(pe_resource_data_entry),
              "Compiler options broke LIEF struct layout");

class PeFile {
 public:
  PeFile(string_view data) : data_(data) { ok_ = Initialize(); }

  bool IsOpen() const { return ok_; }

  string_view header_region() const { return header_region_; }

  uint32_t section_count() const { return section_count_; }
  string_view section_headers() const { return section_headers_; }
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
  string_view data_;

  pe_dos_header dos_header_;
  pe_header pe_header_;
  string_view header_region_;
  uint32_t section_count_;
  string_view section_headers_;
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

  if ((dos_header_.AddressOfNewExeHeader + sizeof(pe_header)) > data_.size()) {
    // Cannot fit the headers
    return false;
  }

  memcpy(&pe_header_, data_.data() + dos_header_.AddressOfNewExeHeader,
         sizeof(pe_header_));

  if (!std::equal(pe_header_.signature, pe_header_.signature + sizeof(PE_Magic),
                  std::begin(PE_Magic))) {
    // Not a PE file.
    return false;
  }

  // TODO(mj): Parse PE header further to determine this
  is_64bit_ = false;

  section_count_ = pe_header_.NumberOfSections;

  const uint32_t sections_offset = dos_header_.AddressOfNewExeHeader +
                                   sizeof(pe_header) +
                                   pe_header_.SizeOfOptionalHeader;

  auto sections_size = CheckedMul(section_count_, sizeof(pe_section));
  if ((sections_offset + sections_size) > data_.size()) {
    // Cannot fit the headers
    return false;
  }

  header_region_ = GetRegion(0, sections_offset);
  section_headers_ = GetRegion(sections_offset, sections_size);

  return true;
}

class Section {
 public:
  std::string name;
  string_view data;

  uint32_t raw_offset() const { return header_.PointerToRawData; }
  uint32_t raw_size() const { return header_.SizeOfRawData; }

  uint32_t virtual_addr() const { return header_.VirtualAddress; }
  uint32_t virtual_size() const { return header_.VirtualSize; }

  Section(string_view header_data) {
    assert(header_data.size() == sizeof(header_));
    memcpy(&header_, header_data.data(), sizeof(header_));
    data = header_data;

    // TODO(mj): Handle long section names:
    // For longer names, this member contains a forward slash (/) followed by an
    // ASCII representation of a decimal number that is an offset into the
    // string table.
    name = std::string(header_.Name,
                       strnlen(header_.Name, STRUCT_SIZES::NameSize));
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
  ForEachSection(pe, [sink](const Section& section) {
    uint64_t vmaddr = section.virtual_addr();
    uint64_t vmsize = section.virtual_size();

    uint64_t fileoff = section.raw_offset();
    uint64_t filesize = section.raw_size();

    sink->AddRange("pe_sections", section.name, vmaddr, vmsize, fileoff,
                   filesize);
  });
}

void AddCatchAll(const PeFile& pe, RangeSink* sink) {
  // The last-line fallback to make sure we cover the entire VM space.
  assert(pe.IsOpen());

  auto begin = pe.header_region().data() - sink->input_file().data().data();
  sink->AddRange("pe_catchall", "[PE Headers]", begin,
                 pe.header_region().size(), pe.header_region());
  begin = pe.section_headers().data() - sink->input_file().data().data();
  sink->AddRange("pe_catchall", "[PE Headers]", begin,
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
          // TODO(mj): Generate symbols from debug info, exports and other known
          // structures
        case DataSource::kArchiveMembers:
        case DataSource::kCompileUnits:
        case DataSource::kInlines:
        default:
          THROW("PE doesn't support this data source");
      }
      AddCatchAll(*pe_file, sink);
    }
  }

  bool GetDisassemblyInfo(absl::string_view /*symbol*/,
                          DataSource /*symbol_source*/,
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
