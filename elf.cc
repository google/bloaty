
#include <string>
#include <iostream>
#include "re2/re2.h"
#include "bloaty.h"

#include <assert.h>

// There are several programs that offer useful information about
// binaries:
//
// - objdump: display object file headers and contents (including disassembly)
// - readelf: more ELF-specific objdump (no disassembly though)
// - nm: display symbols
// - size: display binary size

void ParseELFSymbols(const std::string& filename,
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

      Object* obj = sink->FindObjectByAddr(addr);
      if (obj && obj->size > 0 && size > 0) {
        if (obj->vmaddr == addr && obj->size == size) {
          sink->AddObjectAlias(obj, name);
          continue;
        } else {
          fprintf(stderr, "Imperfect duplicate: (%s, %lx, %lx) (%s, %lx, %lx)\n",
                  obj->name.c_str(), obj->vmaddr, obj->size, name.c_str(), addr, size);
          exit(1);
        }
      }

      if (type == "NOTYPE" || type == "TLS" || ndx == "UND" || ndx == "ABS") {
        // Skip.
      } else if (name.empty()) {
        // This happens in lots of cases where there is a section like .interp
        // or .dynsym.  We could import these and let the section list refine
        // them, but they would show up as garbage symbols until we figure out
        // how to indicate that they are reachable.
        continue;
      } else if (type == "FUNC" || type == "IFUNC") {
        sink->AddObject(name, addr, size, false);
      } else {
        if (name.empty()) {
          fprintf(stderr, "Line: %s\n", line.c_str());
        }
        sink->AddObject(name, addr, size, true);
      }
    }
  }
}

extern bool verbose;
void ParseELFSections(const std::string& filename,
                             ProgramDataSink* sink) {
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
      Object* obj = sink->FindObjectByAddr(addr);
      if (obj && obj->size == 0) {
        obj->size = size;
      }
    }
  }

}

std::string GetBasename(std::string filename) {
  size_t last = filename.rfind("/");
  return last == std::string::npos ? filename : filename.substr(last + 1);
}

void ParseELFDebugInfo(const std::string& filename, ProgramDataSink* sink) {
  std::string cmd =
      std::string("dwarfdump ") + filename;
  // < 3><0x00000482>        DW_TAG_subprogram
  //                           DW_AT_external              yes(1)
  //                           DW_AT_name                  "assign"
  //                           DW_AT_decl_file             0x00000004 third_party/crosstool/v18/stable/toolchain/x86_64-grtev4-linux-gnu/include/c++/4.9.x-google/bits/char_traits.h
  //                           DW_AT_decl_line             0x00000116
  //                           DW_AT_linkage_name          "_ZNSt11char_traitsIcE6assignEPcmc"
  //                           DW_AT_type                  <0x000025c4>
  //                           DW_AT_declaration           yes(1)

  Object* obj = nullptr;
  File* file = nullptr;

  RE2 linkage_name_pattern(R"x(DW_AT_linkage_name\s*"(.*)")x");
  RE2 decl_file_pattern(R"(DW_AT_decl_file\s*0x[0-9a-f]+ (?:\.\/)?(.*))");

  std::string name;

  for (auto& line : ReadLinesFromPipe(cmd)) {
    if (line.find("DW_TAG_subprogram") != std::string::npos) {
      file = nullptr;
      obj = nullptr;
    } else if (RE2::PartialMatch(line, linkage_name_pattern, &name)) {
      obj = sink->FindObjectByName(name);
    } else if (RE2::PartialMatch(line, decl_file_pattern, &name)) {
      file = sink->GetOrCreateFile(name);
    }

    if (obj && file) {
      obj->file = file;
      file->object_size += obj->size;
      std::cerr << obj->name << ": " << file->name << "\n";
      obj = nullptr;
      file = nullptr;
    }
  }
}

