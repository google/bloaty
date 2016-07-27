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

namespace bloaty {

bool StartsWith(const std::string& haystack, const std::string& needle) {
  return !haystack.compare(0, needle.length(), needle);
}

static void ParseMachOSymbols(const std::string& filename, VMRangeSink* sink) {
  std::string cmd = std::string("symbols -noSources -noDemangling ") + filename;

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
}

#if 0

  // file info from symbols
  // [20 spaces]0x00000001000009e0 (    0x21) tests/test_def.c:471
  RE2 pattern2(R"(^\s{20}0x([0-9a-f]+) \(\s*0x([0-9a-f]+)\) (.+):(\d+))");

static void ParseMachODisassembly(const std::string& filename,
                                  ProgramDataSink* program) {
  std::string cmd = std::string("otool -tV ") + filename;

  // ReadLinesFromPipe(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > const&):
  RE2 func_pattern(R"(^([a-zA-Z_].*):)");

  // 0000000100028049        callq   re2::Regexp::Incref() ## re2::Regexp::Incref()
  // Tail call:
  // 0000000100028064        jmp     re2::Regexp::Plus(re2::Regexp*, re2::Regexp::ParseFlags) ## re2::Regexp::Plus(re2::Regexp*, re2::Regexp::ParseFlags)
  RE2 call_pattern(R"((?:callq|jmp)\s+([a-zA-Z_][^#\n]*)(?: #)?)");

  // 000000010000876e        leaq    0x31e9b(%rip), %rax
  // 0000000100008775        movq    %rax, (%rbx)
  RE2 leaq_pattern(R"(leaq\s+(-?0x[0-9a-f]+)\(%rip)");
  RE2 leaq_sym_pattern(R"(leaq\s+(.+)\(%rip\))");
  RE2 addr_pattern(R"(^([0-9a-f]+))");

  Object* current_function = NULL;
  std::string current_function_name;
  long lea_offset = 0;
  std::string name;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    size_t addr;
    if (lea_offset) {
      if (RE2::PartialMatch(line, addr_pattern, RE2::Hex(&addr))) {
        Object* dest = program->FindObjectByAddr((long)addr + lea_offset);
        if (current_function && dest) {
          program->AddRef(current_function, dest);
        }
        lea_offset = 0;
      }
    }

    if (RE2::PartialMatch(line, leaq_pattern, RE2::Hex(&lea_offset))) {
      // Next line will find the actual address.
      /*
      if (current_function) {
        std::cout << "Found lea in function; " << current_function->name << "\n";
      } else {
        std::cout << "Found lea but no current function!  Name: " << current_function_name;
      }
      */
    } else if (RE2::PartialMatch(line, func_pattern, &current_function_name)) {
      current_function = program->FindObjectByName(current_function_name);

      if (!current_function && current_function_name[0] == '_') {
        current_function_name = current_function_name.substr(1);
      }

      current_function = program->FindObjectByName(current_function_name);

      if (!current_function) {
        std::cerr << "Couldn't find function for name: " << current_function_name << "\n";
      }
    } else if (RE2::PartialMatch(line, call_pattern, &name) ||
               RE2::PartialMatch(line, leaq_sym_pattern, &name)) {
      while (name[name.size() - 1] == ' ') {
        name.resize(name.size() - 1);
      }
      Object* target_function = program->FindObjectByName(name);
      if (!target_function && name[0] == '_') {
        name = name.substr(1);
        target_function = program->FindObjectByName(name);
      }

      if (!current_function) {
        std::cerr << line;
        std::cerr << "Whoops!  Found an edge but no current function.  Name: " << name << "\n";
      } else if (!target_function) {
        std::cerr << line;
        std::cerr << "Whoops!  Couldn't find a function entry for: '" << name << "'\n";
      }

      current_function->refs.insert(target_function);

      if (current_function->file != NULL && target_function->file != NULL) {
        current_function->file->refs.insert(target_function->file);
      }
    }
  }
}
#endif

static void ParseMachOSegments(const std::string& filename,
                               VMFileRangeSink* sink) {
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
  std::string cmd = std::string("otool -l ") + filename;

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
}

static void ParseMachOSections(const std::string& filename,
                               VMFileRangeSink* sink) {
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

  std::string cmd = std::string("otool -l ") + filename;

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
}

void RegisterMachODataSources(std::vector<DataSource>* sources) {
  sources->push_back(VMFileRangeDataSource("segments", ParseMachOSegments));
  sources->push_back(VMFileRangeDataSource("sections", ParseMachOSections));
  sources->push_back(VMRangeDataSource("symbols", ParseMachOSymbols));
}

}  // namespace bloaty
