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

#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
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

namespace bloaty {
namespace macho {

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

string_view ReadNullTerminated(string_view data) {
  const char* nullz =
      static_cast<const char*>(memchr(data.data(), '\0', data.size()));

  // Return false if not NULL-terminated.
  if (nullz == NULL) {
    THROW("DWARF string was not NULL-terminated");
  }

  size_t len = nullz - data.data();
  return data.substr(0, len);
}

template <class T>
const T* GetStructPointerAndAdvance(string_view* data) {
  const T* ret = GetStructPointer<T>(*data);
  AdvancePastStruct<T>(data);
  return ret;
}

void MaybeAddOverhead(RangeSink* sink, const char* label, string_view data) {
  if (sink) {
    sink->AddFileRange("macho_overhead", label, data);
  }
}

template <class Struct, class Func>
void ParseMachOHeaderImpl(string_view macho_data, RangeSink* overhead_sink,
                          Func&& loadcmd_func) {
  string_view header_data = macho_data;
  auto header = GetStructPointerAndAdvance<Struct>(&header_data);
  MaybeAddOverhead(overhead_sink,
                   "[Mach-O Headers]",
                   macho_data.substr(0, sizeof(Struct)));
  uint32_t ncmds = header->ncmds;

  for (uint32_t i = 0; i < ncmds; i++) {
    auto command = GetStructPointer<load_command>(header_data);
    string_view command_data = StrictSubstr(header_data, 0, command->cmdsize);
    std::forward<Func>(loadcmd_func)(command->cmd, command_data, macho_data);
    MaybeAddOverhead(overhead_sink, "[Mach-O Headers]", command_data);
    header_data = header_data.substr(command->cmdsize);
  }
}

template <class Func>
void ParseMachOHeader(string_view macho_file, RangeSink* overhead_sink,
                      Func&& loadcmd_func) {
  uint32_t magic = ReadMagic(macho_file);
  switch (magic) {
    case MH_MAGIC:
      // We don't expect to see many 32-bit binaries out in the wild.
      // Apple is aggressively phasing out support for 32-bit binaries:
      //   https://www.macrumors.com/2017/06/06/apple-to-phase-out-32-bit-mac-apps/
      //
      // Still, you can build 32-bit binaries as of this writing, and
      // there are existing 32-bit binaries floating around, so we might
      // as well support them.
      ParseMachOHeaderImpl<mach_header>(macho_file, overhead_sink,
                                        std::forward<Func>(loadcmd_func));
      break;
    case MH_MAGIC_64:
      ParseMachOHeaderImpl<mach_header_64>(
          macho_file, overhead_sink, std::forward<Func>(loadcmd_func));
      break;
    case MH_CIGAM:
    case MH_CIGAM_64:
      // OS X and Darwin currently only run on x86/x86-64 (little-endian
      // platforms), so we expect basically all Mach-O files to be
      // little-endian.  Additionally, pretty much all CPU architectures
      // are little-endian these days.  ARM has the option to be
      // big-endian, but I can't find any OS that is actually compiled to
      // use big-endian mode.  debian-mips is the only big-endian OS I can
      // find (and maybe SPARC).
      //
      // All of this is to say, this case should only happen if you are
      // running Bloaty on debian-mips.  I consider that uncommon enough
      // (and hard enough to test) that we don't support this until there
      // is a demonstrated need.
      THROW("We don't support cross-endian Mach-O files.");
    default:
      THROW("Corrupt Mach-O file");
  }
}

template <class Func>
void ParseFatHeader(string_view fat_file, RangeSink* overhead_sink,
                    Func&& loadcmd_func) {
  string_view header_data = fat_file;
  auto header = GetStructPointerAndAdvance<fat_header>(&header_data);
  MaybeAddOverhead(overhead_sink, "[Mach-O Headers]",
                   fat_file.substr(0, sizeof(fat_header)));
  assert(ByteSwap(header->magic) == FAT_MAGIC);
  uint32_t nfat_arch = ByteSwap(header->nfat_arch);
  for (uint32_t i = 0; i < nfat_arch; i++) {
    auto arch = GetStructPointerAndAdvance<fat_arch>(&header_data);
    string_view macho_data = StrictSubstr(
        fat_file, ByteSwap(arch->offset), ByteSwap(arch->size));
    ParseMachOHeader(macho_data, overhead_sink,
                     std::forward<Func>(loadcmd_func));
  }
}

template <class Func>
void ForEachLoadCommand(string_view maybe_fat_file, RangeSink* overhead_sink,
                        Func&& loadcmd_func) {
  uint32_t magic = ReadMagic(maybe_fat_file);
  switch (magic) {
    case MH_MAGIC:
    case MH_MAGIC_64:
    case MH_CIGAM:
    case MH_CIGAM_64:
      ParseMachOHeader(maybe_fat_file, overhead_sink,
                       std::forward<Func>(loadcmd_func));
      break;
    case FAT_CIGAM:
      ParseFatHeader(maybe_fat_file, overhead_sink,
                     std::forward<Func>(loadcmd_func));
      break;
  }
}

template <class Segment, class Section>
void AddSegmentAsFallback(string_view command_data, string_view file_data,
                          RangeSink* sink) {
  auto segment = GetStructPointerAndAdvance<Segment>(&command_data);

  if (segment->maxprot == VM_PROT_NONE) {
    return;
  }

  string_view segname = ArrayToStr(segment->segname, 16);

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
    label = "[" + label + "]";
    sink->AddRange("macho_fallback", label, section->addr, section->size,
                   StrictSubstr(file_data, section->offset, filesize));
  }

