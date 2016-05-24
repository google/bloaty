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

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <stdlib.h>

#include "re2/re2.h"
#include <assert.h>


/** LineReader ****************************************************************/

// Provides range-based for, for iterating over lines in a pipe.

class LineIterator;

class LineReader {
 public:
  LineReader(FILE* file) : file_(file), eof_(false) {}
  ~LineReader() { pclose(file_); }
  LineIterator begin();
  LineIterator end();

  bool operator!=(int x) const { return !eof_; }

  void Next() {
    size_t size;
    char *buf = fgetln(file_, &size);
    if (buf) {
      line_.assign(buf, size);
      eof_ = false;
    } else if (feof(file_)) {
      eof_ = true;
    } else {
      std::cerr << "Error reading from file.\n";
    }
  }

  const std::string& line() const { return line_; }
  bool eof() { return eof_; }

 private:
  FILE* file_;
  std::string line_;
  bool eof_;
};

class LineIterator {
 public:
  LineIterator(LineReader* reader) : reader_(reader) {}

  bool operator!=(const LineIterator& other) const {
    return !reader_->eof();
  }

  void operator++() { reader_->Next(); }

  const std::string& operator*() const {
    return reader_->line();
  }

 private:
  LineReader* reader_;
};

LineIterator LineReader::begin() { return LineIterator(this); }
LineIterator LineReader::end() { return LineIterator(NULL); }

LineReader ReadLinesFromPipe(const std::string& cmd) {
  FILE* pipe = popen(cmd.c_str(), "r");

  if (!pipe) {
    std::cerr << "Failed to run command: " << cmd << "\n";
    exit(1);
  }

  return LineReader(pipe);
}

class NameStripper {
 public:
  bool StripName(const std::string& name) {
    size_t paren = name.find_first_of('(');
    if (paren == std::string::npos) {
      stripped_ = &name;
      return false;
    } else {
      storage_ = name.substr(0, paren);
      stripped_ = &storage_;
      return true;
    }
  }

  const std::string& stripped() { return *stripped_; }

 private:
  const std::string* stripped_;
  std::string storage_;
};

class Demangler {
 public:
  std::string Demangle(const std::string& symbol) {
    // TODO: optimize to keep a pipe open so we don't exec c++filt separately for
    // every single symbol.
    std::string last;
    for ( auto& line : ReadLinesFromPipe("c++filt " + symbol) ) {
      last = line;
    }
    if (!last.empty()) {
      last.erase(last.size() - 1);
    }
    return last;
  }
};


/** Program data structures ***************************************************/

struct File {
  File(const std::string& name_) : name(name_), source_line_weight(0) {}

  std::string name;

  // Number of object bytes attributed to this file through source line info.
  size_t source_line_weight;

  // Files we reference, ie. through a function call or variable reference.
  std::set<File*> refs;
};

struct Object {
  Object(const std::string& name_) :
      name(name_),
      size(0),
      in_ref_count(0),
      seen_count(0),
      file(NULL) {}

  // Declared name of the symbol.
  std::string name;

  // Name possibly put through c++filt, but also reduced to remove overloads if possible.
  std::string pretty_name;

  void SetSize(size_t size_) {
    size = size_;
    weight = size_;
  }

  uintptr_t vmaddr;
  size_t size;
  size_t weight;
  uint32_t in_ref_count;
  uint32_t seen_count;
  uint32_t first_id;
  std::vector<Object*> first_stack;

  // Whether this is from a data section (rather than code).
  // When this is true we will scan the object for pointers.
  bool data;

  // Source file where this object was declared, if we know it.
  File* file;

  // Objects we reference, ie. through a function call or variable reference.
  std::set<Object*> refs;
};

class Stacks {
 public:
  void Insert(Object* obj, uint32_t id) {
    stacks_[id].push_back(obj);
    stack_.push_back(obj);
    stack_set_.insert(obj);
  }

  bool IsOnStack(Object* obj) {
    return stack_set_.find(obj) != stack_set_.end();
  }

  void Remove(Object* obj, uint32_t id) {
    assert(stacks_[id].back() == obj);
    assert(stack_.back() == obj);
    stacks_[id].pop_back();
    stack_.pop_back();
    stack_set_.erase(obj);
  }

  Object* FindAncestor(Object* node) {
    std::cerr << "Finding ancestor for " << node->name << "\n";
    // Find ancestor in the more efficient way.
    uint32_t id = node->first_id;
    auto it = stacks_.upper_bound(id);
    assert(it != stacks_.begin());
    --it;
    while (it->second.empty()) {
      assert(it != stacks_.begin());
      --it;
    }
    Object* ancestor = it->second.back();

    std::cerr << "The fast way yielded " << ancestor->name << "\n";

    // Find ancestor in the safer way.
    for (int i = node->first_stack.size() - 1; i >= 0; i--) {
      if (stack_set_.find(node->first_stack[i]) != stack_set_.end()) {
        std::cerr << "The safe way yielded " << node->first_stack[i]->name << "\n";
        assert(node->first_stack[i] == ancestor);
        break;
      }
    }

    return ancestor;
  }

