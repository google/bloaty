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
#include <memory>
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
#include "leveldb/table.h"
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

std::string ClampSize(const std::string& input, size_t size) {
  if (input.size() < size) {
    return input;
  } else {
    return input.substr(0, size);
  }
}

std::string SiPrint(size_t size) {
  const char *prefixes[] = {"", "k", "M", "G", "T"};
  int n = 0;
  while (size > 1024) {
    size /= 1024;
    n++;
  }

  return std::to_string(size) + prefixes[n];
}

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

  return LineReader(pipe, true);
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
      int write_fd = toproc_pipe_fd[1];
      int read_fd = fromproc_pipe_fd[0];
      write_file_ = fdopen(write_fd, "w");
      FILE* read_file = fdopen(read_fd, "r");
      if (write_file_ == NULL || read_file == NULL) {
        perror("fdopen");
        exit(1);
      }
      reader_.reset(new LineReader(read_file, false));
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
    fclose(write_file_);
  }

  std::string Demangle(const std::string& symbol) {
    const char *writeptr = symbol.c_str();
    const char *writeend = writeptr + symbol.size();

    while (writeptr < writeend) {
      size_t bytes = fwrite(writeptr, 1, writeend - writeptr, write_file_);
      if (bytes == 0) {
        perror("fread");
        exit(1);
      }
      writeptr += bytes;
    }
    if (fwrite("\n", 1, 1, write_file_) != 1) {
      perror("fwrite");
      exit(1);
    }
    if (fflush(write_file_) != 0) {
      perror("fflush");
      exit(1);
    }

    reader_->Next();
    return reader_->line();
  }

 private:
  FILE* write_file_;
  std::unique_ptr<LineReader> reader_;
  pid_t child_pid_;
};


/** Program data structures ***************************************************/