  sink->AddRange("macho_fallback", "[" + std::string(segname) + "]",
                 segment->vmaddr, segment->vmsize,
                 StrictSubstr(file_data, segment->fileoff, segment->filesize));
}

template <class Segment, class Section>
void ParseSegment(string_view command_data, string_view file_data,
                  RangeSink* sink) {
  auto segment = GetStructPointerAndAdvance<Segment>(&command_data);

  if (segment->maxprot == VM_PROT_NONE) {
    return;
  }

  string_view segname = ArrayToStr(segment->segname, 16);

  if (sink->data_source() == DataSource::kSegments) {
    sink->AddRange(
        "macho_segment", segname, segment->vmaddr, segment->vmsize,
        StrictSubstr(file_data, segment->fileoff, segment->filesize));
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
      sink->AddRange("macho_section", label, section->addr, section->size,
                     StrictSubstr(file_data, section->offset, filesize));
    }
  } else {
    BLOATY_UNREACHABLE();
  }
}

static void ParseDyldInfo(string_view command_data, string_view file_data,
                          RangeSink* sink) {
  auto info = GetStructPointer<dyld_info_command>(command_data);

  sink->AddFileRange(
      "macho_dyld", "Rebase Info",
      StrictSubstr(file_data, info->rebase_off, info->rebase_size));
  sink->AddFileRange("macho_dyld", "Binding Info",
                     StrictSubstr(file_data, info->bind_off, info->bind_size));
  sink->AddFileRange(
      "macho_dyld", "Weak Binding Info",
      StrictSubstr(file_data, info->weak_bind_off, info->weak_bind_size));
  sink->AddFileRange(
      "macho_dyld", "Lazy Binding Info",
      StrictSubstr(file_data, info->lazy_bind_off, info->lazy_bind_size));
  sink->AddFileRange(
      "macho_dyld", "Export Info",
      StrictSubstr(file_data, info->export_off, info->export_size));
}

static void ParseSymbolTable(string_view command_data, string_view file_data,
                             RangeSink* sink) {
  auto symtab = GetStructPointer<symtab_command>(command_data);

  // TODO(haberman): use 32-bit symbol size where appropriate.
  sink->AddFileRange("macho_symtab", "Symbol Table",
                     StrictSubstr(file_data, symtab->symoff,
                                  symtab->nsyms * sizeof(nlist_64)));
  sink->AddFileRange("macho_symtab", "String Table",
                     StrictSubstr(file_data, symtab->stroff, symtab->strsize));
}

static void ParseDynamicSymbolTable(string_view command_data,
                                    string_view file_data, RangeSink* sink) {
  auto dysymtab = GetStructPointer<dysymtab_command>(command_data);

  sink->AddFileRange(
      "macho_dynsymtab", "Table of Contents",
      StrictSubstr(file_data, dysymtab->tocoff,
                   dysymtab->ntoc * sizeof(dylib_table_of_contents)));
  sink->AddFileRange("macho_dynsymtab", "Module Table",
                     StrictSubstr(file_data, dysymtab->modtaboff,
                                  dysymtab->nmodtab * sizeof(dylib_module_64)));
  sink->AddFileRange(
      "macho_dynsymtab", "Referenced Symbol Table",
      StrictSubstr(file_data, dysymtab->extrefsymoff,
                   dysymtab->nextrefsyms * sizeof(dylib_reference)));
  sink->AddFileRange("macho_dynsymtab", "Indirect Symbol Table",
                     StrictSubstr(file_data, dysymtab->indirectsymoff,
                                  dysymtab->nindirectsyms * sizeof(uint32_t)));
  sink->AddFileRange("macho_dynsymtab", "External Relocation Entries",
                     StrictSubstr(file_data, dysymtab->extreloff,
                                  dysymtab->nextrel * sizeof(relocation_info)));
  sink->AddFileRange(
      "macho_dynsymtab", "Local Relocation Entries",
      StrictSubstr(file_data, dysymtab->locreloff,
                   dysymtab->nlocrel * sizeof(struct relocation_info)));
}

static void ParseLinkeditCommand(string_view label, string_view command_data,
                                 string_view file_data, RangeSink* sink) {
  auto linkedit = GetStructPointer<linkedit_data_command>(command_data);
  sink->AddFileRange(
      "macho_linkedit", label,
      StrictSubstr(file_data, linkedit->dataoff, linkedit->datasize));
}

