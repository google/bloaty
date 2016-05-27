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
#include <unordered_set>
#include <vector>

#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "re2/re2.h"
#include <assert.h>

#include "bloaty.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CHECK_SYSCALL(call) \
  if (call < 0) { \
    perror(#call " " __FILE__ ":" TOSTRING(__LINE__)); \
    exit(1); \
  }

std::string* name_path;

void LineReader::Next() {
  char buf[256];
  line_.clear();
  do {
    if (!fgets(buf, sizeof(buf), file_)) {
      if (feof(file_)) {
        eof_ = true;
      } else {
        std::cerr << "Error reading from file.\n";
        exit(1);
      }
    }
    line_.append(buf);
  } while(!eof_ && line_[line_.size() - 1] != '\n');

  if (!eof_) {
    line_.resize(line_.size() - 1);
  }
}

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

bool verbose;

template <class T>
class RangeMap {
 public:
  void Add(uintptr_t addr, size_t size, T val) {
    mappings_[addr] = std::make_pair(val, size);
  }

  T Get(uintptr_t addr) {
    T ret;
    if (!TryGet(addr, &ret)) {
      fprintf(stderr, "No fileoff for: %lx\n", addr);
      exit(1);
    }
    return ret;
  }

  bool TryGet(uintptr_t addr, T* val) {
    auto it = mappings_.upper_bound(addr);
    if (it == mappings_.begin() || (--it, it->first + it->second.second <= addr)) {
      return false;
    }
    assert(addr >= it->first && addr < it->first + it->second.second);
    *val = it->second.first;
    return true;
  }

  bool TryGetExactly(uintptr_t addr, T* val) {
    auto it = mappings_.find(addr);
    if (it == mappings_.end()) {
      return false;
    }
    *val = it->second.first;
    return true;
  }

 private:
  std::map<uintptr_t, std::pair<T, size_t>> mappings_;
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
    return symbol;
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
    if (write(write_fd_, "\n", 1) != 1) {
      perror("read");
      exit(1);
    }
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
    if (ret.empty()) {
      fprintf(stderr, "Failed to demangle: %s\n", symbol.c_str());
    }
    assert(ret.size() > 0);

    return ret;
  }

 private:
  int write_fd_;
  int read_fd_;
  pid_t child_pid_;
};


