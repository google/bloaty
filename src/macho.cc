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

#include <iostream>
#include "string.h"
#include "bloaty.h"
#include "re2/re2.h"

#include <cassert>

#include "absl/strings/string_view.h"
#include "absl/strings/str_join.h"
#include "third_party/darwin_xnu_macho/mach-o/loader.h"
#include "third_party/darwin_xnu_macho/mach-o/fat.h"
#include "third_party/darwin_xnu_macho/mach-o/nlist.h"
#include "third_party/darwin_xnu_macho/mach-o/reloc.h"

ABSL_ATTRIBUTE_NORETURN
static void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

using absl::string_view;

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)
#define WARN(x) fprintf(stderr, "bloaty: %s\n", x);

// segname (& sectname) may NOT be NULL-terminated,
//   i.e. can use up all 16 chars, e.g. '__gcc_except_tab' (no '\0'!)
//   hence specifying size when constructing std::string
static string_view ArrayToStr(const char* s, size_t maxlen) {
  return string_view(s, strnlen(s, maxlen));
}

static uint64_t CheckedAdd(uint64_t a, uint64_t b) {
  absl::uint128 a_128(a), b_128(b);
  absl::uint128 c = a + b;
  if (c > UINT64_MAX) {
    THROW("integer overflow in addition");
  }
  return static_cast<uint64_t>(c);
}

static string_view StrictSubstr(string_view data, size_t off, size_t n) {
  uint64_t end = CheckedAdd(off, n);
  if (end > data.size()) {
    THROW("Mach-O region out-of-bounds");
  }
  return data.substr(off, n);
}

