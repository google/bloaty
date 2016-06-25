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
#include <sstream>
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
//#include "base/init_google.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CHECK_SYSCALL(call) \
  if (call < 0) { \
    perror(#call " " __FILE__ ":" TOSTRING(__LINE__)); \
    exit(1); \
  }

namespace bloaty {

std::string* name_path;

std::string ClampSize(const std::string& input, size_t size) {
  if (input.size() < size) {
    return input;
  } else {
    return input.substr(0, size);
  }
}

std::string DoubleStringPrintf(const char *fmt, double d) {
  char buf[1024];
  snprintf(buf, sizeof(buf), fmt, d);
  return std::string(buf);
}

std::string SiPrint(size_t size) {
  const char *prefixes[] = {"", "k", "M", "G", "T"};
  int n = 0;
  double size_d = size;
  while (size_d > 1024) {
    size_d /= 1024;
    n++;
  }

  if (size_d > 100 || n == 0) {
    return std::to_string(static_cast<size_t>(size_d)) + prefixes[n];
  } else if (size_d > 10) {
    return DoubleStringPrintf("%0.1f", size_d) + prefixes[n];
  } else {
    return DoubleStringPrintf("%0.2f", size_d) + prefixes[n];
  }

}

// LineReader / LineIterator ///////////////////////////////////////////////////

// Convenience code for iterating over lines of a pipe.

LineReader::LineReader(LineReader&& other) {
  Close();

  file_ = other.file_;
  pclose_ = other.pclose_;

  other.file_ = nullptr;
}

void LineReader::Close() {
  if (!file_) return;

  if (pclose_) {
    pclose(file_);
  } else {
    fclose(file_);
  }
}

void LineReader::Next() {
  char buf[256];
  line_.clear();
  do {
    if (!fgets(buf, sizeof(buf), file_)) {
      if (feof(file_)) {
        eof_ = true;
        break;
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
LineIterator LineReader::end() { return LineIterator(nullptr); }

LineReader ReadLinesFromPipe(const std::string& cmd) {
  FILE* pipe = popen(cmd.c_str(), "r");

  if (!pipe) {
    std::cerr << "Failed to run command: " << cmd << "\n";
    exit(1);
  }

  return LineReader(pipe, true);
}


// NameStripper ////////////////////////////////////////////////////////////////

// C++ Symbol names can get really long because they include all the parameter
// types.  For example:
//
// bloaty::RangeMap::ComputeRollup(std::vector<bloaty::RangeMap const*, std::allocator<bloaty::RangeMap const*> > const&, bloaty::Rollup*)
//
// In most cases, we can strip all of the parameter info.  We only need to keep
// it in the case of overloaded functions.  This class can do the stripping, but
// the caller needs to verify that the stripped name is still unique within the
// binary, otherwise the full name should be used.

class NameStripper {
 public:
  bool StripName(const std::string& name) {
    // XXX: bloaty::(anonymous namespace)::ReadELFSegments(std::basic_string
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


// Demangler ///////////////////////////////////////////////////////////////////

// Demangles C++ symbols.
//
// There is no library we can (easily) link against for this, we have to shell
// out to the "c++filt" program
//
// We can't use LineReader or popen() because we need to both read and write to
// the subprocess.  So we need to roll our own.

class Demangler {
 public:
  Demangler();
  ~Demangler();

  std::string Demangle(const std::string& symbol);

 private:
  FILE* write_file_;
  std::unique_ptr<LineReader> reader_;
  pid_t child_pid_;
};

Demangler::Demangler() {
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
    if (write_file_ == nullptr || read_file == nullptr) {
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
    char *const argv[] = {prog, nullptr};
    CHECK_SYSCALL(execvp("c++filt", argv));
  }
}

Demangler::~Demangler() {
  int status;
  kill(child_pid_, SIGTERM);
  waitpid(child_pid_, &status, WEXITED);
  fclose(write_file_);
}

std::string Demangler::Demangle(const std::string& symbol) {
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

// Rollup //////////////////////////////////////////////////////////////////////

// A Rollup is a hierarchical tally of sizes.  Its graphical representation is
// something like this:
//
//  93.3%  93.3%   3.02M Unmapped
//      38.2%  38.2%   1.16M .debug_info
//      23.9%  62.1%    740k .debug_str
//      12.1%  74.2%    374k .debug_pubnames
//      11.7%  86.0%    363k .debug_loc
//       8.9%  94.9%    275k [Other]
//       5.1% 100.0%    158k .debug_ranges
//   6.7% 100.0%    222k LOAD [R E]
//      61.0%  61.0%    135k .text
//      21.4%  82.3%   47.5k .rodata
//       6.2%  88.5%   13.8k .gcc_except_table
//       5.9%  94.4%   13.2k .eh_frame
//       5.6% 100.0%   12.4k [Other]
//   0.0% 100.0%   1.40k [Other]
// 100.0%   3.24M TOTAL
//
// There is a string -> size map of sizes (the meaning of the string labels
// depends on context; it can by symbols, sections, source filenames, etc.) Each
// map entry can have its own sub-Rollup.

class Rollup {
 public:
  // Adds "size" bytes to the rollup under the label names[i].
  // If there are more entries names[i+1, i+2, etc] add them to sub-rollups.
  void Add(const std::vector<std::string> names, size_t i, size_t size) {
    total_ += size;
    if (i < names.size()) {
      auto& child = children_[names[i]];
      if (child.get() == nullptr) {
        child.reset(new Rollup());
      }
      child->Add(names, i + 1, size);
    }
  }

  // Prints a graphical representation of the rollup.
  void Print() const {
    PrintInternal("");
  }

 private:
  void PrintInternal(const std::string& indent) const;
  size_t total_ = 0;

  // Putting Rollup by value seems to work on some compilers/libs but not
  // others.
  typedef std::unordered_map<std::string, std::unique_ptr<Rollup>> ChildMap;
  ChildMap children_;
};

void Rollup::PrintInternal(const std::string& indent) const {
  if (children_.empty() ||
      (children_.size() == 1 && children_.begin()->first == "[None]")) {
    return;
  }

  double total = 0;

  typedef std::tuple<const std::string*, size_t, const Rollup*> ChildTuple;
  std::vector<ChildTuple> sorted_children;
  sorted_children.reserve(children_.size());
  size_t others = 0;
  size_t others_max = 0;
  for (const auto& value : children_) {
    sorted_children.push_back(
        std::make_tuple(&value.first, value.second->total_, value.second.get()));
    total += value.second->total_;
  }

  assert(total == total_);

  std::sort(sorted_children.begin(), sorted_children.end(),
            [](const ChildTuple& a, const ChildTuple& b) {
              return std::get<1>(a) > std::get<1>(b);
            });

  size_t i = sorted_children.size() - 1;
  while (i >= 20) {
    if (*std::get<0>(sorted_children[i]) == "[None]") {
      std::swap(sorted_children[i], sorted_children[19]);
    } else {
      size_t size = std::get<1>(sorted_children[i]);
      others += size;
      others_max = std::max(others_max, size);
      sorted_children.resize(i);
      i--;
    }
  }

  std::string others_label = "[Other]";
  if (others > 0) {
    sorted_children.push_back(std::make_tuple(&others_label, others, nullptr));
  }

  std::sort(sorted_children.begin(), sorted_children.end(),
            [](const ChildTuple& a, const ChildTuple& b) {
              return std::get<1>(a) > std::get<1>(b);
            });

  size_t cumulative = 0;
  NameStripper stripper;
  Demangler demangler;

  for (const auto& child : sorted_children) {
    size_t size = std::get<1>(child);
    cumulative += size;
    //const std::string& name = object->pretty_name;
    //printf("%s %5.1f%% %5.1f%%  %6d %s\n", indent.c_str(), size / total * 100,
    //       cumulative / total * 100, (int)size, child->first.c_str());
    auto demangled = demangler.Demangle(*std::get<0>(child));
    stripper.StripName(demangled);
    printf("%s %5.1f%%  %6s %s\n", indent.c_str(), size / total * 100,
           SiPrint(size).c_str(), stripper.stripped().c_str());
    auto child_rollup = std::get<2>(child);
    if (child_rollup) {
      child_rollup->PrintInternal(indent + "    ");
    }
  }

  if (indent == "") {
    //printf("%s %5.1f%%  %6d %s\n", indent.c_str(), 100.0, (int)total, "TOTAL");
    printf("%s %5.1f%%  %6s %s\n", indent.c_str(), 100.0, SiPrint(total).c_str(), "TOTAL");
  }
}

// RangeMap ////////////////////////////////////////////////////////////////////

// Maps
//
//   [uintptr_t, uintptr_t) -> std::string
//
// where ranges must be non-overlapping.
//
// This is used to map the address space (either pointer offsets or file
// offsets).

class RangeMap {
 public:
  bool Add(uintptr_t addr, size_t size, const std::string& val);
  std::string* Get(uintptr_t addr, uintptr_t* start, size_t* size);
  std::string* TryGet(uintptr_t addr, uintptr_t* start, size_t* size);
  std::string* TryGetExactly(uintptr_t addr, size_t* size);

  void Fill(uintptr_t max, const std::string& val);

  static void ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                            Rollup* rollup);

 private:
  typedef std::map<uintptr_t, std::pair<std::string, size_t>> Map;
  Map mappings_;

  static size_t RangeEnd(Map::const_iterator iter) {
    return iter->first + iter->second.second;
  }

  bool IterIsEnd(Map::const_iterator iter) const {
    return iter == mappings_.end();
  }
};

std::string* RangeMap::TryGet(uintptr_t addr, uintptr_t* start, size_t* size) {
  auto it = mappings_.upper_bound(addr);
  if (it == mappings_.begin() || (--it, it->first + it->second.second <= addr)) {
    return nullptr;
  }
  assert(addr >= it->first && addr < it->first + it->second.second);
  return &it->second.first;
}

std::string* RangeMap::TryGetExactly(uintptr_t addr, size_t* size) {
  auto it = mappings_.find(addr);
  if (it == mappings_.end()) {
    return nullptr;
  }
  return &it->second.first;
}

bool RangeMap::Add(uintptr_t addr, size_t size, const std::string& val) {
  if (TryGet(addr, nullptr, nullptr) ||
      TryGet(addr + size, nullptr, nullptr)) {
    return false;
  }
  mappings_[addr] = std::make_pair(std::move(val), size);
  return true;
}

void RangeMap::Fill(uintptr_t max, const std::string& val) {
  uintptr_t last = 0;

  for (const auto& pair : mappings_) {
    if (pair.first > last) {
      mappings_[last] = std::make_pair(val, pair.first - last);
    }

    last = pair.first + pair.second.second;
  }

  if (last < max) {
    mappings_[last] = std::make_pair(val, max - last);
  }
}

void RangeMap::ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                             Rollup* rollup) {
  assert(range_maps.size() > 0);

  std::vector<Map::const_iterator> iters;
  std::vector<std::string> keys;
  uintptr_t current = UINTPTR_MAX;

  for (auto range_map : range_maps) {
    iters.push_back(range_map->mappings_.begin());
    current = std::min(current, iters.back()->first);
  }

  assert(current != UINTPTR_MAX);

  while (true) {
    uintptr_t next_break = UINTPTR_MAX;
    bool have_data = false;
    keys.clear();

    for (size_t i = 0; i < iters.size(); i++) {
      auto& iter = iters[i];

      // Advance the iterators if its range is behind the current point.
      while (!range_maps[i]->IterIsEnd(iter) && RangeEnd(iter) <= current) {
        ++iter;
        //assert(range_maps[i]->IterIsEnd(iter) || RangeEnd(iter) > current);
      }

      // Push a label and help calculate the next break.
      bool is_end = range_maps[i]->IterIsEnd(iter);
      if (is_end || iter->first > current) {
        keys.push_back("[None]");
        if (!is_end) {
          next_break = std::min(next_break, iter->first);
        }
      } else {
        have_data = true;
        keys.push_back(iter->second.first);
        next_break = std::min(next_break, RangeEnd(iter));
      }
    }

    if (next_break == UINTPTR_MAX) {
      break;
    }

    if (false) {
      for (auto& key : keys) {
        if (key == "[None]") {
          std::stringstream stream;
          stream << " [0x" << std::hex << current << ", 0x" << std::hex
                 << next_break << "]";
          key += stream.str();
        }
      }
    }

    if (have_data) {
      rollup->Add(keys, 0, next_break - current);
    }

    current = next_break;
  }
}


// MemoryMap ///////////////////////////////////////////////////////////////////

MemoryMap::MemoryMap(const MemoryFileMap* base)
    : base_(base), vm_map_(new RangeMap()) {}

MemoryMap::~MemoryMap() {}

void MemoryMap::AddRegex(const std::string& regex, const std::string& replacement) {
  std::unique_ptr<RE2> re2(new RE2(regex));
  regexes_.push_back(std::make_pair(std::move(re2), replacement));
}

void MemoryMap::AddVMRange(uintptr_t vmaddr, size_t size,
                           const std::string& name) {
  std::string str = name;
  std::string tmp;

  for (const auto& pair : regexes_) {
    if (RE2::Extract(str, *pair.first, pair.second, &tmp)) {
      str.swap(tmp);
    }
  }

  vm_map_->Add(vmaddr, size, str);
}

void MemoryMap::AddVMRangeAllowAlias(uintptr_t vmaddr, size_t size,
                                     const std::string& name) {
  AddVMRange(vmaddr, size, name);
}

bool MemoryMap::CoversVMAddresses(uintptr_t vmaddr, size_t vmsize) const {
  // XXX: not correct, need to test the whole range.
  return vm_map_->TryGet(vmaddr, nullptr, nullptr) &&
         vm_map_->TryGet(vmaddr + vmsize, nullptr, nullptr);
}

/*
bool FindAtAddr(uintptr_t vmaddr, std::string* name) const;
bool FindContainingAddr(uintptr_t vmaddr, uintptr_t* start,
                        std::string* name) const;
                        */


// MemoryFileMap ///////////////////////////////////////////////////////////////

MemoryFileMap::MemoryFileMap(MemoryFileMap* base)
    : MemoryMap(base), file_map_(new RangeMap()) {}
MemoryFileMap::~MemoryFileMap() {}

void MemoryFileMap::FillInUnmapped(long filesize) {
  file_map_->Fill(filesize, "Unmapped");
}

void MemoryFileMap::AddFileRange(const std::string& name, uintptr_t vmaddr,
                                 size_t vmsize, long fileoff, long filesize) {
  if (base()) {
    if (!base()->CoversVMAddresses(vmaddr, vmsize) ||
        !base()->CoversFileOffsets(fileoff, filesize)) {
      std::cerr << "bloaty: section " << name << " lies outside any segment.\n";
      return;
    }
  }

  bool overlapped = false;

  AddVMRange(vmaddr, vmsize, name);

  if (filesize) {
    if (!file_map_->Add(fileoff, filesize, name)) {
      overlapped = true;
    }
  }

  if (overlapped) {
    std::cerr << "Unexpected overlap in base memory map adding: " << name
              << "\n";
  }
}

bool MemoryFileMap::CoversFileOffsets(uintptr_t fileoff,
                                      size_t filesize) const {
  // XXX: not correct, need to test the whole range.
  return file_map_->TryGet(fileoff, nullptr, nullptr) &&
         file_map_->TryGet(fileoff + filesize, nullptr, nullptr);
}


#if 0
class Program {
 public:
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
#endif


long GetFileSize(const std::string& filename) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Couldn't get file size for: " << filename << "\n";
    exit(1);
  }
  fseek(file, 0L, SEEK_END);
  long ret = ftell(file);
  fclose(file);
  return ret;
}

}  // namespace bloaty

using namespace bloaty;

int main(int argc, char *argv[]) {
  const std::string filename(argv[1]);
  /*
  std::vector<const RangeMap*> range_maps;
  std::vector<std::unique_ptr<MemoryFileMap>> mem_file_maps;
  std::vector<std::unique_ptr<MemoryMap>> mem_maps;
  const MemoryFileMap* base = nullptr;
  */

  for (int i = 1; i < argc; i++) {

  }

  MemoryFileMap segments, sections;
  MemoryMap symbols(&segments);
  MemoryMap symbols2(&segments);
  symbols.AddRegex("MergePartialFromCodedStream", "Protobuf MergePartialFromCodedStream");
  symbols.AddRegex("ByteSize", "Protobuf ByteSize");
  symbols.AddRegex("SerializeWithCachedSizes", "Protobuf SerializeWithCachedSizes");
  ReadSegments(filename, &segments);
  ReadSections(filename, &sections);
  ReadSymbols(filename, &symbols);
  ReadDWARFSourceFiles(filename, &symbols2);
  ReadDWARFLineInfo(filename, &symbols);
  segments.FillInUnmapped(GetFeleSize(filename));
  Rollup rollup;
  //range_maps.push_back(segments.vm_map());
  range_maps.push_back(sections.vm_map());
  range_maps.push_back(symbols2.map());
  range_maps.push_back(symbols.map());

  //range_maps.push_back(segments.file_map());
  //range_maps.push_back(sections.file_map());
  //
  RangeMap::ComputeRollup(range_maps, &rollup);
  rollup.Print();
  return 0;
}
