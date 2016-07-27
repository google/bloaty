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

#include <string>
#include <iostream>
#include "re2/re2.h"
#include "bloaty.h"

#include <assert.h>

namespace bloaty {

namespace {

// There are several programs that offer useful information about
// binaries:
//
// - objdump: display object file headers and contents (including disassembly)
// - readelf: more ELF-specific objdump (no disassembly though)
// - nm: display symbols
// - size: display binary size

static size_t AlignUpTo(size_t offset, size_t granularity) {
  return offset;
  // Granularity must be a power of two.
  return (offset + granularity - 1) & ~(granularity - 1);
}

static void ReadELFSymbols(
    const std::string& filename, VMRangeSink* sink,
    std::unordered_map<uintptr_t, std::string>* zero_size_symbols) {
  std::string cmd = std::string("readelf -s --wide ") + filename;
  // Symbol table '.symtab' contains 879 entries:
  //    Num:    Value          Size Type    Bind   Vis      Ndx Name
  //      0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND 
  //      1: 0000000000400238     0 SECTION LOCAL  DEFAULT    1 
  //      2: 0000000000400254     0 SECTION LOCAL  DEFAULT    2 
  //     35: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS dfa.cc
  //     36: 0000000000403420   430 FUNC    LOCAL  DEFAULT   13 _ZN10LogMessageC2EPKcii.c
  //     37: 0000000000443120   128 OBJECT  LOCAL  DEFAULT   15 _ZZN3re23DFA14FastSearchL
  RE2 pattern(R"(\s*\d+: ([0-9a-f]+)\s+([0-9]+) ([A-Z]+)\s+[A-Z]+\s+[A-Z]+\s+([A-Z0-9]+) (\S+))");

  // Large symbols get printed slightly differently (notice the 0x):
  //
  //   5898: 00000000001a4d80 0x801058 OBJECT  GLOBAL DEFAULT   33 _ZN8tcmalloc6Static9pageheap_E
  RE2 pattern_large(R"(\s*\d+: ([0-9a-f]+)\s+0x([0-9a-f]+) ([A-Z]+)\s+[A-Z]+\s+[A-Z]+\s+([A-Z0-9]+) (\S+))");

  for (auto& line : ReadLinesFromPipe(cmd)) {
    std::string name;
    std::string type;
    std::string ndx;
    size_t addr, size;

    if (RE2::FullMatch(line, pattern, RE2::Hex(&addr), &size, &type, &ndx,
                       &name) ||
        RE2::FullMatch(line, pattern_large, RE2::Hex(&addr), RE2::Hex(&size),
                       &type, &ndx, &name)) {
      // We can't skip symbols of size 0 because some symbols appear to
      // have size 0 in the symbol table even though their true size appears to
      // be clearly greater than zero.  For example:
      //
      // $ readelf -s --wide
      //  Symbol table '.symtab' contains 22833 entries:
      //    Num:    Value          Size Type    Bind   Vis      Ndx Name
      // [...]
      //     10: 0000000000034450     0 FUNC    LOCAL  DEFAULT   13 __do_global_dtors_aux
      //
      // $ objdump -d -r -M intel
      // 0000000000034450 <__do_global_dtors_aux>:
      //   34450:       80 3d 69 b7 45 00 00    cmp    BYTE PTR [rip+0x45b769],0x0        # 48fbc0 <completed.6687>
      //   34457:       75 72                   jne    344cb <__do_global_dtors_aux+0x7b>
      if (size == 0) {
        (*zero_size_symbols)[addr] = name;
        continue;
      }

      if (type == "NOTYPE" || type == "TLS" || ndx == "UND" || ndx == "ABS") {
        // Skip.
      } else if (name.empty()) {
        // This happens in lots of cases where there is a section like .interp
        // or .dynsym.  We could import these and let the section list refine
        // them, but they would show up as garbage symbols until we figure out
        // how to indicate that they are reachable.
        continue;
      }

      sink->AddVMRangeAllowAlias(addr, AlignUpTo(size, 16), name);
    }
  }
}

static void ReadELFSectionsRefineSymbols(
    const std::string& filename, VMRangeSink* sink,
    std::unordered_map<uintptr_t, std::string>* zero_size_symbols) {
  std::string cmd = std::string("readelf -S --wide ") + filename;
  // We use the section headers to patch up the symbol table a little.
  // There are a few cases where the symbol table shows a zero size for
  // a symbol, but the symbol corresponds to a section which has a useful
  // size in the section headers.  For example:
  //
  // $ readelf -s --wide
  // Symbol table '.dynsym' contains 307 entries:
  //    Num:    Value          Size Type    Bind   Vis      Ndx Name
  //  [...]
  //      4: 00000000004807e0     0 OBJECT  LOCAL  DEFAULT   23 __CTOR_LIST__
  //
  // $ readelf -S
  // Section Headers:
  //  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  //  [...]
  //  [23] .ctors            PROGBITS        00000000004807e0 47f7e0 0004d0 00  WA  0   0  8
  //
  // It would be nice if the linker would put the correct size in the symbol
  // table!

  //  [23] .ctors            PROGBITS        00000000004807e0 47f7e0 0004d0 00  WA  0   0  8
  RE2 pattern(R"(([0-9a-f]+) (?:[0-9a-f]+) ([0-9a-f]+))");

  uintptr_t addr;
  uintptr_t size;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (RE2::PartialMatch(line, pattern, RE2::Hex(&addr), RE2::Hex(&size))) {
      auto it = zero_size_symbols->find(addr);
      if (it != zero_size_symbols->end() && size > 0) {
        sink->AddVMRangeAllowAlias(addr, size, it->second);
        zero_size_symbols->erase(it);
      }
    }
  }
}

static void ReadELFSections(const std::string& filename, VMFileRangeSink* sink) {
  std::string cmd = std::string("readelf -S --wide ") + filename;
  // $ readelf -S
  // Section Headers:
  //  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  //  [...]
  //  [23] .ctors            PROGBITS        00000000004807e0 47f7e0 0004d0 00  WA  0   0  8

  RE2 pattern(R"(([.\w\-]+) +(\w+) +([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+))");

  std::string name;
  std::string type;
  uintptr_t addr;
  uintptr_t off;
  size_t size;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (RE2::PartialMatch(line, pattern, &name, &type, RE2::Hex(&addr),
                          RE2::Hex(&off), RE2::Hex(&size))) {
      if (name == "NULL") {
        continue;
      }

      size_t vmsize;
      if (addr == 0) {
        vmsize = 0;
      } else {
        vmsize = size;
      }

      if (type == "NOBITS") {
        size = 0;
      }

      sink->AddRange(name, addr, vmsize, off, size);
    }
  }
}

