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
#include <signal.h>

#include "re2/re2.h"
#include <assert.h>

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CHECK_SYSCALL(call) \
  if (call < 0) { \
    perror(#call " " __FILE__ ":" TOSTRING(__LINE__)); \
    exit(1); \
  }

std::string* name_path;


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
  Demangler() {
    int toproc_pipe_fd[2];
    int fromproc_pipe_fd[2];
    if (pipe(toproc_pipe_fd) < 0 || pipe(fromproc_pipe_fd) < 0) {
      perror("pipe");
      exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      exit(1);
    }

    if (pid) {
      // Parent.
      CHECK_SYSCALL(close(toproc_pipe_fd[0]));
      CHECK_SYSCALL(close(fromproc_pipe_fd[1]));
      write_fd_ = toproc_pipe_fd[1];
      read_fd_ = fromproc_pipe_fd[0];
      child_pid_ = pid;
    } else {
      // Child.
      CHECK_SYSCALL(close(STDIN_FILENO));
      CHECK_SYSCALL(close(STDOUT_FILENO));
      CHECK_SYSCALL(dup2(toproc_pipe_fd[0], STDIN_FILENO));
      CHECK_SYSCALL(dup2(fromproc_pipe_fd[1], STDOUT_FILENO));

      CHECK_SYSCALL(close(toproc_pipe_fd[0]));
      CHECK_SYSCALL(close(fromproc_pipe_fd[1]));
      CHECK_SYSCALL(close(toproc_pipe_fd[1]));
      CHECK_SYSCALL(close(fromproc_pipe_fd[0]));

      char prog[] = "c++filt";
      char *const argv[] = {prog, NULL};
      CHECK_SYSCALL(execvp("c++filt", argv));
    }
  }

  ~Demangler() {
    int status;
    kill(child_pid_, SIGTERM);
    waitpid(child_pid_, &status, WEXITED);
  }

  std::string Demangle(const std::string& symbol) {
    char buf[2048];
    const char *writeptr = symbol.c_str();
    const char *writeend = writeptr + symbol.size();
    char *readptr = buf;

    while (writeptr < writeend) {
      ssize_t bytes = write(write_fd_, writeptr, writeend - writeptr);
      if (bytes < 0) {
        perror("read");
        exit(1);
      }
      writeptr += bytes;
    }
    write(write_fd_, "\n", 1);
    do {
      ssize_t bytes = read(read_fd_, readptr, sizeof(buf) - (readptr - buf));
      if (bytes < 0) {
        perror("read");
        exit(1);
      }
      readptr += bytes;
    } while(readptr[-1] != '\n');

    --readptr;  // newline.
    *readptr = '\0';

    std::string ret(buf);

    return ret;
  }

 private:
  int write_fd_;
  int read_fd_;
  pid_t child_pid_;
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
      file(NULL) {}

  // Declared name of the symbol.
  std::string name;

  // Name possibly put through c++filt, but also reduced to remove overloads if possible.
  std::string pretty_name;

  void SetSize(size_t size_) {
    size = size_;
    weight = size_;
  }

  uint32_t id;
  uintptr_t vmaddr;
  size_t size;
  size_t weight;
  size_t max_weight;

  // Whether this is from a data section (rather than code).
  // When this is true we will scan the object for pointers.
  bool data;

  // Source file where this object was declared, if we know it.
  File* file;

  // Objects we reference, ie. through a function call or variable reference.
  std::set<Object*> refs;
};

class DominatorCalculator {
 public:
  static void Calculate(Object* root, uint32_t total,
                        std::unordered_map<Object*, Object*>* dominators) {
    DominatorCalculator calculator;
    calculator.CalculateDominators(root, total);
    for (const auto& info : calculator.node_info_) {
      if (!info.node) {
        // Unreachable nodes won't have this.
        continue;
      }
      if (info.dom == 0) {
        // Main won't have this.  But make sure no node can have id 0.
        continue;
      }
      (*dominators)[info.node] = calculator.node_info_[info.dom].node;
    }
  }