  Object* GetTop() {
    assert(!stack_.empty());
    return stack_.back();
  }

  void StoreFirstStack(Object* obj, uint32_t id) {
    obj->first_id = id;
    obj->first_stack = stack_;
  }

 private:
  std::map<uint32_t, std::vector<Object*>> stacks_;
  std::vector<Object*> stack_;
  std::set<Object*> stack_set_;
};

class Program {
 public:
  Object* AddObject(const std::string& name, uintptr_t vmaddr, size_t size, bool data) {

    auto pair = objects_.emplace(name, name);
    Object* ret = &pair.first->second;
    ret->vmaddr = vmaddr;
    ret->SetSize(size);
    ret->data = data;
    ret->name = name;
    objects_by_addr_[vmaddr] = ret;

    auto demangled = demangler_.Demangle(name);
    if (stripper_.StripName(demangled)) {
      auto it = stripped_pretty_names_.find(stripper_.stripped());
      if (it == stripped_pretty_names_.end()) {
        stripped_pretty_names_[stripper_.stripped()] = ret;
        ret->pretty_name = stripper_.stripped();
      } else {
        ret->pretty_name = demangled;
        if (it->second) {
          it->second->pretty_name = demangler_.Demangle(it->second->name);
          it->second = NULL;
        }
      }
    } else {
      ret->pretty_name = demangled;
    }

    return ret;
  }

  void TryAddRef(Object* from, uintptr_t vmaddr) {
    if (!from) {
      return;
    }

    auto it = objects_by_addr_.find(vmaddr);
    if (it != objects_by_addr_.end()) {
      Object* to = it->second;
      if (to) {
        if (from->refs.insert(to).second) {
          to->in_ref_count++;
        }
        if (from->file && to->file) {
          from->file->refs.insert(to->file);
        }
      }
    }
  }

  File* GetFile(const std::string& filename) {
    // C++17: auto pair = files_.try_emplace(filename, filename);
    auto it = files_.find(filename);
    if (it == files_.end()) {
      it = files_.emplace(filename, filename).first;
    }
    return &it->second;
  }

  bool HasFiles() { return files_.size() > 0; }

  Object* FindFunctionByName(const std::string& name) {
    auto it = objects_.find(name);
    return it == objects_.end() ? NULL : &it->second;
  }

  uint32_t CalculateWeights(Object* node, uint32_t id, Stacks* stacks) {
    if (node->seen_count++ == 0) {
      stacks->Insert(node, id);
      stacks->StoreFirstStack(node, id);
      for ( auto& target : node->refs ) {
        if (!stacks->IsOnStack(target)) {  // Break cycles for now.
          id = CalculateWeights(target, id, stacks) + 1;
        }
      }
      stacks->Remove(node, node->first_id);
    }

    if (node->in_ref_count == 0) {
      // Nobody references us, so we don't add our weight to anything.
      std::cerr << "Not adding weight from " << node->name << ", nobody references us.\n";
    } else if (node->in_ref_count == 1) {
      // We have exactly one caller, so just add ourself to our caller's weight.
      std::cerr << "Adding weight from " << node->name << " to " << stacks->GetTop()->name << "\n";
      stacks->GetTop()->weight += node->weight;
    } else if (node->in_ref_count == node->seen_count) {
      // We have multiple callers, find the lowest common ancestor by looking
      // for the deepest node on our stack with a first_id <= to ours.
      Object* ancestor = stacks->FindAncestor(node);
      std::cerr << "Ancestor for " << node->name << " was " << ancestor->name << "\n";
      ancestor->weight += node->weight;
    } else {
      std::cerr << "Skipping: " << node->name << " because we expect " << (node->in_ref_count - node->seen_count) << " more in edges.\n";
    }

    return id;
  }

  void PrintDotGraph(Object* obj, std::ofstream* out, std::set<Object*>* seen) {
    if (!seen->insert(obj).second) {
      return;
    }

    *out << "  \"" << obj->name << "\" [label=\"" << obj->pretty_name << "\\nsize: " << obj->size << "\\nweight: " << obj->weight << "\"];\n";

    for ( auto& target : obj->refs ) {
      //if (target->weight > 200) {
        *out << "  \"" << obj->name << "\" -> \"" << target->name << "\";\n";
        PrintDotGraph(target, out, seen);
      //}
    }
  }