void ParseLoadCommand(uint32_t cmd, string_view command_data,
                      string_view file_data, RangeSink* sink) {
  switch (cmd) {
    case LC_SEGMENT_64:
      ParseSegment<segment_command_64, section_64>(command_data, file_data,
                                                   sink);
      break;
    case LC_SEGMENT:
      ParseSegment<segment_command, section>(command_data, file_data, sink);
      break;
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY:
      ParseDyldInfo(command_data, file_data, sink);
      break;
    case LC_SYMTAB:
      ParseSymbolTable(command_data, file_data, sink);
      break;
    case LC_DYSYMTAB:
      ParseDynamicSymbolTable(command_data, file_data, sink);
      break;
    case LC_CODE_SIGNATURE:
      ParseLinkeditCommand("Code Signature", command_data, file_data, sink);
      break;
    case LC_SEGMENT_SPLIT_INFO:
      ParseLinkeditCommand("Segment Split Info", command_data, file_data, sink);
      break;
    case LC_FUNCTION_STARTS:
      ParseLinkeditCommand("Function Start Addresses", command_data, file_data,
                           sink);
      break;
    case LC_DATA_IN_CODE:
      ParseLinkeditCommand("Table of Non-instructions", command_data, file_data,
                           sink);
      break;
    case LC_DYLIB_CODE_SIGN_DRS:
      ParseLinkeditCommand("Code Signing DRs", command_data, file_data, sink);
      break;
    case LC_LINKER_OPTIMIZATION_HINT:
      ParseLinkeditCommand("Optimization Hints", command_data, file_data, sink);
      break;
  }
}

void ParseLoadCommands(RangeSink* sink) {
  ForEachLoadCommand(
      sink->input_file().data(), sink,
      [sink](uint32_t cmd, string_view command_data, string_view file_data) {
        ParseLoadCommand(cmd, command_data, file_data, sink);
      });
}

void ParseSymbolsFromSymbolTable(string_view command_data, string_view file_data, RangeSink* sink) {
  auto symtab_cmd = GetStructPointer<symtab_command>(command_data);

  // TODO(haberman): use 32-bit symbol size where appropriate.
  string_view symtab = StrictSubstr(file_data, symtab_cmd->symoff,
                                    symtab_cmd->nsyms * sizeof(nlist_64));
  string_view strtab =
      StrictSubstr(file_data, symtab_cmd->stroff, symtab_cmd->strsize);

  uint32_t nsyms = symtab_cmd->nsyms;
  for (uint32_t i = 0; i < nsyms; i++) {
    auto sym = GetStructPointerAndAdvance<nlist_64>(&symtab);

    if (sym->n_type & N_STAB || sym->n_value == 0) {
      continue;
    }

    string_view name = ReadNullTerminated(strtab.substr(sym->n_un.n_strx));
    sink->AddVMRange("macho_symbols", sym->n_value, RangeSink::kUnknownSize,
                     ItaniumDemangle(name, sink->data_source()));
  }
}

void ParseSymbols(RangeSink* sink) {
  ForEachLoadCommand(
      sink->input_file().data(), sink,
      [sink](uint32_t cmd, string_view command_data, string_view file_data) {
        switch (cmd) {
          case LC_SYMTAB:
            ParseSymbolsFromSymbolTable(command_data, file_data, sink);
            break;
          case LC_DYSYMTAB:
            //ParseSymbolsFromDynamicSymbolTable(command_data, file_data, sink);
            break;
        }
      });
}

static void AddMachOFallback(RangeSink* sink) {
  ForEachLoadCommand(
      sink->input_file().data(), sink,
      [sink](uint32_t cmd, string_view command_data, string_view file_data) {
        switch (cmd) {
          case LC_SEGMENT_64:
            AddSegmentAsFallback<segment_command_64, section_64>(
                command_data, file_data, sink);
            break;
          case LC_SEGMENT:
            AddSegmentAsFallback<segment_command, section>(command_data,
                                                           file_data, sink);
            break;
        }
      });
  sink->AddFileRange("macho_fallback", "[Unmapped]", sink->input_file().data());
}

class MachOObjectFile : public ObjectFile {
 public:
  MachOObjectFile(std::unique_ptr<InputFile> file_data)
      : ObjectFile(std::move(file_data)) {}

  std::string GetBuildId() const override {
    // TODO(haberman): implement.
    return std::string();
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) const override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
          ParseLoadCommands(sink);
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
          THROW("Mach-O doesn't support this data source");
      }
      AddMachOFallback(sink);
    }
  }

  bool GetDisassemblyInfo(absl::string_view /*symbol*/,
                          DataSource /*symbol_source*/,
                          DisassemblyInfo* /*info*/) const override {
    WARN("Mach-O files do not support disassembly yet");
    return false;
  }
};

}  // namespace macho

std::unique_ptr<ObjectFile> TryOpenMachOFile(std::unique_ptr<InputFile> &file) {
  uint32_t magic = macho::ReadMagic(file->data());

  // We only support little-endian host and little endian binaries (see
  // ParseMachOHeader() for more rationale).  Fat headers are always on disk as
  // big-endian.
  if (magic == MH_MAGIC || magic == MH_MAGIC_64 || magic == FAT_CIGAM) {
    return std::unique_ptr<ObjectFile>(
        new macho::MachOObjectFile(std::move(file)));
  }

  return nullptr;
}

}  // namespace bloaty
