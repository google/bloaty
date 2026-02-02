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
#include "util.h"

#include <cassert>
#include <string_view>

#include "absl/strings/str_join.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "arfile.h"
#include "third_party/darwin_xnu_macho/mach/machine.h"
#include "third_party/darwin_xnu_macho/mach-o/fat.h"
#include "third_party/darwin_xnu_macho/mach-o/loader.h"
#include "third_party/darwin_xnu_macho/mach-o/nlist.h"
#include "third_party/darwin_xnu_macho/mach-o/reloc.h"

using std::string_view;

namespace bloaty {
namespace macho {

// segname (& sectname) may NOT be NULL-terminated,
//   i.e. can use up all 16 chars, e.g. '__gcc_except_tab' (no '\0'!)
//   hence specifying size when constructing std::string
static string_view ArrayToStr(const char* s, size_t maxlen) {
  return string_view(s, strnlen(s, maxlen));
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
const T* GetStructPointerAndAdvance(string_view* data) {
  const T* ret = GetStructPointer<T>(*data);
  *data = data->substr(sizeof(T));
  return ret;
}

void MaybeAddOverhead(RangeSink* sink, const char* label, string_view data) {
  if (sink) {
    sink->AddFileRange("macho_overhead", label, data);
  }
}

// ARM64E capability field constants
static constexpr uint32_t ARM64E_SUBTYPE_MASK = 0x00FFFFFF;  // Low 24 bits: subtype proper

static bool IsArm64eSubtype(uint32_t cpusubtype) {
  uint32_t subtype_proper = cpusubtype & ARM64E_SUBTYPE_MASK;
  return subtype_proper == CPU_SUBTYPE_ARM64E;
}

std::string CpuTypeToString(uint32_t cputype, uint32_t cpusubtype) {
  switch (cputype) {
    case CPU_TYPE_X86_64:
      switch (cpusubtype) {
        case CPU_SUBTYPE_X86_64_H:
          return "x86_64h";
        default:
          return "x86_64";
      }
    case CPU_TYPE_ARM64:
      if (IsArm64eSubtype(cpusubtype)) {
        return "arm64e";
      }
      switch (cpusubtype) {
        case CPU_SUBTYPE_ARM64_V8:
          return "arm64v8";
        default:
          return "arm64";
      }
    case CPU_TYPE_X86:
      return "i386";
    case CPU_TYPE_ARM:
      switch (cpusubtype) {
        case CPU_SUBTYPE_ARM_V6:
          return "armv6";
        case CPU_SUBTYPE_ARM_V7:
          return "armv7";
        case CPU_SUBTYPE_ARM_V7F:
          return "armv7f";
        case CPU_SUBTYPE_ARM_V7S:
          return "armv7s";
        case CPU_SUBTYPE_ARM_V7K:
          return "armv7k";
        case CPU_SUBTYPE_ARM_V8:
          return "armv8";
        default:
          return "arm";
      }
    default:
      return absl::StrFormat("cpu_%d", cputype);
  }
}

void MaybeAddFileRange(const char* analyzer, RangeSink* sink, string_view label,
                       string_view range) {
  if (sink) {
    sink->AddFileRange(analyzer, label, range);
  }
}

static bool IsMachOContent(string_view data, std::string* error_msg = nullptr) {
  if (data.size() < sizeof(uint32_t)) {
    if (error_msg) *error_msg = "File too small for Mach-O header";
    return false;
  }

  if (data.size() == 0 || std::all_of(data.begin(), data.begin() + std::min(data.size(), size_t(64)),
                                      [](char c) { return c == 0; })) {
    if (error_msg) *error_msg = "File appears to be empty or all zeros";
    return false;
  }

  try {
    uint32_t magic = macho::ReadMagic(data);
    switch (magic) {
      case MH_MAGIC:
      case MH_MAGIC_64:
      case MH_CIGAM:
      case MH_CIGAM_64:
        return true;
      case FAT_MAGIC:
      case FAT_CIGAM:
        return true;
      default:
        if (error_msg) *error_msg = absl::StrFormat("Unknown magic: 0x%08x", magic);
        return false;
    }
  } catch (const std::exception& e) {
    if (error_msg) *error_msg = std::string("Parse error: ") + e.what();
    return false;
  }
}

struct LoadCommand {
  bool is64bit;
  uint32_t cmd;
  string_view command_data;
  string_view file_data;
  string_view filename;
};

template <class Struct>
bool Is64Bit() { return false; }

template <>
bool Is64Bit<mach_header_64>() { return true; }

template <class Struct, class Func>
void ParseMachOHeaderImpl(string_view macho_data, RangeSink* overhead_sink,
                          string_view filename, Func&& loadcmd_func) {
  string_view header_data = macho_data;
  auto header = GetStructPointerAndAdvance<Struct>(&header_data);
  MaybeAddOverhead(overhead_sink,
                   "[Mach-O Headers]",
                   macho_data.substr(0, sizeof(Struct)));
  uint32_t ncmds = header->ncmds;

  for (uint32_t i = 0; i < ncmds; i++) {
    auto command = GetStructPointer<load_command>(header_data);

    // We test for this because otherwise a large ncmds can make bloaty hang for
    // a while, even on a small file.  Hopefully there are no real cases where a
    // zero-size loadcmd exists.
    if (command->cmdsize == 0) {
      THROW("Mach-O load command had zero size.");
    }

    LoadCommand data;
    data.is64bit = Is64Bit<Struct>();
    data.cmd = command->cmd;
    data.command_data = StrictSubstr(header_data, 0, command->cmdsize);
    data.file_data = macho_data;
    data.filename = filename;
    std::forward<Func>(loadcmd_func)(data);

    MaybeAddOverhead(overhead_sink, "[Mach-O Headers]", data.command_data);
    header_data = header_data.substr(command->cmdsize);
  }
}

template <class Func>
void ParseMachOHeader(string_view macho_file, RangeSink* overhead_sink,
                      string_view filename, Func&& loadcmd_func) {
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
                                        filename, std::forward<Func>(loadcmd_func));
      break;
    case MH_MAGIC_64:
      ParseMachOHeaderImpl<mach_header_64>(
          macho_file, overhead_sink, filename, std::forward<Func>(loadcmd_func));
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
void ParseArchiveMembers(string_view archive_data, RangeSink* overhead_sink,
                         string_view arch_suffix, Func&& loadcmd_func) {
  ArFile ar_file(archive_data);
  if (!ar_file.IsOpen()) {
    return;
  }

  ArFile::MemberFile member;
  ArFile::MemberReader reader(ar_file);
  MaybeAddFileRange("ar_archive", overhead_sink, "[AR Headers]", ar_file.magic());

  while (reader.ReadMember(&member)) {
    MaybeAddFileRange("ar_archive", overhead_sink, "[AR Headers]", member.header);

    switch (member.file_type) {
      case ArFile::MemberFile::kNormal: {
        std::string error_msg;
        if (IsMachOContent(member.contents, &error_msg)) {
          try {
            std::string member_name = arch_suffix.empty()
                ? std::string(member.filename)
                : absl::StrFormat("%s [%s]", member.filename, arch_suffix);

            uint32_t magic = macho::ReadMagic(member.contents);
            if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
              ParseFatHeader(member.contents, overhead_sink, member_name,
                             std::forward<Func>(loadcmd_func));
            } else {
              ParseMachOHeader(member.contents, overhead_sink, member_name,
                               std::forward<Func>(loadcmd_func));
            }
          } catch (const std::exception& e) {
            WARN("Failed to parse archive member '$0': $1", member.filename, e.what());
          }
        } else {
          std::string label = arch_suffix.empty()
              ? absl::StrFormat("[AR Non-Mach-O: %s]", member.filename)
              : absl::StrFormat("[AR Non-Mach-O: %s [%s]]", member.filename, arch_suffix);
          MaybeAddFileRange("ar_archive", overhead_sink, label, member.contents);
        }
        break;
      }
      case ArFile::MemberFile::kSymbolTable:
        MaybeAddFileRange("ar_archive", overhead_sink, "[AR Symbol Table]", member.contents);
        break;
      case ArFile::MemberFile::kLongFilenameTable:
        MaybeAddFileRange("ar_archive", overhead_sink, "[AR Headers]", member.contents);
        break;
    }
  }
}

template <class Func>
void ParseFatHeader(string_view fat_file, RangeSink* overhead_sink,
                    string_view filename, Func&& loadcmd_func) {
  string_view header_data = fat_file;
  auto header = GetStructPointerAndAdvance<fat_header>(&header_data);
  MaybeAddOverhead(overhead_sink, "[Mach-O Headers]",
                   fat_file.substr(0, sizeof(fat_header)));

  bool need_swap = (header->magic == FAT_CIGAM);
  if (header->magic != FAT_MAGIC && header->magic != FAT_CIGAM) {
    THROW("Invalid FAT magic");
  }

  uint32_t nfat_arch = need_swap ? ByteSwap(header->nfat_arch) : header->nfat_arch;

  if (nfat_arch > header_data.size() / sizeof(fat_arch)) {
    THROW("invalid nfat_arch count in universal binary header");
  }

  // Process all architectures in universal binaries.
  // Use --source-filter to filter to a specific architecture.

  for (uint32_t i = 0; i < nfat_arch; i++) {
    auto arch = GetStructPointerAndAdvance<fat_arch>(&header_data);

    uint32_t offset = need_swap ? ByteSwap(arch->offset) : arch->offset;
    uint32_t size = need_swap ? ByteSwap(arch->size) : arch->size;
    uint32_t cputype = need_swap ? ByteSwap(arch->cputype) : arch->cputype;
    uint32_t cpusubtype = need_swap ? ByteSwap(arch->cpusubtype) : arch->cpusubtype;

    string_view arch_data = StrictSubstr(fat_file, offset, size);
    std::string arch_name = CpuTypeToString(cputype, cpusubtype);

    ArFile ar_file(arch_data);
    if (ar_file.IsOpen()) {
      ParseArchiveMembers(arch_data, overhead_sink, arch_name,
                          std::forward<Func>(loadcmd_func));
    } else {
      // If this is an archive member, append architecture name
      std::string arch_filename;
      if (!filename.empty()) {
        arch_filename = absl::StrFormat("%s [%s]", filename, arch_name);
        ParseMachOHeader(arch_data, overhead_sink, arch_filename,
                         std::forward<Func>(loadcmd_func));
      } else {
        ParseMachOHeader(arch_data, overhead_sink, "",
                         std::forward<Func>(loadcmd_func));
      }
    }
  }
}

static bool g_warned_about_universal_in_archive = false;

template <class Func>
void ForEachLoadCommand(string_view maybe_fat_file, RangeSink* overhead_sink,
                        Func&& loadcmd_func) {
  uint32_t magic = ReadMagic(maybe_fat_file);
  switch (magic) {
    case MH_MAGIC:
    case MH_MAGIC_64:
    case MH_CIGAM:
    case MH_CIGAM_64:
      ParseMachOHeader(maybe_fat_file, overhead_sink, "",
                       std::forward<Func>(loadcmd_func));
      break;
    case FAT_CIGAM:
    case FAT_MAGIC:
      ParseFatHeader(maybe_fat_file, overhead_sink, "",
                     std::forward<Func>(loadcmd_func));
      break;
  }

  ArFile ar_file(maybe_fat_file);

  if (ar_file.IsOpen()) {
    ArFile::MemberFile member;
    ArFile::MemberReader reader(ar_file);
    MaybeAddFileRange("ar_archive", overhead_sink, "[AR Headers]", ar_file.magic());

    int processed_count = 0;
    int skipped_count = 0;
    bool has_universal_binaries = false;

    while (reader.ReadMember(&member)) {
      MaybeAddFileRange("ar_archive", overhead_sink, "[AR Headers]", member.header);

      switch (member.file_type) {
        case ArFile::MemberFile::kNormal: {
          std::string error_msg;
          if (IsMachOContent(member.contents, &error_msg)) {
            try {
              uint32_t magic = macho::ReadMagic(member.contents);
              switch (magic) {
                case MH_MAGIC:
                case MH_MAGIC_64:
                case MH_CIGAM:
                case MH_CIGAM_64:
                  ParseMachOHeader(member.contents, overhead_sink, member.filename,
                           std::forward<Func>(loadcmd_func));
                  processed_count++;
                  break;
                case FAT_MAGIC:
                case FAT_CIGAM:
                  has_universal_binaries = true;
                  ParseFatHeader(member.contents, overhead_sink, member.filename,
                         std::forward<Func>(loadcmd_func));
                  processed_count++;
                  break;
                default:
                  // This shouldn't happen with IsMachOContent check but be safe
                  MaybeAddFileRange("ar_archive", overhead_sink,
                                   absl::StrFormat("[AR Unknown Mach-O: %s]", member.filename),
                                   member.contents);
                  skipped_count++;
              }
            } catch (const std::exception& e) {
              WARN("Failed to parse Mach-O member '$0': $1", member.filename, e.what());
              MaybeAddFileRange("ar_archive", overhead_sink,
                               absl::StrFormat("[AR Corrupt Mach-O: %s]", member.filename),
                               member.contents);
              skipped_count++;
            }
          } else {
            MaybeAddFileRange("ar_archive", overhead_sink,
                             absl::StrFormat("[AR Non-Mach-O: %s]", member.filename),
                             member.contents);
            skipped_count++;
          }
          break;
        }
        case ArFile::MemberFile::kSymbolTable:
          MaybeAddFileRange("ar_archive", overhead_sink, "[AR Symbol Table]",
                            member.contents);
          break;
        case ArFile::MemberFile::kLongFilenameTable:
          MaybeAddFileRange("ar_archive", overhead_sink, "[AR Headers]",
                            member.contents);
          break;
      }
    }

    if (verbose_level > 1 && (processed_count > 0 || skipped_count > 0)) {
      printf("Archive processing complete: %d Mach-O members processed, %d skipped\n",
             processed_count, skipped_count);
    }

    // Warn when processing universal binaries without --source-filter.
    // Each architecture in a universal binary has its own independent VM address
    // space, so summing VM sizes across different architectures is meaningless
    // Use --domain=file for meaningful size comparisons, or --source-filter
    // to filter to a single architecture.
    if (has_universal_binaries && overhead_sink && !g_warned_about_universal_in_archive) {
      fprintf(stderr, "Warning: Archive contains universal binaries. VM size totals across different architectures are not meaningful. Consider using --domain=file or --source-filter=<architecture> to filter to a specific architecture.\n");
      g_warned_about_universal_in_archive = true;
    }
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
void ParseSegment(LoadCommand cmd, RangeSink* sink) {
  auto segment = GetStructPointerAndAdvance<Segment>(&cmd.command_data);
  string_view segname = ArrayToStr(segment->segname, 16);

  // For unknown reasons, some load commands will have maxprot = NONE
  // indicating they are not accessible, but will also contain a vmaddr
  // and vmsize.  In practice the vmaddr/vmsize of a section sometimes
  // fall within the segment, but sometimes exceed it, leading to an
  // error about exceeding the base map.
  //
  // Since such segments should not be mapped, we simply ignore the
  // vmaddr/vmsize of such segments.
  bool unmapped = segment->maxprot == VM_PROT_NONE;

  if (sink->data_source() == DataSource::kSegments) {
    if (unmapped) {
      sink->AddFileRange(
          "macho_segment", segname,
          StrictSubstr(cmd.file_data, segment->fileoff, segment->filesize));
    } else {
      sink->AddRange(
          "macho_segment", segname, segment->vmaddr, segment->vmsize,
          StrictSubstr(cmd.file_data, segment->fileoff, segment->filesize));
    }
  } else if (sink->data_source() == DataSource::kSections) {
    uint32_t nsects = segment->nsects;
    for (uint32_t j = 0; j < nsects; j++) {
      auto section = GetStructPointerAndAdvance<Section>(&cmd.command_data);

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
      if (unmapped) {
        sink->AddFileRange(
            "macho_section", label,
            StrictSubstr(cmd.file_data, section->offset, filesize));
      } else {
        sink->AddRange("macho_section", label, section->addr, section->size,
                       StrictSubstr(cmd.file_data, section->offset, filesize));
      }
    }
  } else if (sink->data_source() == DataSource::kArchiveMembers) {
    if (!cmd.filename.empty()) {
      if (unmapped) {
        sink->AddFileRange(
            "macho_armember", cmd.filename,
            StrictSubstr(cmd.file_data, segment->fileoff, segment->filesize));
      } else {
        sink->AddRange("macho_armember", cmd.filename, segment->vmaddr, segment->vmsize,
                       StrictSubstr(cmd.file_data, segment->fileoff, segment->filesize));
      }
    }
  } else {
    BLOATY_UNREACHABLE();
  }
}

static bool IsObjectFile(string_view data) {
  string_view header_data = data;
  auto header = GetStructPointerAndAdvance<mach_header>(&header_data);
  return header->filetype == MH_OBJECT;
}

static void CheckNotObject(const char* source, RangeSink* sink) {
  if (IsObjectFile(sink->input_file().data())) {
    THROWF(
        "can't use data source '$0' on object files (only binaries and shared "
        "libraries)",
        source);
  }
}

static void ParseDyldInfo(const LoadCommand& cmd, RangeSink* sink) {
  auto info = GetStructPointer<dyld_info_command>(cmd.command_data);

  sink->AddFileRange(
      "macho_dyld", "Rebase Info",
      StrictSubstr(cmd.file_data, info->rebase_off, info->rebase_size));
  sink->AddFileRange(
      "macho_dyld", "Binding Info",
      StrictSubstr(cmd.file_data, info->bind_off, info->bind_size));
  sink->AddFileRange(
      "macho_dyld", "Weak Binding Info",
      StrictSubstr(cmd.file_data, info->weak_bind_off, info->weak_bind_size));
  sink->AddFileRange(
      "macho_dyld", "Lazy Binding Info",
      StrictSubstr(cmd.file_data, info->lazy_bind_off, info->lazy_bind_size));
  sink->AddFileRange(
      "macho_dyld", "Export Info",
      StrictSubstr(cmd.file_data, info->export_off, info->export_size));
}

static void ParseSymbolTable(const LoadCommand& cmd, RangeSink* sink) {
  auto symtab = GetStructPointer<symtab_command>(cmd.command_data);

  size_t size = cmd.is64bit ? sizeof(nlist_64) : sizeof(struct nlist);
  sink->AddFileRange(
      "macho_symtab", "Symbol Table",
      StrictSubstr(cmd.file_data, symtab->symoff, symtab->nsyms * size));
  sink->AddFileRange(
      "macho_symtab", "String Table",
      StrictSubstr(cmd.file_data, symtab->stroff, symtab->strsize));
}

static void ParseDynamicSymbolTable(const LoadCommand& cmd, RangeSink* sink) {
  auto dysymtab = GetStructPointer<dysymtab_command>(cmd.command_data);

  sink->AddFileRange(
      "macho_dynsymtab", "Table of Contents",
      StrictSubstr(cmd.file_data, dysymtab->tocoff,
                   dysymtab->ntoc * sizeof(dylib_table_of_contents)));
  sink->AddFileRange("macho_dynsymtab", "Module Table",
                     StrictSubstr(cmd.file_data, dysymtab->modtaboff,
                                  dysymtab->nmodtab * sizeof(dylib_module_64)));
  sink->AddFileRange(
      "macho_dynsymtab", "Referenced Symbol Table",
      StrictSubstr(cmd.file_data, dysymtab->extrefsymoff,
                   dysymtab->nextrefsyms * sizeof(dylib_reference)));
  sink->AddFileRange("macho_dynsymtab", "Indirect Symbol Table",
                     StrictSubstr(cmd.file_data, dysymtab->indirectsymoff,
                                  dysymtab->nindirectsyms * sizeof(uint32_t)));
  sink->AddFileRange("macho_dynsymtab", "External Relocation Entries",
                     StrictSubstr(cmd.file_data, dysymtab->extreloff,
                                  dysymtab->nextrel * sizeof(relocation_info)));
  sink->AddFileRange(
      "macho_dynsymtab", "Local Relocation Entries",
      StrictSubstr(cmd.file_data, dysymtab->locreloff,
                   dysymtab->nlocrel * sizeof(struct relocation_info)));
}

static void ParseLinkeditCommand(string_view label, const LoadCommand& cmd,
                                 RangeSink* sink) {
  auto linkedit = GetStructPointer<linkedit_data_command>(cmd.command_data);
  sink->AddFileRange(
      "macho_linkedit", label,
      StrictSubstr(cmd.file_data, linkedit->dataoff, linkedit->datasize));
}

void ParseLoadCommand(const LoadCommand& cmd, RangeSink* sink) {
  switch (cmd.cmd) {
    case LC_SEGMENT_64:
      ParseSegment<segment_command_64, section_64>(cmd, sink);
      break;
    case LC_SEGMENT:
      ParseSegment<segment_command, section>(cmd, sink);
      break;
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY:
      ParseDyldInfo(cmd, sink);
      break;
    case LC_SYMTAB:
      ParseSymbolTable(cmd, sink);
      break;
    case LC_DYSYMTAB:
      ParseDynamicSymbolTable(cmd, sink);
      break;
    case LC_CODE_SIGNATURE:
      ParseLinkeditCommand("Code Signature", cmd, sink);
      break;
    case LC_SEGMENT_SPLIT_INFO:
      ParseLinkeditCommand("Segment Split Info", cmd, sink);
      break;
    case LC_FUNCTION_STARTS:
      ParseLinkeditCommand("Function Start Addresses", cmd, sink);
      break;
    case LC_DATA_IN_CODE:
      ParseLinkeditCommand("Table of Non-instructions", cmd, sink);
      break;
    case LC_DYLIB_CODE_SIGN_DRS:
      ParseLinkeditCommand("Code Signing DRs", cmd, sink);
      break;
    case LC_LINKER_OPTIMIZATION_HINT:
      ParseLinkeditCommand("Optimization Hints", cmd, sink);
      break;
    case LC_DYLD_CHAINED_FIXUPS:
      ParseLinkeditCommand("Chained Fixups", cmd, sink);
      break;
    case LC_DYLD_EXPORTS_TRIE:
      ParseLinkeditCommand("Exports Trie", cmd, sink);
      break;
  }
}

void ParseLoadCommands(RangeSink* sink) {
  ForEachLoadCommand(
      sink->input_file().data(), sink,
      [sink](const LoadCommand& cmd) { ParseLoadCommand(cmd, sink); });
}

template <class NList>
void ParseSymbolsFromSymbolTable(const LoadCommand& cmd, SymbolTable* table,
                                 RangeSink* sink) {
  auto symtab_cmd = GetStructPointer<symtab_command>(cmd.command_data);

  string_view symtab = StrictSubstr(cmd.file_data, symtab_cmd->symoff,
                                    symtab_cmd->nsyms * sizeof(NList));
  string_view strtab =
      StrictSubstr(cmd.file_data, symtab_cmd->stroff, symtab_cmd->strsize);

  uint32_t nsyms = symtab_cmd->nsyms;
  for (uint32_t i = 0; i < nsyms; i++) {
    auto sym = GetStructPointerAndAdvance<NList>(&symtab);
    string_view sym_range(reinterpret_cast<const char*>(sym), sizeof(NList));

    if (sym->n_type & N_STAB || sym->n_value == 0) {
      continue;
    }

    string_view name_region = StrictSubstr(strtab, sym->n_un.n_strx);
    string_view name = ReadNullTerminated(&name_region);

    if (sink->data_source() >= DataSource::kSymbols) {
      sink->AddVMRange("macho_symbols", sym->n_value, RangeSink::kUnknownSize,
                       ItaniumDemangle(name, sink->data_source()));
    }

    if (table) {
      table->insert(std::make_pair(
          name, std::make_pair(sym->n_value, RangeSink::kUnknownSize)));
    }

    // Capture the trailing NULL.
    name = string_view(name.data(), name.size() + 1);
    sink->AddFileRangeForVMAddr("macho_symtab_name", sym->n_value, name);
    sink->AddFileRangeForVMAddr("macho_symtab_sym", sym->n_value, sym_range);
  }
}

void ParseSymbols(string_view file_data, SymbolTable* symtab, RangeSink* sink) {
  ForEachLoadCommand(
      file_data, sink,
      [symtab, sink](const LoadCommand& cmd) {
        switch (cmd.cmd) {
          case LC_SYMTAB:
            if (cmd.is64bit) {
              ParseSymbolsFromSymbolTable<nlist_64>(cmd, symtab, sink);
            } else {
              ParseSymbolsFromSymbolTable<struct nlist>(cmd, symtab, sink);
            }
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
      [sink](const LoadCommand& cmd) {
        switch (cmd.cmd) {
          case LC_SEGMENT_64:
            AddSegmentAsFallback<segment_command_64, section_64>(
                cmd.command_data, cmd.file_data, sink);
            break;
          case LC_SEGMENT:
            AddSegmentAsFallback<segment_command, section>(cmd.command_data,
                                                           cmd.file_data, sink);
            break;
        }
      });
  sink->AddFileRange("macho_fallback", "[Unmapped]", sink->input_file().data());
}

static void AddMachOArchiveMemberFallback(RangeSink* sink) {
  if (sink->data_source() == DataSource::kArchiveMembers) {
    ForEachLoadCommand(
        sink->input_file().data(), sink,
        [sink](const LoadCommand& cmd) {
          if (!cmd.filename.empty()) {
            sink->AddFileRange("unmapped_armember", cmd.filename, cmd.file_data);
          }
        });
  }
}

template <class Segment, class Section>
void ReadDebugSectionsFromSegment(LoadCommand cmd, dwarf::File *dwarf,
                                  RangeSink *sink) {
  auto segment = GetStructPointerAndAdvance<Segment>(&cmd.command_data);
  string_view segname = ArrayToStr(segment->segname, 16);

  if (segname != "__DWARF") {
    return;
  }

  uint32_t nsects = segment->nsects;
  for (uint32_t j = 0; j < nsects; j++) {
    auto section = GetStructPointerAndAdvance<Section>(&cmd.command_data);
    string_view sectname = ArrayToStr(section->sectname, 16);

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

    string_view contents =
        StrictSubstr(cmd.file_data, section->offset, filesize);

    if (sectname.find("__debug_") == 0) {
      sectname.remove_prefix(string_view("__debug_").size());
      dwarf->SetFieldByName(sectname, contents);
    } else if (sectname.find("__zdebug_") == 0) {
      sectname.remove_prefix(string_view("__zdebug_").size());
      string_view *member = dwarf->GetFieldByName(sectname);
      if (!member || ReadBytes(4, &contents) != "ZLIB") {
        continue;
      }
      auto uncompressed_size = ReadBigEndian<uint64_t>(&contents);
      *member = sink->ZlibDecompress(contents, uncompressed_size);
    }
  }
}

static void ReadDebugSectionsFromMachO(const InputFile &file,
                                       dwarf::File *dwarf, RangeSink *sink) {
  dwarf->file = &file;
  dwarf->open = &ReadDebugSectionsFromMachO;
  ForEachLoadCommand(
      file.data(), nullptr, [dwarf, sink](const LoadCommand &cmd) {
        switch (cmd.cmd) {
        case LC_SEGMENT_64:
          ReadDebugSectionsFromSegment<segment_command_64, section_64>(
              cmd, dwarf, sink);
          break;
        case LC_SEGMENT:
          ReadDebugSectionsFromSegment<segment_command, section>(cmd, dwarf,
                                                                 sink);
          break;
        }
      });
}

class MachOObjectFile : public ObjectFile {
 public:
  MachOObjectFile(std::unique_ptr<InputFile> file_data)
      : ObjectFile(std::move(file_data)) {}

  std::string GetBuildId() const override {
    std::string id;

    ForEachLoadCommand(file_data().data(), nullptr, [&id](LoadCommand cmd) {
      if (cmd.cmd == LC_UUID) {
        auto uuid_cmd =
            GetStructPointerAndAdvance<uuid_command>(&cmd.command_data);
        if (!cmd.command_data.empty()) {
          THROWF("Unexpected excess uuid data: $0", cmd.command_data.size());
        }
        id.resize(sizeof(uuid_cmd->uuid));
        memcpy(&id[0], &uuid_cmd->uuid[0], sizeof(uuid_cmd->uuid));
      }
    });

    return id;
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
          ParseSymbols(debug_file().file_data().data(), nullptr, sink);
          break;
        case DataSource::kCompileUnits: {
          CheckNotObject("compileunits", sink);
          SymbolTable symtab;
          DualMap symbol_map;
          NameMunger empty_munger;
          RangeSink symbol_sink(&debug_file().file_data(), sink->options(),
                                DataSource::kRawSymbols,
                                &sinks[0]->MapAtIndex(0), nullptr);
          symbol_sink.AddOutput(&symbol_map, &empty_munger);
          ParseSymbols(debug_file().file_data().data(), &symtab, &symbol_sink);
          dwarf::File dwarf;
          ReadDebugSectionsFromMachO(debug_file().file_data(), &dwarf, sink);
          ReadDWARFCompileUnits(dwarf, symbol_map, sink);
          ParseSymbols(sink->input_file().data(), nullptr, sink);
          break;
        }
        case DataSource::kInlines: {
          CheckNotObject("inlines", sink);
          dwarf::File dwarf;
          ReadDebugSectionsFromMachO(debug_file().file_data(), &dwarf, sink);
          ReadDWARFInlines(dwarf, sink, true);
          break;
        }
        case DataSource::kArchs: {
          ProcessArchitectures(sink);
          break;
        }
        case DataSource::kArchiveMembers:
          ParseLoadCommands(sink);
          AddMachOArchiveMemberFallback(sink);
          break;
        default:
          THROW("Mach-O doesn't support this data source");
      }
      AddMachOFallback(sink);
    }
  }

  void ProcessArchitectures(RangeSink* sink) const {
    uint32_t magic = ReadMagic(file_data().data());

    if (magic == FAT_CIGAM) {
      string_view header_data = file_data().data();
      auto header = GetStructPointerAndAdvance<fat_header>(&header_data);
      uint32_t nfat_arch = ByteSwap(header->nfat_arch);

      for (uint32_t i = 0; i < nfat_arch; i++) {
        auto arch = GetStructPointerAndAdvance<fat_arch>(&header_data);
        uint32_t cputype = ByteSwap(arch->cputype);
        uint32_t cpusubtype = ByteSwap(arch->cpusubtype);
        uint32_t offset = ByteSwap(arch->offset);
        uint32_t size = ByteSwap(arch->size);

        std::string arch_name = CpuTypeToString(cputype, cpusubtype);
        string_view slice_data = StrictSubstr(file_data().data(), offset, size);

        sink->AddFileRange("archs", arch_name, slice_data);
      }
    } else {
      auto header = GetStructPointer<mach_header>(file_data().data());
      std::string arch_name = CpuTypeToString(header->cputype, header->cpusubtype);

      sink->AddFileRange("archs", arch_name, file_data().data());
    }
  }

  bool GetDisassemblyInfo(std::string_view /*symbol*/,
                          DataSource /*symbol_source*/,
                          DisassemblyInfo* /*info*/) const override {
    WARN("Mach-O files do not support disassembly yet");
    return false;
  }
};

}  // namespace macho

std::unique_ptr<ObjectFile> TryOpenMachOFile(std::unique_ptr<InputFile> &file) {
  uint32_t magic = macho::ReadMagic(file->data());

  ArFile ar(file->data());
  // We only support little-endian host and little endian binaries (see
  // ParseMachOHeader() for more rationale).  Fat headers are always on disk as
  // big-endian.
  if (magic == MH_MAGIC || magic == MH_MAGIC_64 || magic == FAT_MAGIC || magic == FAT_CIGAM) {
    return std::unique_ptr<ObjectFile>(
        new macho::MachOObjectFile(std::move(file)));
  } else if (ar.IsOpen()) {
    ArFile::MemberFile member;
    ArFile::MemberReader reader(ar);
    /* if the first archive member is Darwin handle it as macho */
    if (reader.ReadMember(&member) && member.format == ArFile::MemberFile::Darwin) {
      return std::unique_ptr<ObjectFile>(new macho::MachOObjectFile(std::move(file)));
    } else {
      return nullptr;
    }
  }

  return nullptr;
}

}  // namespace bloaty