  void PrintSymbolsByTransitiveWeight() {
    Object* obj = FindFunctionByName("_main");
    Stacks stacks;
    CalculateWeights(obj, 0, &stacks);
    /*

    std::vector<Object*> object_list;
    object_list.reserve(objects_.size());
    for ( auto& pair : objects_ ) {
      object_list.push_back(&pair.second);
      assert(pair.first == pair.second.name);
    }

    std::sort(object_list.begin(), object_list.end(), [](Object* a, Object* b) {
      return a->weight > b->weight;
    });

    for ( auto object : object_list) {
      printf(" %7d %s\n", (int)object->weight, object->name.c_str());
    }
    */

    std::ofstream out("graph.dot");
    out << "digraph weights {\n";
    /*
    for ( auto object : object_list) {
      out << "  \"" << object->name << "\"\n";
    }
    */
    std::set<Object*> seen;
    PrintDotGraph(obj, &out, &seen);
    out << "}\n";
  }

  void GC(Object* obj, std::set<Object*>* garbage) {
    if (garbage->erase(obj) != 1) {
      return;
    }

    std::cerr << "Reached obj: " << obj->name << "\n";

    for ( auto& child : obj->refs ) {
      GC(child, garbage);
    }
  }

  void GCFiles(File* file, std::set<File*>* garbage) {
    if (garbage->erase(file) != 1) {
      return;
    }

    std::cerr << "Reached file: " << file->name << "\n";

    for ( auto& child : file->refs ) {
      GCFiles(child, garbage);
    }
  }

  void PrintGarbage() {
    std::set<Object*> garbage;

    for ( auto& pair : objects_ ) {
      garbage.insert(&pair.second);
    }

    Object* obj = FindFunctionByName("_main");

    if (!obj) {
      std::cerr << "Error: couldn't find main function.\n";
      exit(1);
    }

    GC(obj, &garbage);

    std::cerr << "Total objects: " << objects_.size() << "\n";
    std::cerr << "Garbage objects: " << garbage.size() << "\n";

    for ( auto& obj : garbage ) {
      std::cerr << "Garbage obj: " << obj->name << "\n";
      for ( auto& ref : obj->refs ) {
        ref->in_ref_count--;
      }
    }

    if (obj->file) {
      std::set<File*> garbage_files;
      for ( auto& pair : files_ ) {
        garbage_files.insert(&pair.second);
      }

      GCFiles(obj->file, &garbage_files);

      std::cerr << "Total files: " << files_.size() << "\n";
      std::cerr << "Garbage files: " << garbage_files.size() << "\n";

      for ( auto& file : garbage_files ) {
        std::cerr << "Garbage file: " << file->name << "\n";
      }
    }
  }

  void PrintSymbols() {
    double total = 0;

    std::vector<Object*> object_list;
    object_list.reserve(objects_.size());
    for ( auto& pair : objects_ ) {
      object_list.push_back(&pair.second);
      assert(pair.first == pair.second.name);
      total += pair.second.size;
    }

    std::sort(object_list.begin(), object_list.end(), [](Object* a, Object* b) {
      return a->size > b->size;
    });

    size_t cumulative = 0;

    for ( auto object : object_list) {
      size_t size = object->size;
      cumulative += size;
      const std::string& name = object->pretty_name;
      printf("%5.1f%% %5.1f%%  %6d %s\n", size / total * 100, cumulative / total * 100, (int)size, name.c_str());
    }

    printf("%5.1f%%  %6d %s\n", 100.0, (int)total, "TOTAL");
  }

  void PrintFiles() {
    double total = 0;

    std::vector<File*> file_list;
    file_list.reserve(files_.size());
    for ( auto& pair : files_ ) {
      file_list.push_back(&pair.second);
      total += pair.second.source_line_weight;
    }

    std::sort(file_list.begin(), file_list.end(), [](File* a, File* b) {
      return a->source_line_weight > b->source_line_weight;
    });

    size_t cumulative = 0;

    for ( auto file : file_list) {
      size_t size = file->source_line_weight;
      cumulative += size;
      const std::string& name = file->name;
      printf("%5.1f%% %5.1f%%  %6d %s\n", size / total * 100, cumulative / total * 100, (int)size, name.c_str());
    }

    printf("%5.1f%%  %6d %s\n", 100.0, (int)total, "TOTAL");
  }

  // Files, indexed by filename.
  std::unordered_map<std::string, File> files_;

  // Objects, indexed by name.
  std::unordered_map<std::string, Object> objects_;
  std::unordered_map<uintptr_t, Object*> objects_by_addr_;
  std::unordered_map<std::string, Object*> stripped_pretty_names_;


