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

#ifdef __APPLE__
  #include <mach-o/loader.h>
#endif

// There are several programs that offer useful information about
// binaries:
//
// - otool: display object file headers and contents (including disassembly)
// - nm: display symbols
// - size: display binary size
// - symbols: display symbols, drawing on more sources
// - pagestuff: display page-oriented information
// - dsymutil: create .dSYM bundle with DWARF information inside
// - dwarfdump: dump DWARF debugging information from .o or .dSYM

#define CHECK_RETURN(call) if (!(call)) { return false; }

namespace bloaty {

#ifndef __APPLE__
std::unique_ptr<FileHandler> TryOpenMachOFile(const InputFile& file) {
  return nullptr;
}
#else // __APPLE__

bool StartsWith(const std::string& haystack, const std::string& needle) {
  return !haystack.compare(0, needle.length(), needle);
}

static bool ParseMachOSymbols(RangeSink* sink) {
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
      if (StartsWith(name, "DYLD-STUB")) {
        continue;
      }

      sink->AddVMRange(addr, size, name);
    }
  }

  return true;
}

static bool ParseMachOLoadCommands(RangeSink* sink, DataSource data_source) {
  struct mach_header_64 *header = (struct mach_header_64 *) sink->input_file().data().data();

  // advance past mach header to get to first load command
  struct load_command *command = (struct load_command *) (header + 1);

  for (uint32_t i = 0;
      i < header->ncmds;
      i++, command = (struct load_command *) ((char *) command + command->cmdsize))
  {
    if (command->cmd == LC_SEGMENT_64) {
      struct segment_command_64 *segment_command = (struct segment_command_64 *) command;

      // __PAGEZERO is a special blank segment usually used to trap NULL-dereferences
      // (since all protection bits are 0 it cannot be read from, written to, or executed)
      if (std::string(segment_command->segname) == SEG_PAGEZERO) {
        // theoretically __PAGEZERO *should* count towards VM size
        // however on x86_64 __PAGEZERO takes up all lower 4 GiB
        // hence we skip it to avoid skewing the results (such as showing 99.99% or 100% for __PAGEZERO)
        continue;
      }
      
      if (data_source == DataSource::kSegments) { // if segments are all we need this is enough
        // side note: segname (& sectname) may NOT be NULL-terminated,
        //   i.e. can use up all 16 chars, e.g. '__gcc_except_tab' (no '\0'!)
        //   hence specifying size when constructing std::string
        sink->AddRange(std::string(segment_command->segname, 16),
                       segment_command->vmaddr,
                       segment_command->vmsize,
                       segment_command->fileoff,
                       segment_command->filesize);
      } else if (data_source == DataSource::kSections) { // otherwise load sections
        // advance past segment command header
        struct section_64 *section = (struct section_64 *) (segment_command + 1);
        for (uint32_t j = 0; j < segment_command->nsects; j++, section++) {
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

          std::string label = std::string(section->segname, 16) + "," + std::string(section->sectname, 16);
          sink->AddRange(label, section->addr, section->size, section->offset, filesize);
        }
      } else {
        return false;
      }
    }
  }

  return true;
}

class MachOFileHandler : public FileHandler {
  bool ProcessBaseMap(RangeSink* sink) override {
    return ParseMachOLoadCommands(sink, DataSource::kSegments);
  }

  bool ProcessFile(const std::vector<RangeSink*>& sinks) override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
        case DataSource::kSections:
          CHECK_RETURN(ParseMachOLoadCommands(sink, sink->data_source()));
          break;
        case DataSource::kSymbols:
          CHECK_RETURN(ParseMachOSymbols(sink));
          break;
        case DataSource::kArchiveMembers:
        case DataSource::kCompileUnits:
        case DataSource::kInlines:
        default:
          fprintf(stderr, "Mach-O doesn't support this data source.\n");
          return false;
      }
    }

    return true;
  }
};

std::unique_ptr<FileHandler> TryOpenMachOFile(const InputFile& file) {
  struct mach_header_64 *header = (struct mach_header_64 *) file.data().data();

  // 32 bit and fat binaries are effectively deprecated, don't bother
  // note check for MH_CIGAM_64 is unneccessary as long as this tool is executed on
  //   the same architecture as the target binary
  // too allow checking for target binaries complied for a different architecture (e.g. checking ARM
  //   binaries on Intel), corresponding changes also need to be made to swap byte-order when
  //   reading sizes and offsets from load commands
  if (header->magic == MH_MAGIC_64) { 
    return std::unique_ptr<FileHandler>(new MachOFileHandler);
  }

  return nullptr;
}
#endif //__APPLE__

}  // namespace bloaty

// vim: expandtab:ts=2:sw=2