// Uses the simpler of the two algorithms described here:
//   https://www.cs.princeton.edu/courses/archive/fall03/cs528/handouts/a%20fast%20algorithm%20for%20finding.pdf
template <class T>
class DominatorCalculator {
 public:
  static void Calculate(T* root, uint32_t total) {
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
      info.node->dominator = calculator.node_info_[info.dom].node;
    }
  }

 private:
  void Initialize(T* pv) {
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

  void CalculateDominators(T* pr, uint32_t total) {
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
    T* node;
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

template <class T>
class WeightGraphPrinter {
 public:
  void Print(T* entry, uint32_t maxid) {
    DominatorCalculator<T>::Calculate(entry, maxid);
    std::set<T*> seen;
    CalculateWeightsRecursive(entry, &seen);
    std::ofstream out("/tmp/graph.dot");
    out << "digraph weights {\n";
    out << "  rankdir=LR;\n";
    /*
    for ( auto object : object_list) {
      out << "  \"" << object->name << "\"\n";
    }
    */
    {
      std::unordered_map<T*, std::unordered_set<T*>> dominating;
      for (auto obj : seen) {
        total_size_ += obj->size;
        if (obj->dominator) {
          //if (name_path && obj->dominator->name == *name_path) {
            //std::cerr << "Adding dominator: " << obj->name << " -> " << *name_path << "\n";
          //  std::cerr << "Adding dominator: " << obj->dominator->name << " -> " << obj->name << "\n";
          //}
          dominating[obj->dominator].insert(obj);
        }
      }

      PrintDotGraph(entry, &out, dominating);
    }
    out << "}\n";
  }

 private:
  size_t total_size_ = 0;

  uint32_t FontSize(size_t size) {
    return 9 + pow(size * 200.0 / total_size_, 0.7);
  }

  void PrintDotGraph(T* obj, std::ofstream* out,
      const std::unordered_map<T*, std::unordered_set<T*>>& dominating) {
    size_t other_size = 0;
    size_t max_other = 0;
    size_t other_count = 0;
    size_t unsummarized = 0;
    auto it = dominating.find(obj);
    if (it != dominating.end()) {
      const std::unordered_set<T*> direct = it->second;
      std::vector<T*> sorted;
      std::copy(direct.begin(), direct.end(),
                std::back_inserter(sorted));
      std::sort(sorted.begin(), sorted.end(), [&](T* a, T* b) {
        auto it_a = dominating.find(a);
        auto it_b = dominating.find(b);
        size_t children_a = (it_a == dominating.end()) ? 0 : it_a->second.size();
        size_t children_b = (it_b == dominating.end()) ? 0 : it_b->second.size();
        if (children_a != children_b) {
          return children_a > children_b;
        }
        return a->size > b->size;
      });
      for (size_t i = 0; i < sorted.size(); i++) {
        T* target = sorted[i];
        if ((static_cast<double>(target->weight) / total_size_) > 0.001) {
          unsummarized++;
          *out << "  \"" << obj->name << "\" -> \"" << target->name << "\";\n"; // [penwidth=" << (pow(obj->weight * 100.0 / max_weight_, 0.6)) << "];\n";
          PrintDotGraph(target, out, dominating);
        } else {
          other_count++;
          other_size += target->weight;
          max_other = std::max(target->weight, max_other);
        }
      }
    }

    if (unsummarized > 0) {
      if (other_size) {
        std::string other_name = obj->name + " OTHER";
        *out << "  \"" << obj->name << "\" -> \"" << other_name
             << "\";\n";  // [penwidth=" << (pow(obj->weight * 100.0 /
                          // max_weight_, 0.6)) << "];\n";
        *out << "  \"" << other_name << "\" [shape=box, label=\"["
             << other_count << " others under " << SiPrint(max_other)
             << "]\\nsize: " << SiPrint(other_size)
             << "\", fontsize=" << FontSize(other_size) << "];\n";
      }
      *out << "  \"" << obj->name << "\" [shape=box, label=\""
           << ClampSize(obj->pretty_name, 80)
           << "\\nsize: " << SiPrint(obj->size)
           << "\\nweight: " << SiPrint(obj->weight)
           << "\", fontsize=" << FontSize(obj->size) << "];\n";
    } else {
      size_t size = obj->size + other_size;
      std::string label = ClampSize(obj->pretty_name, 80);
      if (other_count > 0 && max_other > 0) {
        label += "\\nand " + std::to_string(other_count) + " children under " +
                 SiPrint(max_other);
      }
      *out << "  \"" << obj->name << "\" [shape=box, label=\"" << label
           << "\\nsize: " << SiPrint(size)
           << "\", fontsize=" << FontSize(obj->weight) << "];\n";
    }
  }

  void CalculateWeightsRecursive(T* obj, std::set<T*>* seen) {
    if (!seen->insert(obj).second) {
      return;
    }

    obj->weight = obj->size;
    obj->max_weight = obj->weight;

    for (auto target : obj->refs) {
      CalculateWeightsRecursive(target, seen);
      obj->max_weight = std::max(obj->max_weight, target->max_weight);
    }

    if (obj->dominator) {
      obj->dominator->weight += obj->weight;
    }
  }
};

class Program {
 public:
  Program() : no_file_("[couldn't resolve source filename]") {}
  Object* entry() { return entry_; }

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
    ret->file = &no_file_;
    no_file_.object_size += ret->size;
    total_size_ += size;
    objects_by_addr_.Add(vmaddr, size, ret);

    return ret;
  }

  void CalculatePrettyNames() {
    if (pretty_names_calculated_) {
      return;
    }

    for (auto& pair: objects_) {
      Object* obj = pair.second;
      auto demangled = demangler_.Demangle(obj->name);
      if (stripper_.StripName(demangled)) {
        auto it = stripped_pretty_names_.find(stripper_.stripped());
        if (it == stripped_pretty_names_.end()) {
          stripped_pretty_names_[stripper_.stripped()] = obj;
          obj->pretty_name = stripper_.stripped();
        } else {
          obj->pretty_name = demangled;
          if (it->second) {
            it->second->pretty_name = demangler_.Demangle(it->second->name);
            it->second = NULL;
          }
        }
      } else {
        obj->pretty_name = demangled;
      }
    }

    pretty_names_calculated_ = true;
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

  void PrintNoFile() {
    double total = 0;

    CalculatePrettyNames();

    std::vector<Object*> object_list;
    object_list.reserve(objects_.size());
    // XXX: should uniquify objects, can have dupes because of aliases.
    for ( auto& pair : objects_ ) {
      auto obj = pair.second;
      if (obj->file != &no_file_) {
        continue;
      }
      object_list.push_back(pair.second);
      //assert(pair.first == pair.second->name);
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

  void PrintWeightGraph() {
    WeightGraphPrinter<Object> printer;
    printer.Print(entry_, next_id_);
  }

  uint32_t next_id_ = 1;
  size_t total_size_ = 0;
  size_t max_weight_ = 0;

  // Files, indexed by filename.
  std::unordered_map<std::string, File> files_;

  // Objects, indexed by name.
  std::unordered_map<std::string, Object*> objects_;
  std::unordered_map<std::string, Object*> stripped_pretty_names_;
  RangeMap<Object*> objects_by_addr_;
  RangeMap<uintptr_t> file_offsets_;
  Object* entry_ = nullptr;
  bool pretty_names_calculated_ = false;

  NameStripper stripper_;
  Demangler demangler_;
  File no_file_;
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

class RuleMap {
 public:
  RuleMap() : next_id_(1) {}

  void Split(const std::string& str, std::string* part1, std::string* part2) {
    size_t n = str.find(": ");
    if (n == std::string::npos) {
      std::cout << "No separator?\n";
      exit(1);
    }
    part1->assign(str, 0, n);
    part2->assign(str, n + 2, std::string::npos);
  }

  Rule* GetOrCreateRule(const std::string& name) {
    // C++17: auto pair = rules_.try_emplace(filename, filename);
    auto it = rules_.find(name);
    if (it == rules_.end()) {
      it = rules_.emplace(name, name).first;
      it->second.id = next_id_++;
    }
    return &it->second;
  }

  Rule* GetRule(const std::string& name) {
    auto it = rules_.find(name);
    if (it == rules_.end()) {
      std::cerr << "No such rule: " << name << "\n";
      exit(1);
    }
    return &it->second;
  }

  void ReadMapFile(const std::string& filename) {
    std::ifstream input(filename);
    for (std::string line, part1, part2; std::getline(input, line); ) {
      if (line == "") {
        break;
      }
      Split(line, &part1, &part2);
      GetOrCreateRule(part2)->refs.insert(GetOrCreateRule(part1));
    }

    for (std::string line, part1, part2; std::getline(input, line); ) {
      Split(line, &part1, &part2);
      //source_files_[part1] = GetRule(part2);
      source_files_[part1] = GetOrCreateRule(part2);
    }
  }

  void AddFilesToRuleSizes(Program* program) {
    for (auto& pair : program->files_) {
      File* file = &pair.second;
      auto it = source_files_.find(file->name);
      if (it == source_files_.end()) {
        std::string fallback;
        if (TryGetFallbackFilename(file->name, &fallback)) {
          it = source_files_.find(fallback);
          if (it == source_files_.end()) {
            std::cerr << "No fallback source file in map: " << file->name << ", " << file->object_size << "\n";
            continue;
          }
        } else {
          std::cerr << "No source file in map: " << file->name << ", " << file->object_size << "\n";
          continue;
        }
      }
      Rule* rule = it->second;
      rule->size += file->object_size;
      file->rule = rule;
    }
  }

  size_t count() { return rules_.size(); }

  void PrintDepGraph() {
    std::ofstream out("deps.dot");
    out << "digraph deps {\n";
    for (const auto& pair : rules_) {
      const Rule* rule = &pair.second;
      for (auto ref : rule->refs) {
        out << "  \"" << rule->name << "\" -> \"" << ref->name << "\";\n";
      }
    }
    out << "}\n";
  }

  void PrintDomTree() {
    std::ofstream out("dom.dot");
    out << "digraph dom {\n";
    for (const auto& pair : rules_) {
      const Rule* rule = &pair.second;
      if (rule->dominator) {
        out << "  \"" << rule->dominator->name << "\" -> \"" << rule->name << "\";\n";
      }
    }
    out << "}\n";
  }

  void PrintWeightGraph(Rule* entry) {
    WeightGraphPrinter<Rule> printer;
    printer.Print(entry, next_id_);
  }

  void ParseLineInfo(const std::string& filename) {
    ParseELFLineInfo(filename, source_files_);
  }

 private:
  std::unordered_map<std::string, Rule> rules_;
  std::unordered_map<std::string, Rule*> source_files_;
  uint32_t next_id_;
};

void DoSymbolAnalysis(const std::string& filename) {
  Program program;
  ProgramDataSink sink(&program);

  const std::string build_id = ReadBuildId(filename);
  bool found_cache = TryOpenCacheFile(filename, build_id, &program);

  if (!found_cache) {
    ReadObjectData(filename, &sink);
    ParseVTables(filename, &program);
    program.PrintWeightGraph();
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

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: bloaty <binary file> <rule map>\n";
    exit(1);
  }

  const std::string bin_file = argv[1];
  const std::string rule_map = argv[2];

  RuleMap map;
  map.ReadMapFile(rule_map);

  Program program;
  ProgramDataSink sink(&program);

  ParseELFSymbols(bin_file, &sink);
  ParseELFSections(bin_file, &sink);
  //ParseELFDebugInfo(bin_file, &sink);
  bloaty::GetFunctionFilePairs(bin_file, &sink);
  ParseELFFileMapping(bin_file, &sink);

  Rule* entry = nullptr;
  if (program.entry()) {
    Object* obj_entry = program.entry();
    std::cerr << "Program entry point function: " << obj_entry->name << "\n";
    if (obj_entry->file) {
      File* file_entry = obj_entry->file;
      std::cerr << "Program entry point file: " << file_entry->name << "\n";
      if (file_entry->rule) {
        entry = file_entry->rule;
        std::cerr << "Rule entry point: " << entry->name << "\n";
      }
    }
  }

  if (!entry) {
    std::cerr << "Couldn't figure out entry.\n";
   // entry = map.GetRule("//superroot/servers:sr_www");
    entry = map.GetRule("//net/proto2/compiler/public:protocol_compiler");
  }
  //map.ParseLineInfo(bin_file);
  map.AddFilesToRuleSizes(&program);
  map.PrintWeightGraph(entry);
  map.PrintDepGraph();
  map.PrintDomTree();
  program.PrintNoFile();
}