namespace bloaty {

uint32_t ReadMagic(string_view data) {
  if (data.size() < sizeof(uint32_t)) {
    THROW("Malformed Mach-O file");
  }
  uint32_t magic;
  memcpy(&magic, data.data(), sizeof(magic));
  return magic;
}

template <class T>
const T* GetStructPointer(string_view data) {
  if (sizeof(T) > data.size()) {
    THROW("Premature EOF reading Mach-O data.");
  }
  return reinterpret_cast<const T*>(data.data());
}

template <class T>
void AdvancePastStruct(string_view* data) {
  *data = data->substr(sizeof(T));
}

template <class T>
const T* GetStructPointerAndAdvance(string_view* data) {
  const T* ret = GetStructPointer<T>(*data);
  AdvancePastStruct<T>(data);
  return ret;
}

template <class Segment, class Section>
void ParseMachOSegment(string_view command_data, RangeSink* sink) {
  auto segment = GetStructPointerAndAdvance<Segment>(&command_data);

  if (segment->maxprot == VM_PROT_NONE) {
    return;
  }

  string_view segname = ArrayToStr(segment->segname, 16);

  if (sink->data_source() == DataSource::kSegments) {
    sink->AddRange(segname, segment->vmaddr, segment->vmsize, segment->fileoff,
                   segment->filesize);
  } else if (sink->data_source() == DataSource::kSections) {
    uint32_t nsects = segment->nsects;
    for (uint32_t j = 0; j < nsects; j++) {
      auto section = GetStructPointerAndAdvance<Section>(&command_data);

      // filesize equals vmsize unless the section is zerofill
      uint64_t filesize = section->size;
      switch (section->flags & SECTION_TYPE) {
        case S_ZEROFILL:
        case S_GB_ZEROFILL:
        case S_THREAD_LOCAL_ZEROFILL:
          filesize = 0;
          break;
        default:
          break;
      }

      std::string label = absl::StrJoin(
          std::make_tuple(segname, ArrayToStr(section->sectname, 16)), ",");
      sink->AddRange(label, section->addr, section->size, section->offset,
                     filesize);
    }
  } else {
    BLOATY_UNREACHABLE();
  }
}

static void ParseMachODyldInfo(string_view command_data, RangeSink* sink) {
  auto info = GetStructPointer<dyld_info_command>(command_data);

  sink->AddFileRange("Rebase Info", info->rebase_off, info->rebase_size);
  sink->AddFileRange("Binding Info", info->bind_off, info->bind_size);
  sink->AddFileRange("Weak Binding Info", info->weak_bind_off,
                     info->weak_bind_size);
  sink->AddFileRange("Lazy Binding Info", info->lazy_bind_off,
                     info->lazy_bind_size);
  sink->AddFileRange("Export Info", info->export_off, info->export_size);
}

static void ParseSymbolTable(string_view command_data, RangeSink* sink) {
  auto symtab = GetStructPointer<symtab_command>(command_data);

  // TODO(haberman): use 32-bit symbol size where appropriate.
  sink->AddFileRange("Symbol Table", symtab->symoff,
                     symtab->nsyms * sizeof(nlist_64));
  sink->AddFileRange("String Table", symtab->stroff, symtab->strsize);
}

static void ParseDynamicSymbolTable(string_view command_data, RangeSink* sink) {
  auto dysymtab = GetStructPointer<dysymtab_command>(command_data);

  sink->AddFileRange("Table of Contents", dysymtab->tocoff,
                     dysymtab->ntoc * sizeof(dylib_table_of_contents));
  sink->AddFileRange("Module Table", dysymtab->modtaboff,
                     dysymtab->nmodtab * sizeof(dylib_module_64));
  sink->AddFileRange("Referenced Symbol Table", dysymtab->extrefsymoff,
                     dysymtab->nextrefsyms * sizeof(dylib_reference));
  sink->AddFileRange("Indirect Symbol Table", dysymtab->indirectsymoff,
                     dysymtab->nindirectsyms * sizeof(uint32_t));
  sink->AddFileRange("External Relocation Entries", dysymtab->extreloff,
                     dysymtab->nextrel * sizeof(relocation_info));
  sink->AddFileRange("Local Relocation Entries", dysymtab->locreloff,
                     dysymtab->nlocrel * sizeof(struct relocation_info));
}

static void ParseLinkeditCommand(string_view label, string_view command_data,
                                 RangeSink* sink) {
  auto linkedit = GetStructPointer<linkedit_data_command>(command_data);
  sink->AddFileRange(label, linkedit->dataoff, linkedit->datasize);
}

void ParseMachOLoadCommand(uint32_t cmd, string_view command_data,
                           RangeSink* sink) {
  switch (cmd) {
    case LC_SEGMENT_64:
      ParseMachOSegment<segment_command_64, section_64>(command_data, sink);
      break;
    case LC_SEGMENT:
      ParseMachOSegment<segment_command, section>(command_data, sink);
      break;
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY:
      ParseMachODyldInfo(command_data, sink);
      break;
    case LC_SYMTAB:
      ParseSymbolTable(command_data, sink);
      break;
    case LC_DYSYMTAB:
      ParseDynamicSymbolTable(command_data, sink);
      break;
    case LC_CODE_SIGNATURE:
      ParseLinkeditCommand("Code Signature", command_data, sink);
      break;
    case LC_SEGMENT_SPLIT_INFO:
      ParseLinkeditCommand("Segment Split Info", command_data, sink);
      break;
    case LC_FUNCTION_STARTS:
      ParseLinkeditCommand("Function Start Addresses", command_data, sink);
      break;
    case LC_DATA_IN_CODE:
      ParseLinkeditCommand("Table of Non-instructions", command_data, sink);
      break;
    case LC_DYLIB_CODE_SIGN_DRS:
      ParseLinkeditCommand("Code Signing DRs", command_data, sink);
      break;
    case LC_LINKER_OPTIMIZATION_HINT:
      ParseLinkeditCommand("Optimization Hints", command_data, sink);
      break;
  }
}

static void ParseMachOSymbols(string_view command_data, string_view file_data,
                              RangeSink* sink) {
  (void)command_data;
  (void)file_data;
#if 0
  auto symtab = GetStructPointer<symtab_command>(command_data);

  // TODO(haberman): use 32-bit symbol size where appropriate.
  sink->AddFileRange("Symbol Table", symtab->symoff,
                     symtab->nsyms * sizeof(nlist_64));
  sink->AddFileRange("String Table", symtab->stroff, symtab->strsize);
#endif

  std::string cmd = std::string("symbols -noSources -noDemangling ") +
                    sink->input_file().filename();

  // [16 spaces]0x00000001000009e0 (  0x3297) run_tests [FUNC, EXT, LENGTH, NameNList, MangledNameNList, Merged, NList, Dwarf, FunctionStarts]
  // [16 spaces]0x00000001000015a0 (     0x9) __ZN10LineReader5beginEv [FUNC, EXT, LENGTH, NameNList, MangledNameNList, Merged, NList, Dwarf, FunctionStarts]
  // [16 spaces]0x0000000100038468 (     0x8) __ZN3re2L12empty_stringE [NameNList, MangledNameNList, NList]
  RE2 pattern(R"(^\s{16}0x([0-9a-f]+) \(\s*0x([0-9a-f]+)\) (.+) \[((?:FUNC)?))");

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    std::string name;
    std::string maybe_func;
    size_t addr, size;

    if (RE2::PartialMatch(line, pattern, RE2::Hex(&addr), RE2::Hex(&size),
                          &name, &maybe_func)) {
      if (absl::StartsWith(name, "DYLD-STUB")) {
        continue;
      }

      sink->AddVMRange(addr, size, ItaniumDemangle(name, sink->data_source()));
    }
  }
}


