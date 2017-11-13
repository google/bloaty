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
#include "bloaty.h"
#include "re2/re2.h"

#include <cassert>

#include "third_party/darwin_xnu_macho/mach-o/loader.h"
#include "third_party/darwin_xnu_macho/mach-o/fat.h"
#include "third_party/darwin_xnu_macho/mach-o/nlist.h"
#include "third_party/darwin_xnu_macho/mach-o/reloc.h"

ABSL_ATTRIBUTE_NORETURN
static void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)
#define WARN(x) fprintf(stderr, "bloaty: %s\n", x);

// segname (& sectname) may NOT be NULL-terminated,
//   i.e. can use up all 16 chars, e.g. '__gcc_except_tab' (no '\0'!)
//   hence specifying size when constructing std::string
static std::string str_from_char(const char *s, size_t maxlen) {
  return std::string(s, strnlen(s, maxlen));
}

namespace bloaty {

uint32_t MaybeByteSwap(uint32_t val, bool is_native_endian) {
  if (!is_native_endian) {
    val = ByteSwap(val);
  }
  return val;
}

static void ParseMachOLoadCommands(RangeSink* sink) {
  const uint8_t *raw_data = (const uint8_t *) sink->input_file().data().data();
  uint32_t magic = *(uint32_t *) raw_data;

  uint32_t offset = 0; // file offset for 64 bit in fat binaries (0 for 64 bit native)

  if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
    bool is_native_endian = (magic == FAT_MAGIC);
    struct fat_header *fat_header = (struct fat_header *) raw_data;
    struct fat_arch *arch = (struct fat_arch *) (fat_header + 1);

    for (uint32_t i = 0; i < MaybeByteSwap(fat_header->nfat_arch, is_native_endian); i++, arch++) {
      if (MaybeByteSwap(arch->cputype, is_native_endian) & CPU_ARCH_ABI64) {
        offset = MaybeByteSwap(arch->offset, is_native_endian);
      } else {
        sink->AddRange("[Non-64bit arch in fat binary]", 0, 0,
                       MaybeByteSwap(arch->offset, is_native_endian),
                       MaybeByteSwap(arch->size, is_native_endian));
      }
    }

    assert(offset);
  }

  struct mach_header_64 *header = (struct mach_header_64 *) (raw_data + offset);

  // advance past mach header to get to first load command
  struct load_command *command = (struct load_command *) (header + 1);
  struct segment_command_64 *__linkedit_segment = NULL;

