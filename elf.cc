
#include <string>
#include <iostream>
#include "re2/re2.h"
#include "bloaty.h"

// There are several programs that offer useful information about
// binaries:
//
// - objdump: display object file headers and contents (including disassembly)
// - readelf: more ELF-specific objdump (no disassembly though)
// - nm: display symbols
// - size: display binary size

static void ParseELFSymbols(const std::string& filename,
                            ProgramDataSink* sink) {
  std::string cmd = std::string("readelf -s --wide ") + filename;
  // Symbol table '.symtab' contains 879 entries:
  //    Num:    Value          Size Type    Bind   Vis      Ndx Name
  //      0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND 
  //      1: 0000000000400238     0 SECTION LOCAL  DEFAULT    1 
  //      2: 0000000000400254     0 SECTION LOCAL  DEFAULT    2 
  //     35: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS dfa.cc
  //     36: 0000000000403420   430 FUNC    LOCAL  DEFAULT   13 _ZN10LogMessageC2EPKcii.c
  //     37: 0000000000443120   128 OBJECT  LOCAL  DEFAULT   15 _ZZN3re23DFA14FastSearchL
  RE2 pattern(R"(\s*\d+: ([0-9a-f]+)\s+([0-9]+) ([A-Z]+)\s+[A-Z]+\s+[A-Z]+\s+([A-Z0-9]+) (.*))");

  for (auto& line : ReadLinesFromPipe(cmd)) {
    std::string name;
    std::string type;
    std::string ndx;
    size_t addr, size;

    if (RE2::FullMatch(line, pattern, RE2::Hex(&addr), &size, &type, &ndx, &name)) {
      if (type == "NOTYPE" || ndx == "UND" || ndx == "ABS") {
        // Skip.
      } else if (type == "FUNC" || type == "IFUNC") {
        sink->AddObject(name, addr, size, false);
      } else {
        sink->AddObject(name, addr, size, true);
      }
    }
  }
}

static void ParseELFDisassembly(const std::string& filename,
                                ProgramDataSink* sink) {
  std::string cmd = std::string("objdump -d -r -M intel ") + filename;

  // 0000000000403af0 <_ZN19DominatorCalculator8CompressEj>:
  RE2 func_pattern(R"([0-9a-f]+ <([\w.]+)>:)");

  //   43f806:       e8 a5 86 fd ff          call   417eb0 <_ZN3re26Regexp6WalkerIiED1Ev>
  //   401b4f:       e8 cc 5c 00 00          call   407820 <_ZN7Program30PrintSymbolsByTransitiveWeightEv>
  // Tail call:
  //   43f80b:       eb ce                   jmp    43f7db <_ZN3re26Regexp8ToStringEv>
  RE2 call_pattern(R"((?:call|jmp)\s+[0-9a-f]+ <([\w.]+)>)");

  //   43f941:       4c 8d 25 70 44 21 00    lea    r12,[rip+0x214470]        # 653db8 <__frame_dummy_init_array_entry+0x10>
  RE2 ref_pattern(R"(# [0-9a-f]+ <([\w.]+)(?:\+0x\d+)?>)");

  // Some references only show up like this. :(
  // It's a guess whether this is really an address.
  //
  // If we get false positives We could be smarter here and try to analyze the
  // assembly after this instruction to determine whether this value is used in
  // a memory reference.
  //   42fdc5:       41 bf e0 f2 51 00       mov    r15d,0x51f2e0
  RE2 const_pattern(R"(0x([0-9a-f]+))");

  Object* current_function = NULL;
  std::string current_function_name;
  std::string name;
  uintptr_t addr;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (RE2::FullMatch(line, func_pattern, &current_function_name)) {
      current_function = sink->FindObjectByName(current_function_name);

      if (!current_function && current_function_name[0] == '_') {
        current_function_name = current_function_name.substr(1);
      }

      current_function = sink->FindObjectByName(current_function_name);

      if (!current_function) {
        std::cerr << "Couldn't find function for name: " << current_function_name << "\n";
      }
    } else if (RE2::PartialMatch(line, call_pattern, &name) ||
               RE2::PartialMatch(line, ref_pattern, &name)) {
      Object* target_function = sink->FindObjectByName(name);
      if (!target_function && name[0] == '_') {
        name = name.substr(1);
        target_function = sink->FindObjectByName(name);
      }

      if (!current_function) {
        //std::cerr << line << "\n";
        //std::cerr << "Whoops!  Found an edge but no current function.  Name: " << name << "\n";
      } else if (!target_function) {
        //std::cerr << line << "\n";
        //std::cerr << "Whoops!  Couldn't find a function entry for: '" << name << "'\n";
      } else {
        sink->AddRef(current_function, target_function);
      }
    }

    re2::StringPiece piece(line);
    while (RE2::FindAndConsume(&piece, const_pattern, RE2::Hex(&addr))) {
      Object* to = sink->FindObjectByAddr(addr);
      if (current_function && to) {
        sink->AddRef(current_function, to);
      }
    }
  }
}

static void ParseELFFileMapping(const std::string& filename,
                                ProgramDataSink* sink) {
  //   Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  //   LOAD           0x000000 0x0000000000400000 0x0000000000400000 0x19a8a9 0x19a8a9 R E 0x200000
  //
  // We should probably be able to handle any type where filesize > 0, but they
  // are sometimes overlapping and we don't properly handle that yet.
  RE2 load_pattern(R"((?:LOAD)\s+0x([0-9a-f]+) 0x([0-9a-f]+) 0x[0-9a-f]+ 0x([0-9a-f]+))");

  RE2 entry_pattern(R"(^Entry point 0x([0-9a-f]+))");

  std::string cmd = std::string("readelf -l --wide ") + filename;
  uintptr_t fileoff;
  uintptr_t vmaddr;
  size_t filesize;
  uintptr_t entry_addr = 0;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (RE2::PartialMatch(line, load_pattern, RE2::Hex(&fileoff),
                          RE2::Hex(&vmaddr), RE2::Hex(&filesize))) {
      fprintf(stderr, "Adding mapping %lx, %lx, %lx\n", vmaddr, vmaddr + filesize, fileoff);
      sink->AddFileMapping(vmaddr, fileoff, filesize);
    } else if (RE2::PartialMatch(line, entry_pattern, RE2::Hex(&entry_addr))) {
      // We've captured the entry point.
    }
  }

  Object* entry = sink->FindObjectByAddr(entry_addr);
  sink->SetEntryPoint(entry);
}

void ReadObjectData(const std::string& filename, ProgramDataSink* sink) {
  ParseELFSymbols(filename, &sink);
  ParseELFDisassembly(filename, &sink);
  ParseELFFileMapping(filename, &sink);
}