static void ReadELFSegments(const std::string& filename, VMFileRangeSink* sink) {
  //   Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  //   LOAD           0x000000 0x0000000000400000 0x0000000000400000 0x19a8a9 0x19a8a9 R E 0x200000
  //
  // We should probably be able to handle any type where filesize > 0, but they
  // are sometimes overlapping and we don't properly handle that yet.
  RE2 load_pattern(
      R"((?:LOAD)\s+0x([0-9a-f]+) 0x([0-9a-f]+) 0x[0-9a-f]+ 0x([0-9a-f]+) 0x([0-9a-f]+) (...))");

  std::string cmd = std::string("readelf -l --wide ") + filename;
  uintptr_t fileoff;
  uintptr_t vmaddr;
  size_t filesize;
  size_t memsize;
  std::string flg;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (RE2::PartialMatch(line, load_pattern, RE2::Hex(&fileoff),
                          RE2::Hex(&vmaddr), RE2::Hex(&filesize),
                          RE2::Hex(&memsize), &flg)) {
      sink->AddRange("LOAD [" + flg + "]", vmaddr, memsize, fileoff, filesize);
    }
  }
}

static void ParseELFDisassembly(const std::string& filename,
                                AddressReferenceSink* sink) {
  std::string cmd = std::string("objdump -d -r -M intel ") + filename;

  //   43f806:       e8 a5 86 fd ff          call   417eb0 <_ZN3re26Regexp6WalkerIiED1Ev>
  //   401b4f:       e8 cc 5c 00 00          call   407820 <_ZN7Program30PrintSymbolsByTransitiveWeightEv>
  // Tail call:
  //   43f80b:       eb ce                   jmp    43f7db <_ZN3re26Regexp8ToStringEv>
  RE2 call_pattern(R"(([0-9a-f]+): [\s0-9a-f]+ (?:call|jmp)\s+([0-9a-f]+) <[\w.]+>)");

  //   43f941:       4c 8d 25 70 44 21 00    lea    r12,[rip+0x214470]        # 653db8 <__frame_dummy_init_array_entry+0x10>
  RE2 ref_pattern(R"(([0-9a-f]+): [\s0-9a-f]+ .* # [0-9a-f]+ <[\w.]+(?:\+0x[0-9a-f]+)?>)");

  // Some references only show up like this. :(
  // It's a guess whether this is really an address.
  //
  // If we get false positives We could be smarter here and try to analyze the
  // assembly after this instruction to determine whether this value is used in
  // a memory reference.
  //   42fdc5:       41 bf e0 f2 51 00       mov    r15d,0x51f2e0
  RE2 current_addr_pattern(R"(^\s*([0-9a-f]+):)");
  RE2 const_pattern(R"(0x([0-9a-f]+))");

  uintptr_t addr, ref_addr;
  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    re2::StringPiece piece(line);
    if (RE2::PartialMatch(line, call_pattern,
                          RE2::Hex(&addr), RE2::Hex(&ref_addr)) ||
        RE2::PartialMatch(line, ref_pattern,
                          RE2::Hex(&addr), RE2::Hex(&ref_addr))) {
      sink->Add(addr, ref_addr);
    } else if (RE2::FindAndConsume(&piece, current_addr_pattern, RE2::Hex(&addr))) {
      while (RE2::FindAndConsume(&piece, const_pattern, RE2::Hex(&ref_addr))) {
        sink->Add(addr, ref_addr);
      }
    }
  }
}