  for (uint32_t i = 0;
      i < header->ncmds;
      i++, command = (struct load_command *) ((char *) command + command->cmdsize))
  {
    if (command->cmd == LC_SEGMENT_64) {
      struct segment_command_64 *segment = (struct segment_command_64 *) command;

      // __PAGEZERO is a special blank segment usually used to trap NULL-dereferences
      // (since all protection bits are 0 it cannot be read from, written to, or executed)
      if (str_from_char(segment->segname, 16) == SEG_PAGEZERO) {
        // theoretically __PAGEZERO *should* count towards VM size
        // however on x86_64 __PAGEZERO takes up all lower 4 GiB
        // hence we skip it to avoid skewing the results (such as showing 99.99% or 100% for __PAGEZERO)
        continue;
      }

      if (str_from_char(segment->segname, 16) == SEG_LINKEDIT) {
        __linkedit_segment = segment;
      }

      if (sink->data_source() == DataSource::kSegments) { // if segments are all we need this is enough
        sink->AddRange(str_from_char(segment->segname, 16),
                       segment->vmaddr,
                       segment->vmsize,
                       segment->fileoff,
                       segment->filesize);
      } else if (sink->data_source() == DataSource::kSections) { // otherwise load sections
        // advance past segment command header
        struct section_64 *section = (struct section_64 *) (segment + 1);
        for (uint32_t j = 0; j < segment->nsects; j++, section++) {
          // filesize equals vmsize unless the section is zerofill
          // note that the flags check (& 0x01) in the original implementation is incorrect
          // the lowest byte is interpreted similar to a enum rather than a bitmask
          // e.g. __mod_init_func (S_MOD_INIT_FUNC_POINTERS, 0x09) has lowest bit set but is not a zerofill
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

          std::string label = str_from_char(section->segname, 16) + "," + str_from_char(section->sectname, 16);
          sink->AddRange(label, section->addr, section->size, section->offset, filesize);
        }
      } else {
        BLOATY_UNREACHABLE();
      }
    }

    // additional __LINKEDIT info
    // not "sections" per se, but nevertheless useful (and more complex parsing logic)
    if (sink->data_source() == DataSource::kSections) {
      if (command->cmd == LC_DYLD_INFO || command->cmd == LC_DYLD_INFO_ONLY) {
        // technically load commands can come in any order
        // however unless deliberately hand crafted, LC_SEGMENT_64 comes before any others
        // hence __LINKEDIT should've been identified
        // while technically dyld info can point to any segment, "normal" binaries have it points into __LINKEDIT
        assert(__linkedit_segment);

        struct dyld_info_command *dyld_info = (struct dyld_info_command *) command;

        #define ADD_DYLD_INFO_RANGE(section, desc) \
        sink->AddRange(SEG_LINKEDIT "," desc, \
                       __linkedit_segment->vmaddr + dyld_info-> section##_off - __linkedit_segment->fileoff, \
                       dyld_info-> section##_size, \
                       dyld_info-> section##_off, \
                       dyld_info-> section##_size)

        ADD_DYLD_INFO_RANGE(rebase, "Rebase Info");
        ADD_DYLD_INFO_RANGE(bind, "Binding Info");
        ADD_DYLD_INFO_RANGE(weak_bind, "Weak Binding Info");
        ADD_DYLD_INFO_RANGE(lazy_bind, "Lazy Binding Info");
        ADD_DYLD_INFO_RANGE(export, "Export Info");
      }
    }

    if (command->cmd == LC_SYMTAB) {
      assert(__linkedit_segment);

      struct symtab_command *symtab  = (struct symtab_command *) command;

      sink->AddRange(SEG_LINKEDIT ",Symbol Table",
                     __linkedit_segment->vmaddr + symtab->symoff - __linkedit_segment->fileoff,
                     symtab->nsyms * sizeof(struct nlist_64),
                     symtab->symoff,
                     symtab->nsyms * sizeof(struct nlist_64));

      sink->AddRange(SEG_LINKEDIT ",String Table",
                     __linkedit_segment->vmaddr + symtab->stroff - __linkedit_segment->fileoff,
                     symtab->strsize,
                     symtab->stroff,
                     symtab->strsize);
    }

    if (command->cmd == LC_DYSYMTAB) {
      assert(__linkedit_segment);

      struct dysymtab_command *dysymtab = (struct dysymtab_command *) command;

      #define ADD_DYSYM_RANGE(desc, offset, num, entry_type) \
      sink->AddRange(SEG_LINKEDIT "," desc, \
                     __linkedit_segment->vmaddr + dysymtab-> offset - __linkedit_segment->fileoff, \
                     dysymtab-> num * sizeof(entry_type), \
                     dysymtab-> offset, \
                     dysymtab-> num * sizeof(entry_type))

      ADD_DYSYM_RANGE("Table of Contents", tocoff, ntoc, struct dylib_table_of_contents);
      ADD_DYSYM_RANGE("Module Table", modtaboff, nmodtab, struct dylib_module_64);
      ADD_DYSYM_RANGE("Referenced Symbol Table", extrefsymoff, nextrefsyms, struct dylib_reference);
      ADD_DYSYM_RANGE("Indirect Symbol Table", indirectsymoff, nindirectsyms, uint32_t);
      ADD_DYSYM_RANGE("External Relocation Entries", extreloff, nextrel, struct relocation_info);
      ADD_DYSYM_RANGE("Local Relocation Entries", locreloff, nlocrel, struct relocation_info);
    }

    #define CHECK_LINKEDIT_DATA_COMMAND(lc, desc) \
    if (command->cmd == lc) { \
      assert(__linkedit_segment); \
      struct linkedit_data_command *linkedit_data = (struct linkedit_data_command *) command; \
      sink->AddRange(SEG_LINKEDIT "," desc, \
                     __linkedit_segment->vmaddr + linkedit_data->dataoff - __linkedit_segment->fileoff, \
                     linkedit_data->datasize, \
                     linkedit_data->dataoff, \
                     linkedit_data->datasize); \
    }

    CHECK_LINKEDIT_DATA_COMMAND(LC_CODE_SIGNATURE, "Code Signature");
    CHECK_LINKEDIT_DATA_COMMAND(LC_SEGMENT_SPLIT_INFO, "Segment Split Info");
    CHECK_LINKEDIT_DATA_COMMAND(LC_FUNCTION_STARTS, "Function Start Addresses");
    CHECK_LINKEDIT_DATA_COMMAND(LC_DATA_IN_CODE, "Table of Non-instructions");
    CHECK_LINKEDIT_DATA_COMMAND(LC_DYLIB_CODE_SIGN_DRS, "Code Signing DRs");
    CHECK_LINKEDIT_DATA_COMMAND(LC_LINKER_OPTIMIZATION_HINT, "Optimizatio nHints");
  }
}

static void ParseMachOSymbols(RangeSink* sink) {
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

      sink->AddVMRange(addr, size, name);
    }
  }
}

class MachOObjectFile : public ObjectFile {
 public:
  MachOObjectFile(std::unique_ptr<InputFile> file_data)
      : ObjectFile(std::move(file_data)) {}

  void ProcessBaseMap(RangeSink* sink) override {
    ParseMachOLoadCommands(sink);
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
          ParseMachOLoadCommands(sink);
          break;
        case DataSource::kRawSymbols:
        case DataSource::kShortSymbols:
        case DataSource::kFullSymbols:
          ParseMachOSymbols(sink);
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
  uint32_t magic = *(uint32_t *) file->data().data();

  // 32 bit binaries are effectively deprecated, don't bother
  // note check for MH_CIGAM_64 is unneccessary as long as this tool is executed on
  //   the same architecture as the target binary
  // check for FAT_CIGAM *is* neccessary since fat header is always big-endian and would read FAT_CIGAM on Intel
  // to allow checking for target binaries complied for a different architecture (e.g. checking ARM
  //   binaries on Intel), corresponding changes also need to be made to swap byte-order when
  //   reading sizes and offsets from load commands
  // for fat binaries, reports info on 64 bit binary (file size and offsets relative to only the 64 bit part)
  if (magic == MH_MAGIC_64 || magic == FAT_CIGAM || magic == FAT_MAGIC) {
    return std::unique_ptr<ObjectFile>(new MachOObjectFile(std::move(file)));
  }

  return nullptr;
}

}  // namespace bloaty

// vim: expandtab:ts=2:sw=2