/** Program data structures ***************************************************/

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
  Program() : next_id_(1), total_size_(0), entry_(NULL) {}

  Object* AddObject(const std::string& name, uintptr_t vmaddr, size_t size, bool data) {

    if (name_path && name == *name_path) {
      fprintf(stderr, "Adding object %s addr=%lx, size=%lx\n", name.c_str(), vmaddr, size);
    }

    Object* ret = new Object(name);
    objects_[name] = ret;
    ret->id = next_id_++;
    ret->vmaddr = vmaddr;
    ret->SetSize(size);
    ret->data = data;
    ret->name = name;
    total_size_ += size;
    objects_by_addr_.Add(vmaddr, size, ret);

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

  void AddObjectAlias(Object* obj, const std::string& name) {
    objects_[name] = obj;

    // Keep the lexicographcally-first name as the canonical one.
    if (name < obj->name) {
      obj->aliases.insert(obj->name);
      obj->name = name;
    } else {
      obj->aliases.insert(name);
    }
  }

  void AddFileMapping(uintptr_t vmaddr, uintptr_t fileoff, size_t filesize) {
    file_offsets_.Add(vmaddr, filesize, vmaddr - fileoff);
  }

  bool TryGetFileOffset(uintptr_t vmaddr, uintptr_t *ofs) {
    uintptr_t diff;
    if (file_offsets_.TryGet(vmaddr, &diff)) {
      *ofs = vmaddr - diff;
      return true;
    } else {
      return false;
    }
  }

  void SetEntryPoint(Object* obj) {
    entry_ = obj;
  }

  void TryAddRef(Object* from, uintptr_t vmaddr) {
    if (!from) {
      return;
    }

    Object* to;
    if (objects_by_addr_.TryGet(vmaddr, &to)) {
      if (verbose) {
        fprintf(stderr, "Added ref! %s -> %s\n", from->pretty_name.c_str(), to->pretty_name.c_str());
      }
      from->refs.insert(to);
      //if (to) {
        if (from->file && to->file) {
          from->file->refs.insert(to->file);
        }
      //}
    }
  }

  File* GetOrCreateFile(const std::string& filename) {
    // C++17: auto pair = files_.try_emplace(filename, filename);
    auto it = files_.find(filename);
    if (it == files_.end()) {
      it = files_.emplace(filename, filename).first;
    }
    return &it->second;
  }

  bool HasFiles() { return files_.size() > 0; }

  Object* FindObjectByName(const std::string& name) {
    auto it = objects_.find(name);
    return it == objects_.end() ? NULL : it->second;
  }

  Object* FindObjectByAddr(uintptr_t addr) {
    Object* ret;
    if (objects_by_addr_.TryGetExactly(addr, &ret)) {
      return ret;
    } else {
      return NULL;
    }
  }

  Object* FindObjectContainingAddr(uintptr_t addr) {
    Object* ret;
    if (objects_by_addr_.TryGet(addr, &ret)) {
      return ret;
    } else {
      return NULL;
    }
  }

  void PrintDotGraph(Object* obj, std::ofstream* out, std::set<Object*>* seen) {
    if (!seen->insert(obj).second) {
      return;
    }

    *out << "  \"" << obj->name << "\" [label=\"" << obj->pretty_name << "\\nsize: " << obj->size << "\\nweight: " << obj->weight << "\", fontsize=" << std::max(obj->size * 80000.0 / total_size_, 9.0) << "];\n";

    for ( auto& target : obj->refs ) {
      if (target->max_weight > 30000) {
        *out << "  \"" << obj->name << "\" -> \"" << target->name << "\" [penwidth=" << (pow(target->weight * 100.0 / max_weight_, 0.6)) << "];\n";
        PrintDotGraph(target, out, seen);
      }
    }
  }

  void CalculateWeightsRecursive(
      Object* obj, const std::unordered_map<Object*, Object*>& dominators,
      std::set<Object*>* seen) {
    if (!seen->insert(obj).second) {
      return;
    }

    obj->weight = obj->size;
    obj->max_weight = obj->weight;

    for (auto target : obj->refs) {
      CalculateWeightsRecursive(target, dominators, seen);
      obj->max_weight = std::max(obj->max_weight, target->max_weight);
    }

    auto it = dominators.find(obj);
    //assert(it != dominators.end());
    if (it == dominators.end()) {
    } else {
      it->second->weight += obj->weight;
    }
  }

  void CalculateWeights() {
    if (!entry_) {
      std::cerr << "Transitive weight graph requires entry point.";
    }

    std::unordered_map<Object*, Object*> dominators;
    DominatorCalculator::Calculate(entry_, next_id_, &dominators);
    std::set<Object*> seen;
    CalculateWeightsRecursive(entry_, dominators, &seen);
    max_weight_  = entry_->max_weight;
  }

  void PrintSymbolsByTransitiveWeight() {
    std::vector<Object*> object_list;
    object_list.reserve(objects_.size());
    for ( auto& pair : objects_ ) {
      object_list.push_back(pair.second);
    }

    std::sort(object_list.begin(), object_list.end(), [](Object* a, Object* b) {
      return a->weight > b->weight;
    });

    int i = 0;
    for (auto object : object_list) {
      if (++i > 40) break;
      printf(" %7d %s\n", (int)object->weight, object->pretty_name.c_str());
    }

    std::ofstream out("graph.dot");
    out << "digraph weights {\n";
    /*
    for ( auto object : object_list) {
      out << "  \"" << object->name << "\"\n";
    }
    */
    {
      std::set<Object*> seen;
      PrintDotGraph(entry_, &out, &seen);
    }
    out << "}\n";
  }

  void GC(Object* obj, std::unordered_set<Object*>* garbage,
          std::vector<Object*>* stack) {
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
    std::unordered_set<Object*> garbage;
    std::vector<Object*> stack;

    for ( auto& pair : objects_ ) {
      garbage.insert(pair.second);
    }

    if (!entry_) {
      std::cerr << "Error: Can't calculate garbage without entry point.\n";
      exit(1);
    }

    GC(entry_, &garbage, &stack);
    std::vector<Object*> garbage_sorted;
    std::copy(garbage.begin(), garbage.end(),
              std::back_inserter(garbage_sorted));
    std::sort(garbage_sorted.begin(), garbage_sorted.end(), [](Object* a, Object* b) {
      return a->pretty_name > b->pretty_name;
    });

    for (auto& obj : garbage_sorted) {
      //if (name_path && obj->name == *name_path) {
      //if (obj->size > 0) {
      //  fprintf(stderr, "Garbage obj: %s (%s %lx)\n", obj->pretty_name.c_str(), obj->name.c_str(), obj->vmaddr);
      //}
      //}
    }

    if (entry_->file) {
      std::set<File*> garbage_files;
      for ( auto& pair : files_ ) {
        garbage_files.insert(&pair.second);
      }

      GCFiles(entry_->file, &garbage_files);

      std::cerr << "Total files: " << files_.size() << "\n";
      std::cerr << "Garbage files: " << garbage_files.size() << "\n";

      //for ( auto& file : garbage_files ) {
        //std::cerr << "Garbage file: " << file->name << "\n";
      //}
    }

    std::cerr << "Total objects: " << objects_.size() << "\n";
    std::cerr << "Garbage objects: " << garbage.size() << "\n";
  }

  void PrintSymbols() {
    double total = 0;

    std::vector<Object*> object_list;
    object_list.reserve(objects_.size());
    for ( auto& pair : objects_ ) {
      object_list.push_back(pair.second);
      assert(pair.first == pair.second->name);
      total += pair.second->size;
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
  std::unordered_map<std::string, Object*> objects_;
  std::unordered_map<std::string, Object*> stripped_pretty_names_;
  RangeMap<Object*> objects_by_addr_;
  RangeMap<uintptr_t> file_offsets_;
  Object* entry_;

  NameStripper stripper_;
  Demangler demangler_;
};

Object* ProgramDataSink::AddObject(const std::string& name, uintptr_t vmaddr,
                                   size_t size, bool data) {
  return program_->AddObject(name, vmaddr, size, data);
}

Object* ProgramDataSink::FindObjectByName(const std::string& name) {
  return program_->FindObjectByName(name);
}

Object* ProgramDataSink::FindObjectByAddr(uintptr_t addr) {
  return program_->FindObjectByAddr(addr);
}

Object* ProgramDataSink::FindObjectContainingAddr(uintptr_t addr) {
  return program_->FindObjectContainingAddr(addr);
}

File* ProgramDataSink::GetOrCreateFile(const std::string& filename) {
  return program_->GetOrCreateFile(filename);
}

void ProgramDataSink::AddObjectAlias(Object* obj, const std::string& name) {
  program_->AddObjectAlias(obj, name);
}

void ProgramDataSink::AddRef(Object* from, Object* to) {
  if (name_path &&
      (from->name == *name_path || to->name == *name_path)) {
    std::cerr << "  Add ref from " << from->pretty_name << " to " << to->pretty_name << "\n";
  }
  from->refs.insert(to);
}

void ProgramDataSink::SetEntryPoint(Object* obj) {
  program_->SetEntryPoint(obj);
}

void ProgramDataSink::AddFileMapping(uintptr_t vmaddr, uintptr_t fileoff,
                                     size_t filesize) {
  program_->AddFileMapping(vmaddr, fileoff, filesize);
}

void ParseVTables(const std::string& filename, Program* program) {
  FILE* f = fopen(filename.c_str(), "rb");

  for (auto& pair : program->objects_) {
    Object* obj = pair.second;

    if (!obj->data) {
      continue;
    }

    if (name_path && obj->name == *name_path) {
      std::cerr << "VTable scanning " << obj->name << "\n";
      verbose = true;
    } else {
      verbose = false;
    }

    uintptr_t base;
    if (!program->TryGetFileOffset(obj->vmaddr, &base)) {
      continue;
    }
    fseek(f, base, SEEK_SET);

    for (size_t i = 0; i < obj->size; i += sizeof(uintptr_t)) {
      uintptr_t addr;
      if (fread(&addr, sizeof(uintptr_t), 1, f) != 1) {
        perror("fread");
        exit(1);
      }

      if (verbose) {
        fprintf(stderr, "  Try add ref to: %x\n", (int)addr);
      }
      program->TryAddRef(obj, addr);
    }
  }

  fclose(f);
}

std::string GetCacheFilename(std::string filename,
                             const std::string& build_id) {
  return std::string("/tmp/blc.") + basename(&filename[0]) + "." + build_id;
}

bool TryOpenCacheFile(const std::string& filename, const std::string& build_id,
                      Program* program) {
  const std::string cache_filename = GetCacheFilename(filename, build_id);
  std::ifstream input(cache_filename);
  if (!input.is_open()) {
    return false;
  }
  std::cerr << "Found cache file: " << cache_filename << "\n";

#define FIELD R"(([^\t]+)\t)"
  RE2 pattern(FIELD FIELD FIELD FIELD FIELD R"(([^\t]+))");
  RE2 pattern2(R"(([^, ]+))");
  RE2 refs_pattern(R"(([^ ]+) -> {([^}]*)})");
  RE2 entry_pattern(R"(ENTRY: ([^ \n]+))");
  std::string line;
  uintptr_t vmaddr;
  uintptr_t size;
  uintptr_t weight;
  uintptr_t max_weight;
  std::string name;
  std::string refs;
  std::string from;
  std::string aliases;
  while (std::getline(input, line)) {
    if (RE2::FullMatch(line, pattern, RE2::Hex(&vmaddr), RE2::Hex(&size),
                       RE2::Hex(&weight), RE2::Hex(&max_weight),
                       &name, &aliases)) {
      Object* obj = program->AddObject(name, vmaddr, size, false);
      obj->weight = weight;
      obj->max_weight = max_weight;

      std::string alias;
      re2::StringPiece piece(aliases);
      while (RE2::FindAndConsume(&piece, pattern2, &alias)) {
        obj->aliases.insert(alias);
      }
    } else if (RE2::FullMatch(line, refs_pattern, &from, &refs)) {
      Object* obj = program->FindObjectByName(from);
      if (!obj) {
        std::cerr << "Ref from undefined object? " << name << "\n";
      }

      re2::StringPiece piece(refs);
      std::string ref;
      while (RE2::FindAndConsume(&piece, pattern2, &ref)) {
        Object* target = program->FindObjectByName(ref);
        if (!target) {
          std::cerr << "Ref to undefined object? " << name << ", " << ref << "\n";
          exit(1);
        }
        obj->refs.insert(target);
      }

    } else if (RE2::FullMatch(line, entry_pattern, &name)) {
      Object* obj = program->FindObjectByName(name);
      if (!obj) {
        std::cerr << "Unknown entry point? " << name << "\n";
      }
      program->SetEntryPoint(obj);
    } else {
      std::cerr << "Bad line: " << line << "\n";
      exit(1);
    }
  }

  return true;
}

void WriteCacheFile(Program* program, const std::string& filename,
                    const std::string& build_id) {
  const std::string cache_filename = GetCacheFilename(filename, build_id);
  std::ofstream output(cache_filename);
  if (!output.is_open()) {
    std::cerr << "Couldn't open for writing: " << cache_filename << "\n";
    exit(1);
  }

  const auto& object_map = program->objects_;

  // Create an ordered list of canonical names.
  std::set<std::string> names;
  for (const auto& pair : object_map) {
    names.insert(pair.second->name);  // Might be a duplicate.
  }

  for (const auto& name : names) {
    Object* obj = program->FindObjectByName(name);
    output << std::hex << obj->vmaddr << "\t";
    output << std::hex << obj->size << "\t";
    output << std::hex << obj->weight << "\t";
    output << std::hex << obj->max_weight << "\t";
    output << obj->name << "\t";

    output << "{";
    bool first = true;
    for (const auto& alias : obj->aliases) {
      if (first) {
        first = false;
      } else {
        output << ", ";
      }
      // Could output IDs instead of names for significant size reduction.
      output << alias;
    }
    output << "}";
    output << "\n";
  }

  for (const auto& name : names) {
    Object* obj = program->FindObjectByName(name);
    if (obj->refs.empty()) {
      continue;
    }

    output << obj->name << " -> ";
    output << "{";
    bool first = true;
    for (const auto& target : obj->refs) {
      if (first) {
        first = false;
      } else {
        output << ", ";
      }
      // Could output IDs instead of names for significant size reduction.
      output << target->name;
    }
    output << "}";
    output << "\n";
  }

  if (program->entry_) {
    output << "ENTRY: " << program->entry_->name << "\n";
  }

  std::cerr << "Wrote cache file: " << cache_filename << "\n";
}

void ReadObjectData(const std::string& filename, ProgramDataSink* sink) {
#ifdef __APPLE__
  ReadMachOObjectData(filename, sink);
#else
  ReadELFObjectData(filename, sink);
#endif
}

std::string ReadBuildId(const std::string& filename) {
#ifdef __APPLE__
  return ReadMachOBuildId(filename);
#else
  return ReadELFBuildId(filename);
#endif
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: bloaty <binary file>\n";
    exit(1);
  }

  const std::string filename = argv[1];

  if (argc == 3) {
    name_path = new std::string(argv[2]);
  }

  Program program;
  ProgramDataSink sink(&program);

  const std::string build_id = ReadBuildId(filename);
  bool found_cache = TryOpenCacheFile(filename, build_id, &program);

  if (!found_cache) {
    ReadObjectData(filename, &sink);
    ParseVTables(filename, &program);
    program.CalculateWeights();
    if (!program.HasFiles()) {
      std::cerr << "Warning: no debug information present.\n";
    }
  }

  if (!found_cache) {
    WriteCacheFile(&program, filename, build_id);
  }

  program.PrintGarbage();
  program.PrintSymbolsByTransitiveWeight();
}
