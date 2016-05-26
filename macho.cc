
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

bool StartsWith(const std::string& haystack, const std::string& needle) {
  return !haystack.compare(0, needle.length(), needle);
}

static void ParseMachOSymbols(const std::string& filename,
                              ProgramDataSink* program) {
  std::string cmd = std::string("symbols -fullSourcePath -noDemangling ") + filename;

  // [16 spaces]0x00000001000009e0 (  0x3297) run_tests [FUNC, EXT, LENGTH, NameNList, MangledNameNList, Merged, NList, Dwarf, FunctionStarts]
  // [16 spaces]0x00000001000015a0 (     0x9) __ZN10LineReader5beginEv [FUNC, EXT, LENGTH, NameNList, MangledNameNList, Merged, NList, Dwarf, FunctionStarts]
  // [16 spaces]0x0000000100038468 (     0x8) __ZN3re2L12empty_stringE [NameNList, MangledNameNList, NList]
  RE2 pattern1(R"(^\s{16}0x([0-9a-f]+) \(\s*0x([0-9a-f]+)\) (.+) \[((?:FUNC)?))");

  // [20 spaces]0x00000001000009e0 (    0x21) tests/test_def.c:471
  RE2 pattern2(R"(^\s{20}0x([0-9a-f]+) \(\s*0x([0-9a-f]+)\) (.+):(\d+))");

  Object* obj_with_unknown_filename = NULL;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    std::string name;
    std::string maybe_func;
    size_t addr, size;
    int line_num;

    if (RE2::PartialMatch(line, pattern1, RE2::Hex(&addr), RE2::Hex(&size), &name, &maybe_func)) {
      if (StartsWith(name, "DYLD-STUB")) {
        continue;
      }

      obj_with_unknown_filename = program->AddObject(name, addr, size, maybe_func.empty());
    } else if (RE2::PartialMatch(line, pattern2, RE2::Hex(&addr), RE2::Hex(&size), &name, &line_num)) {
      // OPT: avoid the lookup if this is the same filename as the last entry.
      File* file = program->GetOrCreateFile(name);
      if (obj_with_unknown_filename) {
        // This is a heuristic.  We assume that the first source line for a
        // function represents the file that function was declared in.  Ideally
        // the debug info would tell us this directly.
        obj_with_unknown_filename->file = file;
        obj_with_unknown_filename =  NULL;
      }
      file->source_line_weight += size;
    }
  }
}

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

// Returns the difference between the data segment's file offset and vmaddr.
// This number can be added to a vmaddr to locate the file offset.
static void ParseMachOFileMapping(const std::string& filename,
                                  ProgramDataSink* sink) {
  std::string cmd = std::string("otool -l ") + filename;
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

  RE2 key_decimal_pattern(R"((\w+) ([1-9][0-9]*))");
  RE2 key_hex_pattern(R"((\w+) 0x([0-9a-f]+))");
  RE2 key_text_pattern(R"( (\w+) (\w+))");
  std::string key;
  std::string text_val;
  uintptr_t val;
  uintptr_t vmaddr;
  uintptr_t fileoff;
  uintptr_t filesize;

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
      } else if (key == "fileoff") {
        fileoff = val;
      } else if (key == "filesize") {
        filesize = val;
        sink->AddFileMapping(vmaddr, fileoff, filesize);
      } else if (key == "entryoff") {
        Object* entry = sink->FindObjectByAddr(first_text_vmaddr + val);
        if (entry) {
          sink->SetEntryPoint(entry);
        }
      }
    } else if (RE2::PartialMatch(line, key_text_pattern, &key, &text_val)) {
      if (key == "segname") {
        in_text_section = (text_val == "__TEXT");
      }
    }

  }
}

void ReadObjectData(const std::string& filename, ProgramDataSink* sink) {
  ParseMachOSymbols(filename, sink);
  ParseMachODisassembly(filename, sink);
  ParseMachOFileMapping(filename, sink);
}