void ParseELFLineInfo(
    const std::string& filename,
    const std::unordered_map<std::string, Rule*>& source_files) {
  std::string cmd =
      std::string("readelf --debug-dump=decodedline --wide ") + filename;

  // Decoded dump of debug contents of section .debug_line:
  //
  // CU: ../sysdeps/x86_64/start.S:
  // File name                            Line number    Starting address
  // start.S                                       63             0x343a0
  //
  // start.S                                       79             0x343a2
  // start.S                                       85             0x343a5
  // start.S                                       88             0x343a6
  // start.S                                       90             0x343a9
  // start.S                                       93             0x343ad
  //
  // net/proto2/compiler/internal/parser.cc:
  // parser.cc                                    385             0x481ba
  RE2 full_name_pattern(R"((?:CU: )?(?:\.\/)?(.*):\s*)");
  RE2 entry_pattern(R"((\S+)\s+\d+\s+(?:0x)?([0-9a-f]+))");

  uintptr_t last_addr = 0;
  uintptr_t addr = 0;
  std::string name;

  // Most recent long name we have seen for each short name.
  std::unordered_map<std::string, std::string> name_map;
  std::unordered_set<std::string> complained_about;

  Rule* last_rule = nullptr;
  std::string last_name;

  for (auto& line : ReadLinesFromPipe(cmd)) {
    if (RE2::FullMatch(line, full_name_pattern, &name)) {
      name_map[GetBasename(name)] = name;
    } else if (RE2::FullMatch(line, entry_pattern, &name, RE2::Hex(&addr))) {
      if (last_addr == 0) {
        // We don't trust a new address until it is in a region that seems like
        // it could plausibly be mapped.  We could use actual load instructions
        // to make this heuristic more robust.
        if (addr > 0x10000) {
          last_addr = addr;
        }
      } else {
        if (addr != 0) {
          if (!(last_rule && name == last_name)) { // Optimization: avoid the double hashtable lookup.
            std::string long_name = name_map[name];
            auto it = source_files.find(long_name);
            if (it == source_files.end()) {
              std::string fallback;
              if (TryGetFallbackFilename(long_name, &fallback)) {
                last_rule = source_files.find(fallback)->second;
              } else {
                if (complained_about.insert(long_name).second) {
                  std::cerr << "Warning: couldn't find source file for: " << long_name << "\n";
                }
                continue;
              }
            } else {
              last_rule = it->second;
            }
          }
          if (!last_rule) {
            std::cerr << "No last rule!\n";
            exit(1);
          }
          size_t size = addr - last_addr;
          if (size < 0x10000) {
            last_rule->size += size;
            if (last_rule->name == "//net/proto2/io/internal:io_lite") {
              std::cerr << "Adding to io_lite: " << std::hex << size << " (" << std::hex << last_addr << ", " << std::hex << addr << ") -> " << last_rule->size << "\n";
            }
          }
        }
        last_addr = addr;
      }
    }
  }
}