  NameStripper stripper_;
  Demangler demangler_;
};

bool StartsWith(const std::string& haystack, const std::string& needle) {
  return !haystack.compare(0, needle.length(), needle);
}

void ParseMachOSymbols(const std::string& filename, Program* program) {
  std::string cmd = std::string("symbols -fullSourcePath -noDemangling ") + filename;

  // [16 spaces]0x00000001000009e0 (  0x3297) run_tests [FUNC, EXT, LENGTH, NameNList, MangledNameNList, Merged, NList, Dwarf, FunctionStarts]
  // [16 spaces]0x00000001000015a0 (     0x9) __ZN10LineReader5beginEv [FUNC, EXT, LENGTH, NameNList, MangledNameNList, Merged, NList, Dwarf, FunctionStarts]
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
      File* file = program->GetFile(name);
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

void ParseMachODisassembly(const std::string& filename, Program* program) {
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
        program->TryAddRef(current_function, (long)addr + lea_offset - 16);
        program->TryAddRef(current_function, (long)addr + lea_offset);
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
      current_function = program->FindFunctionByName(current_function_name);

      if (!current_function && current_function_name[0] == '_') {
        current_function_name = current_function_name.substr(1);
      }

      current_function = program->FindFunctionByName(current_function_name);

      if (!current_function) {
        std::cerr << "Couldn't find function for name: " << current_function_name << "\n";
      }
    } else if (RE2::PartialMatch(line, call_pattern, &name) ||
               RE2::PartialMatch(line, leaq_sym_pattern, &name)) {
      while (name[name.size() - 1] == ' ') {
        name.resize(name.size() - 1);
      }
      Object* target_function = program->FindFunctionByName(name);
      if (!target_function && name[0] == '_') {
        name = name.substr(1);
        target_function = program->FindFunctionByName(name);
      }

      if (!current_function) {
        std::cerr << line;
        std::cerr << "Whoops!  Found an edge but no current function.  Name: " << name << "\n";
      } else if (!target_function) {
        std::cerr << line;
        std::cerr << "Whoops!  Couldn't find a function entry for: '" << name << "'\n";
      }

      if (current_function->refs.insert(target_function).second) {
        target_function->in_ref_count++;
      }

      if (current_function->file != NULL && target_function->file != NULL) {
        current_function->file->refs.insert(target_function->file);
      }
    }
  }
}

// Returns the difference between the data segment's file offset and vmaddr.
// This number can be added to a vmaddr to locate the file offset.
long GetDataSegmentDelta(const std::string& filename) {
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

  RE2 seg_pattern(R"(^  segname __DATA)");
  RE2 vmaddr_pattern(R"(^   vmaddr 0x([0-9a-f]+))");
  RE2 fileoff_pattern(R"(^  fileoff ([0-9a-f]+))");
  bool in_dataseg = false;
  size_t vmaddr;
  size_t fileoff;

  for ( auto& line : ReadLinesFromPipe(cmd) ) {
    if (StartsWith(line, "  segname __DATA")) {
      in_dataseg = true;
    } else if (in_dataseg && RE2::PartialMatch(line, vmaddr_pattern, RE2::Hex(&vmaddr))) {
      // Proceed...
    } else if (in_dataseg && RE2::PartialMatch(line, fileoff_pattern, &fileoff)) {
      // This will likely underflow, but that is ok/expected.
      return fileoff - vmaddr;
    }
  }

  std::cerr << "Didn't find data segment.\n";
  exit(1);
}

void ParseVTables(const std::string& filename, Program* program) {
  bool computed_delta = false;
  long delta;

  FILE* f = fopen(filename.c_str(), "rb");

  for ( auto& pair : program->objects_ ) {
    Object* obj = &pair.second;
    if (!obj->data) {
      continue;
    }

    if (!computed_delta) {
      delta = GetDataSegmentDelta(filename);
      computed_delta = true;
    }

    for (size_t i = 0; i < obj->size; i += sizeof(uintptr_t)) {
      uintptr_t addr;
      fseek(f, obj->vmaddr + delta + i, SEEK_SET);
      fread(&addr, sizeof(uintptr_t), 1, f);
      program->TryAddRef(obj, addr);
    }
  }

  fclose(f);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: bloaty <binary file>\n";
    exit(1);
  }

  Program program;
  ParseMachOSymbols(argv[1], &program);
  ParseVTables(argv[1], &program);
  ParseMachODisassembly(argv[1], &program);

  if (!program.HasFiles()) {
    std::cerr << "Warning: no debug information present.\n";
  }

  program.PrintGarbage();
  program.PrintSymbolsByTransitiveWeight();
}