 private:
  void Initialize(Object* pv) {
    uint32_t v = pv->id;
    node_info_[v].node = pv;
    Semi(v) = ++n_;
    Vertex(n_) = v;
    Label(v) = v;
    Ancestor(v) = 0;
    for (const auto& target : pv->refs) {
      uint32_t w = target->id;
      if (Semi(w) == 0) {
        Parent(w) = v;
        Initialize(target);
      }
      Pred(w).insert(v);
    }
  }

  void Link(uint32_t v, uint32_t w) {
    Ancestor(w) = v;
  }

  void Compress(uint32_t v) {
    if (Ancestor(Ancestor(v)) != 0) {
      Compress(Ancestor(v));
      if (Semi(Label(Ancestor(v))) < Semi(Label(v))) {
        Label(v) = Label(Ancestor(v));
      }
      Ancestor(v) = Ancestor(Ancestor(v));
    }
  }

  uint32_t Eval(uint32_t v) {
    if (Ancestor(v) == 0) {
      return v;
    } else {
      Compress(v);
      return Label(v);
    }
  }

  void CalculateDominators(Object* pr, uint32_t total) {
    uint32_t r = pr->id;
    n_ = 0;
    node_info_.resize(total);
    ordering_.resize(total);

    Initialize(pr);

    for (uint32_t i = n_ - 1; i > 0; --i) {
      uint32_t w = Vertex(i);

      for (uint32_t v : Pred(w)) {
        uint32_t u = Eval(v);
        if (Semi(u) < Semi(w)) {
          Semi(w) = Semi(u);
        }
      }
      Bucket(Vertex(Semi(w))).insert(w);
      Link(Parent(w), w);

      for (uint32_t v : Bucket(Parent(w))) {
        uint32_t u = Eval(v);
        Dom(v) = Semi(u) < Semi(v) ? u : Parent(w);
      }
    }

    for (uint32_t i = 1; i < n_; i++) {
      uint32_t w = Vertex(i);
      if (Dom(w) != Vertex(Semi(w))) {
        Dom(w) = Dom(Dom(w));
      }
    }

    Dom(r) = 0;
  }

  uint32_t& Parent(uint32_t v) {
    return node_info_[v].parent;
  }

  uint32_t& Ancestor(uint32_t v) {
    return node_info_[v].ancestor;
  }

  uint32_t& Semi(uint32_t v) {
    return node_info_[v].semi;
  }

  uint32_t& Label(uint32_t v) {
    return node_info_[v].label;
  }

  uint32_t& Dom(uint32_t v) {
    return node_info_[v].dom;
  }

  uint32_t& Vertex(uint32_t v) {
    return ordering_[v];
  }

  std::set<uint32_t>& Pred(uint32_t v) {
    return node_info_[v].pred;
  }

  std::set<uint32_t>& Bucket(uint32_t v) {
    return node_info_[v].bucket;
  }

  uint32_t n_;

  struct NodeInfo {
    Object* node;
    uint32_t parent;
    uint32_t ancestor;
    uint32_t label;
    uint32_t semi;
    uint32_t dom;
    std::set<uint32_t> pred;
    std::set<uint32_t> bucket;
  };
  std::vector<NodeInfo> node_info_;
  std::vector<uint32_t> ordering_;  // i -> (node_info_ index)
};

class Program {
 public:
  Program() : next_id_(1), total_size_(0) {}