static std::string ReadELFBuildId(const std::string& filename) {
  // Build ID: 669eba985387d24f7d959e0f0e49d9209bcf975b
  RE2 pattern(R"(Build ID: (\w+))");
  std::string cmd = std::string("readelf -n ") + filename;
  std::string build_id;

  for (auto& line : ReadLinesFromPipe(cmd)) {
    if (RE2::PartialMatch(line, pattern, &build_id)) {
      return build_id;
    }
  }

  std::cerr << "Didn't find build id.\n";
  exit(1);
}

static uintptr_t ReadELFEntryPoint(const std::string& filename) {
  std::string cmd = std::string("readelf -l --wide ") + filename;
  RE2 entry_pattern(R"(^Entry point 0x([0-9a-f]+))");
  uintptr_t entry_addr;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (RE2::PartialMatch(line, entry_pattern, RE2::Hex(&entry_addr))) {
      return entry_addr;
    }
  }

  std::cerr << "Didn't find entry point id.\n";
  exit(1);
}

}  // namespace

static void ReadELFSymbols(const std::string& filename, VMRangeSink* sink) {
  std::unordered_map<uintptr_t, std::string> zero_size_symbols;

  ReadELFSymbols(filename, sink, &zero_size_symbols);
  ReadELFSectionsRefineSymbols(filename, sink, &zero_size_symbols);
}

// ELF files put debug info directly into the binary, so we call the DWARF
// reader directly on them.

static void ReadELFSourceFiles(const std::string& filename,
                               VMRangeSink* sink) {
  ReadDWARFSourceFiles(filename, sink);
}

static void ReadELFLineInfo(const std::string& filename, VMRangeSink* sink) {
  ReadDWARFLineInfo(filename, sink, true);
}

static void ReadELFLineInfoFile(const std::string& filename,
                                VMRangeSink* sink) {
  ReadDWARFLineInfo(filename, sink, false);
}

void RegisterELFDataSources(std::vector<DataSource>* sources) {
  sources->push_back(VMFileRangeDataSource("segments", ReadELFSegments));
  sources->push_back(VMFileRangeDataSource("sections", ReadELFSections));
  sources->push_back(VMRangeDataSource("symbols", ReadELFSymbols));
  sources->push_back(VMRangeDataSource("sourcefiles", ReadELFSourceFiles));
  sources->push_back(VMRangeDataSource("lineinfo", ReadELFLineInfo));
  sources->push_back(VMRangeDataSource("lineinfo:file", ReadELFLineInfoFile));
}

std::string ReadBuildId(const std::string& filename) {
  return ReadELFBuildId(filename);
}

uintptr_t ReadEntryPoint(const std::string& filename) {
  return ReadELFEntryPoint(filename);
}

}  // namespace bloaty