template <class Struct>
void ParseMachOHeaderImpl(string_view file_data, RangeSink* sink) {
  string_view header_data = file_data;
  auto header = GetStructPointerAndAdvance<Struct>(&header_data);
  uint32_t ncmds = header->ncmds;

  for (uint32_t i = 0; i < ncmds; i++) {
    auto command = GetStructPointer<load_command>(header_data);
    string_view command_data = StrictSubstr(header_data, 0, command->cmdsize);
    switch (sink->data_source()) {
      case DataSource::kSegments:
      case DataSource::kSections:
        ParseMachOLoadCommand(command->cmd, command_data, sink);
        break;
      case DataSource::kSymbols:
      case DataSource::kRawSymbols:
      case DataSource::kShortSymbols:
      case DataSource::kFullSymbols:
        if (command->cmd == LC_SYMTAB) {
          ParseMachOSymbols(command_data, file_data, sink);
        }
        break;
      default:
        THROW("Unexpected data source");
    }
    header_data = header_data.substr(command->cmdsize);
  }
}

static void ParseMachOHeader(string_view file_data, RangeSink* sink) {
  uint32_t magic = ReadMagic(file_data);
  switch (magic) {
    case MH_MAGIC:
      // We don't expect to see many 32-bit binaries out in the wild.  Apple is
      // aggressively phasing out support for 32-bit binaries:
      //   https://www.macrumors.com/2017/06/06/apple-to-phase-out-32-bit-mac-apps/
      //
      // Still, you can build 32-bit binaries as of this writing, and there are
      // existing 32-bit binaries floating around, so we might as well support
      // them.
      ParseMachOHeaderImpl<mach_header>(file_data, sink);
      break;
    case MH_MAGIC_64:
      ParseMachOHeaderImpl<mach_header_64>(file_data, sink);
      break;
    case MH_CIGAM:
    case MH_CIGAM_64:
      // OS X and Darwin currently only run on x86/x86-64 (little-endian
      // platforms), so we expect basically all Mach-O files to be
      // little-endian.  Additionally, pretty much all CPU architectures are
      // little-endian these days.  ARM has the option to be big-endian, but I
      // can't find any OS that is actually compiled to use big-endian mode.
      // debian-mips is the only big-endian OS I can find (and maybe SPARC).
      //
      // All of this is to say, this case should only happen if you are running
      // Bloaty on debian-mips.  I consider that uncommon enough (and hard
      // enough to test) that we don't support this until there is a
      // demonstrated need.
      THROW("We don't support cross-endian Mach-O files.");
    default:
      THROW("Corrupt Mach-O file");
  }
}

static void ParseMachOFatHeader(string_view file_data, RangeSink* sink) {
  string_view header_data = file_data;
  auto header = GetStructPointerAndAdvance<fat_header>(&header_data);
  assert(ByteSwap(header->magic) == FAT_MAGIC);
  uint32_t nfat_arch = ByteSwap(header->nfat_arch);
  for (uint32_t i = 0; i < nfat_arch; i++) {
    auto arch = GetStructPointerAndAdvance<fat_arch>(&header_data);
    string_view arch_data =
        StrictSubstr(file_data, ByteSwap(arch->offset), ByteSwap(arch->size));
    ParseMachOHeader(arch_data, sink);
  }
}

static void ParseMachOFile(RangeSink* sink) {
  string_view file_data = sink->input_file().data();
  uint32_t magic = ReadMagic(file_data);
  switch (magic) {
    case MH_MAGIC:
    case MH_MAGIC_64:
    case MH_CIGAM:
    case MH_CIGAM_64:
      ParseMachOHeader(file_data, sink);
      break;
    case FAT_CIGAM:
      ParseMachOFatHeader(file_data, sink);
    default:
      // TODO: .a file (AR).
      THROW("Unrecognized Mach-O file");
  }
}

class MachOObjectFile : public ObjectFile {
 public:
  MachOObjectFile(std::unique_ptr<InputFile> file_data)
      : ObjectFile(std::move(file_data)) {}

  void ProcessBaseMap(RangeSink* sink) override {
    ParseMachOFile(sink);
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
        case DataSource::kSymbols:
        case DataSource::kRawSymbols:
        case DataSource::kShortSymbols:
        case DataSource::kFullSymbols:
          ParseMachOFile(sink);
          break;
        case DataSource::kArchiveMembers:
        case DataSource::kCompileUnits:
        case DataSource::kInlines:
        default:
          THROW("Mach-O doesn't support this data source");
      }
    }
  }

  bool GetDisassemblyInfo(absl::string_view /*symbol*/,
                          DataSource /*symbol_source*/,
                          DisassemblyInfo* /*info*/) override {
    WARN("Mach-O files do not support disassembly yet");
    return false;
  }
};

std::unique_ptr<ObjectFile> TryOpenMachOFile(std::unique_ptr<InputFile> &file) {
  uint32_t magic = ReadMagic(file->data());

  // We only support little-endian host and little endian binaries (see
  // ParseMachOHeader() for more rationale).  Fat headers are always on disk as
  // big-endian.
  if (magic == MH_MAGIC || magic == MH_MAGIC_64 || magic == FAT_CIGAM) {
    return std::unique_ptr<ObjectFile>(new MachOObjectFile(std::move(file)));
  }

  return nullptr;
}

}  // namespace bloaty
