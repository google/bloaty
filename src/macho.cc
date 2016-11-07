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

static bool ParseMachOSegments(RangeSink* sink) {
  // Load command 2
  //       cmd LC_SEGMENT_64
  //   cmdsize 632
  //   segname __DATA
  //    vmaddr 0x000000010003a000
  //    vmsize 0x0000000000004000
  //   fileoff 237568
  //  filesize 16384
  //   maxprot 0x00000007
  //  initprot 0x00000003
  //    nsects 7
  //     flags 0x0
  //
  std::string cmd = std::string("otool -l ") + sink->input_file().filename();

  RE2 key_decimal_pattern(R"((\w+) ([1-9][0-9]*))");
  RE2 key_hex_pattern(R"((\w+) 0x([0-9a-f]+))");
  RE2 key_text_pattern(R"( (\w+) (\w+))");
  std::string key;
  std::string text_val;
  std::string segname;
  uintptr_t val;
  uintptr_t vmaddr = 0;
  uintptr_t vmsize = 0;
  uintptr_t fileoff = 0;
  uintptr_t filesize = 0;

  // The entry point appears to be relative to the vmaddr of the first __TEXT
  // segment.  Should verify that this is the precise rule.
  uintptr_t first_text_vmaddr = 0;
  bool in_text_section = false;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {

    if (RE2::PartialMatch(line, key_decimal_pattern, &key, &val) ||
        RE2::PartialMatch(line, key_hex_pattern, &key, RE2::Hex(&val))) {
      if (key == "vmaddr") {
        vmaddr = val;
        if (first_text_vmaddr == 0 && in_text_section) {
          first_text_vmaddr = vmaddr;
        }
      } else if (key == "vmsize") {
        vmsize = val;
      } else if (key == "fileoff") {
        fileoff = val;
      } else if (key == "filesize") {
        filesize = val;
        sink->AddRange(segname, vmaddr, vmsize, fileoff, filesize);
      } else if (key == "entryoff") {
        /*
        Object* entry = sink->FindObjectByAddr(first_text_vmaddr + val);
        if (entry) {
          sink->SetEntryPoint(entry);
        }
        */
      }
    } else if (RE2::PartialMatch(line, key_text_pattern, &key, &text_val)) {
      if (key == "segname") {
        in_text_section = (text_val == "__TEXT");
        segname = text_val;
        vmaddr = 0;
        vmsize = 0;
        fileoff = 0;
        filesize = 0;
      }
    }
  }

  return true;
}

static bool ParseMachOSections(RangeSink* sink) {
  // Section
  //   sectname __text
  //    segname __TEXT
  //       addr 0x0000000100000ac0
  //       size 0x0000000000030b10
  //     offset 2752
  //      align 2^4 (16)
  //     reloff 0
  //     nreloc 0
  //      flags 0x80000400
  //  reserved1 0
  //  reserved2 0

  std::string cmd = std::string("otool -l ") + sink->input_file().filename();

  RE2 key_decimal_pattern(R"((\w+) ([1-9][0-9]*))");
  RE2 key_hex_pattern(R"((\w+) 0x([0-9a-f]+))");
  RE2 key_text_pattern(R"( (\w+) (\w+))");
  std::string key;
  std::string text_val;
  std::string sectname;
  std::string segname;
  uintptr_t val;
  uintptr_t addr = 0;
  uintptr_t size = 0;
  uintptr_t offset = 0;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {

    if (RE2::PartialMatch(line, key_decimal_pattern, &key, &val) ||
        RE2::PartialMatch(line, key_hex_pattern, &key, RE2::Hex(&val))) {
      if (key == "addr") {
        addr = val;
      } else if (key == "size") {
        size = val;
      } else if (key == "offset") {
        offset = val;
      } else if (key == "size") {
        size = val;
      } else if (key == "flags") {
        size_t filesize = size;
        if (val & 0x1) {
          filesize = 0;
        }

        if (segname.empty() || sectname.empty()) {
          continue;
        }

        std::string label = segname + "," + sectname;
        sink->AddRange(label, addr, size, offset, filesize);

        sectname.clear();
        segname.clear();
        addr = 0;
        size = 0;
        offset = 0;
      }
    } else if (RE2::PartialMatch(line, key_text_pattern, &key, &text_val)) {
      if (key == "sectname") {
        sectname = text_val;
      } else if (key == "segname") {
        segname = text_val;
      }
    }
  }

  return true;
}

class MachOFileHandler : public FileHandler {
  bool ProcessBaseMap(RangeSink* sink) override {
    return ParseMachOSegments(sink);
  }

  bool ProcessFile(const std::vector<RangeSink*>& sinks) override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
          CHECK_RETURN(ParseMachOSegments(sink));
          break;
        case DataSource::kSections:
          CHECK_RETURN(ParseMachOSections(sink));
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
  std::string cmd = "file " + file.filename();
  std::string cmd_tonull = cmd + " > /dev/null 2> /dev/null";
  if (system(cmd_tonull.c_str()) < 0) {
    return nullptr;
  }

  for (auto& line : ReadLinesFromPipe(cmd)) {
    if (line.find("Mach-O") != std::string::npos) {
      return std::unique_ptr<FileHandler>(new MachOFileHandler);
    }
  }

  return nullptr;
}

}  // namespace bloaty