// This is a relatively complicated state machine designed to do something that
// might not actually be important.  It's trying to avoid creating spurious
// references by avoiding scanning constants in unreachable code.
class RefAdder {
 public:
  RefAdder(ProgramDataSink* sink) : sink_(sink) {}
  void OnFunction(const std::string& name) {
    for (auto it = scanned_refs_.begin(); it != scanned_refs_.end() ; ) {
      if (it->second) {
        ++it;
      } else {
        scanned_refs_.erase(it++);
      }
    }
    if (!scanned_refs_.empty() && current_function_) {
      fprintf(stderr, "Function (%s): Discarding the following scanned refs.\n", current_function_->name.c_str());
      for (const auto& pair : scanned_refs_) {
        if (pair.second) {
          fprintf(stderr, "  %lx: %s\n", pair.first, pair.second->name.c_str());
        }
      }
    }
    scanned_refs_.clear();
    branch_targets_.clear();
    scanning_ = false;
    // Needed?
    // if (!current_function && current_function_name[0] == '_') {
    //   current_function_name = current_function_name.substr(1);
    // }
    // current_function = sink->FindObjectByName(current_function_name);
    current_function_ = sink_->FindObjectByName(name);
    if (!current_function_) {
      std::cerr << "Couldn't find function for name: " << name << "\n";
    }
  }
  void OnAddrRef(uintptr_t current_addr, uintptr_t dest_addr) {
    Object* to = sink_->FindObjectContainingAddr(dest_addr);
    if (to) {
      AddOrStore(current_addr, to);
    }
  }
  void OnNameRef(uintptr_t current_addr, const std::string& dest_name) {
    Object* to = sink_->FindObjectByName(dest_name);
    if (to) {
      AddOrStore(current_addr, to);
    }
  }
  void OnUnconditionalBranch(uintptr_t current_addr, uintptr_t dest_addr) {
    if (verbose) {
      fprintf(stderr, "Unconditional branch at: %lx -> %lx\n", current_addr, dest_addr);
    }
    if (dest_addr) {
      AddBranchTarget(current_addr, dest_addr);
    }
    //scanning_ = true;
    if (!scanned_refs_.empty() && scanned_refs_.rbegin()->second) {
      scanned_refs_.emplace(std::make_pair(current_addr, nullptr));
    }
  }
  void OnConditionalBranch(uintptr_t current_addr, uintptr_t dest_addr) {
    if (verbose) {
      fprintf(stderr, "Conditional branch at: %lx -> %lx\n", current_addr, dest_addr);
    }
    AddBranchTarget(current_addr, dest_addr);
  }
 private:
  void AddRef(Object* to) {
    sink_->AddRef(current_function_, to);
  }
  void AddOrStore(uintptr_t current_addr, Object* dest) {
    if (!current_function_) {
      return;
    }
    MaybeStopScanning(current_addr);
    if (scanning_) {
      scanned_refs_.emplace(std::make_pair(current_addr, dest));
    } else {
      AddRef(dest);
    }
  }
  void AddBranchTarget(uintptr_t current_addr, uintptr_t dest_addr) {
    MaybeStopScanning(current_addr);
    if (dest_addr > current_addr) {
      // Forward branch.  Store the branch target so we know to stop passively
      // scanning when we see it.
      branch_targets_.insert(dest_addr);
    } else {
      // Backward branch.  Add any scanned-but-unadded refs (if any) starting at
      // this dest.
      auto r = &scanned_refs_;
      for (auto it = r->lower_bound(dest_addr); it != r->end() ; ) {
        if (it->second == NULL) {
          break;
        } else {
          AddRef(it->second);
          r->erase(it++);
        }
      }
    }
  }
  void MaybeStopScanning(uintptr_t current_addr) {
    auto it = branch_targets_.begin();
    if (it != branch_targets_.end() && current_addr >= *it) {
      branch_targets_.erase(it);
      scanning_ = false;
    }
  }
  // While we are scanning through code we think is unreachable, we accumulate
  // the addresses we find here, in case we find out later that the code is
  // actually reachable.  The pairs are:
  //   (current address, Object*)
  //
  // A NULL Object* represents an unconditional branch.
  std::map<uintptr_t, Object*> scanned_refs_;
  // When we think we are in unreachable code and we are just scanning, this is
  // ture.
  bool scanning_ = false;
  // The function we are inside. 
  Object* current_function_ = NULL;
  // Track a list of jump targets we have seen.  If we see an unconditional
  // branch (jmp or ret) we start putting addresses in scanned_addresses instead
  // of adding them directly until we hit the next jump target.
  std::set<uintptr_t> branch_targets_;
  ProgramDataSink* sink_;
};

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

  // jmp    4b0c40 <
  // 40059b:       eb 03                   jmp    4005a0 <_ZN10LogMessageC2EPKcii.constprop.136+0x136>
  // 52c597:       c3                      ret
  RE2 unconditional_jmp(R"(jmp\s+([0-9a-f]+))");

  //   40287b:       73 03                   jae    402880 <_ZN19DominatorCalculator8CompressEj+0x70>
  RE2 conditional_jmp(R"(\sj[a-z]+\s+([0-9a-f]+) <)");

  //   43f941:       4c 8d 25 70 44 21 00    lea    r12,[rip+0x214470]        # 653db8 <__frame_dummy_init_array_entry+0x10>
  RE2 ref_pattern(R"(# [0-9a-f]+ <([\w.]+)(?:\+0x[0-9a-f]+)?>)");

  // To capture tha address at the beginning of the line.
  RE2 addr_pattern(R"(^\s*([0-9a-f]+))");

  // Some references only show up like this. :(
  // It's a guess whether this is really an address.
  //
  // If we get false positives We could be smarter here and try to analyze the
  // assembly after this instruction to determine whether this value is used in
  // a memory reference.
  //   42fdc5:       41 bf e0 f2 51 00       mov    r15d,0x51f2e0
  RE2 const_pattern(R"(0x([0-9a-f]+))");

  std::string current_function_name;
  std::string name;
  uintptr_t current_addr;
  uintptr_t addr;

  RefAdder adder(sink);

  for (auto& line : ReadLinesFromPipe(cmd)) {
    if (RE2::FullMatch(line, func_pattern, &current_function_name)) {
      adder.OnFunction(current_function_name);
    } else if (RE2::PartialMatch(line, addr_pattern, RE2::Hex(&current_addr))) {
      if (RE2::PartialMatch(line, call_pattern, &name) ||
          RE2::PartialMatch(line, ref_pattern, &name)) {
        adder.OnNameRef(current_addr, name);
      } else if (RE2::PartialMatch(line, unconditional_jmp, RE2::Hex(&addr))) {
        adder.OnUnconditionalBranch(current_addr, addr);
      } else if (line.find(" ret ") != std::string::npos) {
        adder.OnUnconditionalBranch(current_addr, 0);
      } else if (RE2::PartialMatch(line, conditional_jmp, RE2::Hex(&addr))) {
        adder.OnConditionalBranch(current_addr, addr);
      } else {
        re2::StringPiece piece(line);
        while (RE2::FindAndConsume(&piece, const_pattern, RE2::Hex(&addr))) {
          adder.OnAddrRef(current_addr, addr);
        }
      }
    }
  }
}

void ParseELFFileMapping(const std::string& filename, ProgramDataSink* sink) {
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
      sink->AddFileMapping(vmaddr, fileoff, filesize);
    } else if (RE2::PartialMatch(line, entry_pattern, RE2::Hex(&entry_addr))) {
      // We've captured the entry point.
    }
  }

  Object* entry = sink->FindObjectByAddr(entry_addr);
  sink->SetEntryPoint(entry);
}

std::string ReadELFBuildId(const std::string& filename) {
  // Build ID: 669eba985387d24f7d959e0f0e49d9209bcf975b
  RE2 pattern(R"(Build ID: (\w+))");
  std::string cmd = std::string("readelf -n ") + filename;
  std::string build_id;

  for (auto& line : ReadLinesFromPipe(cmd)) {
    if (RE2::PartialMatch(line, pattern, &build_id)) {
      return build_id;
    }
  }

  std::cerr << "Didn't find BUILD id.\n";
  exit(1);
}

void ReadELFObjectData(const std::string& filename, ProgramDataSink* sink) {
  ParseELFSymbols(filename, sink);
  ParseELFSections(filename, sink);
  ParseELFDisassembly(filename, sink);
  ParseELFFileMapping(filename, sink);
}