  Object* AddObject(const std::string& name, uintptr_t vmaddr, size_t size, bool data) {

    if (name_path && name == *name_path) {
      std::cerr << "Adding object " << name << ", " << data << "\n";
    }

    auto pair = objects_.emplace(name, name);
    Object* ret = &pair.first->second;
    ret->id = next_id_++;
    ret->vmaddr = vmaddr;
    ret->SetSize(size);
    ret->data = data;
    ret->name = name;
    total_size_ += size;
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

  void PrintDotGraph(Object* obj, std::ofstream* out, std::set<Object*>* seen) {
    if (!seen->insert(obj).second) {
      return;
    }

    *out << "  \"" << obj->name << "\" [label=\"" << obj->pretty_name << "\\nsize: " << obj->size << "\\nweight: " << obj->weight << "\", fontsize=" << std::max(obj->size * 800.0 / total_size_, 9.0) << "];\n";

    for ( auto& target : obj->refs ) {
      if (target->max_weight > 500) {
        *out << "  \"" << obj->name << "\" -> \"" << target->name << "\" [penwidth=" << (pow(target->weight * 100.0 / max_weight_, 0.6)) << "];\n";
        PrintDotGraph(target, out, seen);
      }
    }
  }

  void CalculateWeights(Object* obj,
                        const std::unordered_map<Object*, Object*>& dominators,
                        std::set<Object*>* seen) {
    if (!seen->insert(obj).second) {
      return;
    }

    obj->weight = obj->size;
    obj->max_weight = obj->weight;

    for (auto target : obj->refs) {
      CalculateWeights(target, dominators, seen);
      obj->max_weight = std::max(obj->max_weight, target->max_weight);
    }

    auto it = dominators.find(obj);
    //assert(it != dominators.end());
    if (it == dominators.end()) {
    } else {
      it->second->weight += obj->weight;
    }
  }

  void PrintSymbolsByTransitiveWeight() {
    Object* root = FindFunctionByName("_main");

    {
      std::unordered_map<Object*, Object*> dominators;
      DominatorCalculator::Calculate(root, next_id_, &dominators);
      std::set<Object*> seen;
      CalculateWeights(root, dominators, &seen);
      max_weight_  = root->max_weight;
    }
    //CalculateWeights(root);
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
    {
      std::set<Object*> seen;
      PrintDotGraph(root, &out, &seen);
    }
    out << "}\n";
  }

  void GC(Object* obj, std::set<Object*>* garbage, std::vector<Object*>* stack) {
    if (garbage->erase(obj) != 1) {
      return;
    }

    stack->push_back(obj);

    if (name_path && obj->name == *name_path) {
      std::string indent;
      for (auto obj : *stack) {
        indent += "  ";
        std::cerr << indent << "-> " << obj->name << "\n";
      }
    }

    for ( auto& child : obj->refs ) {
      GC(child, garbage, stack);
    }

    stack->pop_back();
  }

  void GCFiles(File* file, std::set<File*>* garbage) {
    if (garbage->erase(file) != 1) {
      return;
    }

    for ( auto& child : file->refs ) {
      GCFiles(child, garbage);
    }
  }

  void PrintGarbage() {
    std::set<Object*> garbage;
    std::vector<Object*> stack;

    for ( auto& pair : objects_ ) {
      garbage.insert(&pair.second);
    }

    Object* obj = FindFunctionByName("_main");

    if (!obj) {
      std::cerr << "Error: couldn't find main function.\n";
      exit(1);
    }

    GC(obj, &garbage, &stack);

    std::cerr << "Total objects: " << objects_.size() << "\n";
    std::cerr << "Garbage objects: " << garbage.size() << "\n";

    for ( auto& obj : garbage ) {
      //std::cerr << "Garbage obj: " << obj->name << "\n";
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
        //std::cerr << "Garbage file: " << file->name << "\n";
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

  uint32_t next_id_;
  size_t total_size_;
  size_t max_weight_;

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

      current_function->refs.insert(target_function);

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
    bool verbose = false;
    if (!obj->data) {
      continue;
    }

    if (name_path && obj->name == *name_path) {
      std::cerr << "VTable scanning " << obj->name << "\n";
      verbose = true;
    }

    if (!computed_delta) {
      delta = GetDataSegmentDelta(filename);
      computed_delta = true;
    }

    for (size_t i = 0; i < obj->size; i += sizeof(uintptr_t)) {
      uintptr_t addr;
      fseek(f, obj->vmaddr + delta + i, SEEK_SET);
      fread(&addr, sizeof(uintptr_t), 1, f);
      if (verbose) {
        fprintf(stderr, "  Try add ref to: %x\n", (int)addr);
      }
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

  if (argc == 3) {
    name_path = new std::string(argv[2]);
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
