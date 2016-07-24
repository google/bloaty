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
const size_t kMaxLabelLen = 80;

void PrintSpaces(size_t n) {
  for (size_t i = 0; i < n; i++) {
    printf(" ");
  }
}

double Percent(size_t part, size_t whole) {
  if (whole == 0) {
    return 0;
  } else {
    return static_cast<double>(part) / whole * 100;
  }
}

std::string FixedWidthString(const std::string& input, size_t size) {
  if (input.size() < size) {
    std::string ret = input;
    while (ret.size() < size) {
      ret += " ";
    }
    return ret;
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

void Split(const std::string& str, char delim, std::vector<std::string>* out) {
  std::stringstream stream(str);
  std::string item;
  while (std::getline(stream, item, delim)) {
    out->push_back(item);
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

std::string others_label = "[Other]";

class Rollup {
 public:
  Rollup() {}

  void AddSizes(const std::vector<std::string> names,
                size_t size, bool is_vmsize) {
    AddInternal(names, 0, size, is_vmsize);
  }

  // Prints a graphical representation of the rollup.
  void Print() const {
    RollupRow row(nullptr);
    row.vmsize = vm_total_;
    row.filesize = file_total_;
    size_t longest_label = 0;
    ComputeRows(0, &row, &longest_label);
    row.Print(0, longest_label);
  }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Rollup);

  size_t vm_total_ = 0;
  size_t file_total_ = 0;

  // Putting Rollup by value seems to work on some compilers/libs but not
  // others.
  typedef std::unordered_map<std::string, std::unique_ptr<Rollup>> ChildMap;
  ChildMap children_;

  // Adds "size" bytes to the rollup under the label names[i].
  // If there are more entries names[i+1, i+2, etc] add them to sub-rollups.
  void AddInternal(const std::vector<std::string> names, size_t i,
                   size_t size, bool is_vmsize) {
    if (is_vmsize) {
      vm_total_ += size;
    } else {
      file_total_ += size;
    }
    if (i < names.size()) {
      auto& child = children_[names[i]];
      if (child.get() == nullptr) {
        child.reset(new Rollup());
      }
      child->AddInternal(names, i + 1, size, is_vmsize);
    }
  }

  struct RollupRow {
    RollupRow(const std::string* name_) : name(name_) {}
    RollupRow(const ChildMap::value_type& value)
        : name(&value.first),
          vmsize(value.second->vm_total_),
          filesize(value.second->file_total_) {}

    const std::string* name;
    size_t vmsize = 0;
    size_t filesize = 0;
    std::vector<RollupRow> sorted_children;

    void Print(size_t indent, size_t longest_label) const;
  };

  void ComputeRows(size_t indent, RollupRow* row, size_t* longest_label) const;
};

void Rollup::ComputeRows(size_t indent, RollupRow* row,
                         size_t* longest_label) const {
  auto& child_rows = row->sorted_children;
  child_rows.reserve(children_.size());

  if (children_.empty() ||
      (children_.size() == 1 &&
       children_.begin()->first == "[None]")) {
    return;
  }

  for (const auto& value : children_) {
    child_rows.push_back(RollupRow(value));
  }

  std::sort(child_rows.begin(), child_rows.end(), [](const RollupRow& a,
                                                     const RollupRow& b) {
    return std::max(a.vmsize, a.filesize) > std::max(b.vmsize, b.filesize);
  });

  // Filter out everything but the top 20.
  RollupRow others(&others_label);

  size_t i = child_rows.size() - 1;
  while (i >= 20) {
    if (*child_rows[i].name == "[None]") {
      // Don't collapse [None] into [Other].
      std::swap(child_rows[i], child_rows[19]);
    } else {
      others.vmsize += child_rows[i].vmsize;
      others.filesize += child_rows[i].filesize;
      child_rows.erase(child_rows.end() - 1);
      i--;
    }
  }

  if (others.vmsize > 0 || others.filesize > 0) {
    child_rows.push_back(others);
  }

  // Put [Other] in the right place.
  std::sort(child_rows.begin(), child_rows.end(), [](const RollupRow& a,
                                                     const RollupRow& b) {
    return std::max(a.vmsize, a.filesize) > std::max(b.vmsize, b.filesize);
  });

  Demangler demangler;
  NameStripper stripper;
  for (auto& child_row : child_rows) {
    if (*child_row.name == others_label) {
      continue;
    }

    auto it = children_.find(*child_row.name);
    if (it == children_.end()) {
      std::cerr << "Should never happen, couldn't find name: "
                << *child_row.name << "\n";
      exit(1);
    }


    auto demangled = demangler.Demangle(*child_row.name);
    stripper.StripName(demangled);
    size_t allowed_label_len = std::min(stripper.stripped().size(), kMaxLabelLen);
    *longest_label = std::max(*longest_label, allowed_label_len + indent);
    it->second->ComputeRows(indent + 4, &child_row, longest_label);
  }
}

void Rollup::RollupRow::Print(size_t indent, size_t longest_label) const {
  NameStripper stripper;
  Demangler demangler;

  if (indent == 0) {
    printf("     VM SIZE    ");
    PrintSpaces(longest_label);
    printf("    FILE SIZE   ");
    printf("\n");
    printf(" -------------- ");
    PrintSpaces(longest_label);
    printf(" -------------- ");
    printf("\n");
  }

  for (const auto& child : sorted_children) {
    auto demangled = demangler.Demangle(*child.name);
    stripper.StripName(demangled);
    printf("%s %5.1f%%  %6s %s %6s  %5.1f%%\n", FixedWidthString("", indent).c_str(),
           Percent(child.vmsize, vmsize), SiPrint(child.vmsize).c_str(),
           FixedWidthString(stripper.stripped(), longest_label).c_str(),
           SiPrint(child.filesize).c_str(), Percent(child.filesize, filesize));
    child.Print(indent + 4, longest_label);
  }

  if (indent == 0) {
    printf("%s %5.1f%%  %6s %s %6s  %5.1f%%\n",
           FixedWidthString("", indent).c_str(), 100.0, SiPrint(vmsize).c_str(),
           FixedWidthString("TOTAL", longest_label).c_str(),
           SiPrint(filesize).c_str(), 100.0);
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
  bool AddRange(uintptr_t addr, size_t size, const std::string& val);
  const std::string* Get(uintptr_t addr, uintptr_t* start, size_t* size) const;
  const std::string* TryGet(uintptr_t addr, uintptr_t* start,
                            size_t* size) const;
  const std::string* TryGetExactly(uintptr_t addr, size_t* size) const;

  void Fill(uintptr_t max, const std::string& val);

  static void ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                            bool is_vmsize, Rollup* rollup);

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

const std::string* RangeMap::TryGet(uintptr_t addr, uintptr_t* start,
                                    size_t* size) const {
  auto it = mappings_.upper_bound(addr);
  if (it == mappings_.begin() ||
      (--it, it->first + it->second.second <= addr)) {
    return nullptr;
  }
  assert(addr >= it->first && addr < it->first + it->second.second);
  if (start) *start = it->first;
  if (size) *size = it->second.second;
  return &it->second.first;
}

const std::string* RangeMap::TryGetExactly(uintptr_t addr, size_t* size) const {
  auto it = mappings_.find(addr);
  if (it == mappings_.end()) {
    return nullptr;
  }
  if (size) *size = it->second.second;
  return &it->second.first;
}

bool RangeMap::AddRange(uintptr_t addr, size_t size, const std::string& val) {
  // XXX: properly test the whole range, not just the two endpoints.
  if (TryGet(addr, nullptr, nullptr) ||
      TryGet(addr + size - 1, nullptr, nullptr)) {
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
                             bool is_vmsize, Rollup* rollup) {
  assert(range_maps.size() > 0);

  std::vector<Map::const_iterator> iters;
  std::vector<std::string> keys;
  uintptr_t current = UINTPTR_MAX;

  for (auto range_map : range_maps) {
    iters.push_back(range_map->mappings_.begin());
    current = std::min(current, iters.back()->first);
  }

  assert(current != UINTPTR_MAX);

  // Iterate over all ranges in parallel to perform this transformation:
  //
  //   -----  -----  -----             ---------------
  //     |      |      1                    A,X,1
  //     |      X    -----             ---------------
  //     |      |      |                    A,X,2
  //     A    -----    |               ---------------
  //     |      |      |                      |
  //     |      |      2      ----->          |
  //     |      Y      |                    A,Y,2
  //     |      |      |                      |
  //   -----    |      |               ---------------
  //     B      |      |                    B,Y,2
  //   -----    |    -----             ---------------
  //            |                      [None],Y,[None]
  //          -----
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
      rollup->AddSizes(keys, next_break - current, is_vmsize);
    }

    current = next_break;
  }
}


// MemoryMap ///////////////////////////////////////////////////////////////////

// Contains a [range] -> label map for VM space and file space.
class MemoryMap {
 public:
  MemoryMap() {}
  virtual ~MemoryMap() {}

  // For the file space mapping, Fills in generic "Unmapped" sections for
  // portions of the file that have no mapping.
  void FillInUnmapped(long filesize);

  // Adds a regex that will be applied to all labels prior to inserting them in
  // the map.  All regexes will be applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);

  bool FindAtAddr(uintptr_t vmaddr, std::string* name) const;
  bool FindContainingAddr(uintptr_t vmaddr, uintptr_t* start,
                          std::string* name) const;

  const RangeMap* file_map() const { return &file_map_; }
  const RangeMap* vm_map() const { return &vm_map_; }
  RangeMap* file_map() { return &file_map_; }
  RangeMap* vm_map() { return &vm_map_; }

 protected:
  std::string ApplyNameRegexes(const std::string& name);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MemoryMap);
  friend class VMRangeSink;

  RangeMap vm_map_;
  RangeMap file_map_;
  std::unordered_map<std::string, std::string> aliases_;
  std::vector<std::pair<std::unique_ptr<RE2>, std::string>> regexes_;
};

void MemoryMap::FillInUnmapped(long filesize) {
  file_map_.Fill(filesize, "[Unmapped]");
}

void MemoryMap::AddRegex(const std::string& regex, const std::string& replacement) {
  std::unique_ptr<RE2> re2(new RE2(regex));
  regexes_.push_back(std::make_pair(std::move(re2), replacement));
}

std::string MemoryMap::ApplyNameRegexes(const std::string& name) {
  std::string ret = name;

  for (const auto& pair : regexes_) {
    if (RE2::Replace(&ret, *pair.first, pair.second)) {
      break;
    }
  }

  return ret;
}

/*
bool FindAtAddr(uintptr_t vmaddr, std::string* name) const;
bool FindContainingAddr(uintptr_t vmaddr, uintptr_t* start,
                        std::string* name) const;
                        */


// MemoryFileMap ///////////////////////////////////////////////////////////////

// A MemoryMap for things like segments and sections, where every range exists
// in both VM space and file space.  We can use MemoryFileMaps to translate VM
// addresses into file offsets.
class MemoryFileMap : public MemoryMap {
 public:
  MemoryFileMap() {}
  virtual ~MemoryFileMap() {}

  void AddRange(const std::string& name, uintptr_t vmaddr, size_t vmsize,
                long fileoff, long filesize);

  // Translates a VM address to a file offset.  Returns false if this VM address
  // is not mapped from the file.
  bool TranslateVMAddress(uintptr_t vmaddr, uintptr_t* fileoff) const;

  // Translates a VM address range to a file offset range.  Returns false if
  // nothing in this VM address range is mapped into a file.
  //
  // This VM address range may not translate to multiple discrete file ranges.
  // We generally would never expect this to happen.
  bool TranslateVMRange(uintptr_t vmaddr, size_t vmsize,
                        uintptr_t* fileoff, uintptr_t* filesize) const;

 private:
  // Maps each vm_map_ start address to a corresponding file_map_ start address.
  // If a given vm_map_ start address is missing from the map, it does not come
  // from the file.
  std::unordered_map<uintptr_t, uintptr_t> vm_to_file_;
};

void MemoryFileMap::AddRange(const std::string& name,
                             uintptr_t vmaddr, size_t vmsize,
                             long fileoff, long filesize) {
  std::string label = ApplyNameRegexes(name);
  if ((vmsize > 0 && !vm_map()->AddRange(vmaddr, vmsize, label)) ||
      (filesize > 0 && !file_map()->AddRange(fileoff, filesize, label))) {
    std::cerr << "bloaty: unexpected overlap adding name '" << name << "'\n";
    return;
  }

  if (vmsize > 0 && filesize > 0) {
    vm_to_file_[vmaddr] = fileoff;
  }
}

bool MemoryFileMap::TranslateVMAddress(uintptr_t vmaddr,
                                       uintptr_t* fileoff) const {
  uintptr_t vmstart;
  if (!vm_map()->TryGet(vmaddr, &vmstart, nullptr)) {
    return false;
  }

  auto it = vm_to_file_.find(vmstart);
  if (it == vm_to_file_.end()) {
    return false;
  }

  uintptr_t filestart = it->second;
  uintptr_t filesize;
  if (!file_map()->TryGetExactly(filestart, &filesize)) {
    std::cerr << "Fatal error, should never happen.\n";
    exit(1);
  }

  if (vmaddr - vmstart > filesize) {
    // File mapping is shorter than VM mapping and doesn't actually contain our
    // address.
    return false;
  }

  *fileoff = (vmaddr - vmstart) + filestart;
  return true;
}

bool MemoryFileMap::TranslateVMRange(uintptr_t vmaddr, size_t vmsize,
                                     uintptr_t* fileoff,
                                     uintptr_t* filesize) const {
  uintptr_t vm_range_start, vm_range_size;
  if (!vm_map()->TryGet(vmaddr, &vm_range_start, &vm_range_size)) {
    return false;
  }

  if (vmaddr + vmsize > vm_range_start + vm_range_size) {
    std::cerr << "Tried to translate range that spanned regions of of our "
              << "mapping.  This shouldn't happen.\n";
    exit(1);
  }

  auto it = vm_to_file_.find(vm_range_start);
  if (it == vm_to_file_.end()) {
    return false;
  }

  uintptr_t file_range_start = it->second;
  uintptr_t file_range_size;
  if (!file_map()->TryGetExactly(file_range_start, &file_range_size)) {
    std::cerr << "Fatal error, should never happen.\n";
    exit(1);
  }

  if (vmaddr - vm_range_start > file_range_size) {
    // File mapping is shorter than VM mapping and doesn't actually contain our
    // address.
    return false;
  }

  *fileoff = (vmaddr - vm_range_start) + file_range_start;
  *filesize = std::min(vmsize, file_range_size - (vmsize - vm_range_start));
  return true;
}


// VMRangeSink ////////////////////////////////////////////////////////////////

// Adds a region to the memory map.  It should not overlap any previous
// region added with Add(), but it should overlap the base memory map.
void VMRangeSink::AddVMRange(uintptr_t vmaddr, size_t vmsize,
                             const std::string& name) {
  if (vmsize == 0) {
    return;
  }
  const std::string label = map_->ApplyNameRegexes(name);
  uintptr_t fileoff, filesize;
  if (!map_->vm_map()->AddRange(vmaddr, vmsize, label)) {
    std::cerr << "Error adding range to VM map for name: " << name << "=["
              << std::hex << vmaddr << ", " << std::hex << vmsize << "]\n";
    uintptr_t vmstart, vm_region_size;
    auto existing = map_->vm_map()->TryGet(vmaddr, &vmstart, &vm_region_size);
    if (!existing) {
      existing = map_->vm_map()->TryGet(vmaddr + vmsize - 1, &vmstart, &vm_region_size);
    }
    if (!existing) {
      std::cerr << "WTF?????????????????????????\n";
    }
    std::cerr << "Existing mapping: " << *existing << "=[" << vmstart << ", " << vm_region_size<< "]\n";
  }
  if (translator_->TranslateVMRange(vmaddr, vmsize, &fileoff, &filesize)) {
    if (!map_->file_map()->AddRange(fileoff, filesize, label)) {
      std::cerr << "Error adding range to file map for name: " << name << "\n";
    }
  }
}

void VMRangeSink::AddVMRangeAllowAlias(uintptr_t vmaddr, size_t size,
                                       const std::string& name) {
  size_t existing_size;
  const auto existing = map_->vm_map()->TryGetExactly(vmaddr, &existing_size);
  if (existing) {
    if (size != existing_size) {
      std::cerr << "bloaty: Warning: inexact alias for name: " << name
                << ", aliases=" << *existing << "\n";
    }
    map_->aliases_.insert(std::make_pair(name, *existing));
  } else {
    AddVMRange(vmaddr, size, name);
  }
}

void VMRangeSink::AddVMRangeIgnoreDuplicate(uintptr_t vmaddr, size_t vmsize,
                                            const std::string& name) {
  if (vmsize == 0) {
    return;
  }

  const std::string label = map_->ApplyNameRegexes(name);
  uintptr_t fileoff, filesize;
  map_->vm_map()->AddRange(vmaddr, vmsize, label);
  if (translator_->TranslateVMRange(vmaddr, vmsize, &fileoff, &filesize)) {
    map_->file_map()->AddRange(fileoff, filesize, label);
  }
}


// VMFileRangeSink /////////////////////////////////////////////////////////////

void VMFileRangeSink::AddRange(const std::string& name, uintptr_t vmaddr,
                               size_t vmsize, long fileoff, long filesize) {
  map_->AddRange(name, vmaddr, vmsize, fileoff, filesize);
}

// Bloaty //////////////////////////////////////////////////////////////////////

// Represents a program execution and associated state.

class Bloaty {
 public:
  Bloaty();

  void SetFilename(const std::string& filename);
  void AddDataSource(const std::string& name);
  MemoryMap* FindMap(const std::string& name) const;

  void ScanDataSources();

  void GetRangeMaps(std::vector<const RangeMap*>* maps, bool is_vm);
  size_t GetSourceCount() const { return sources_.size(); }

 private:
  static long GetFileSize(const std::string& filename);

  std::map<std::string, DataSource> sources_map_;
  MemoryFileMap segment_map_;
  std::vector<std::unique_ptr<MemoryMap>> maps_;
  std::vector<const DataSource*> sources_;
  std::map<std::string, MemoryMap*> maps_by_name_;
  std::string filename_;
};

void Bloaty::GetRangeMaps(std::vector<const RangeMap*>* maps, bool is_vm) {
  maps->clear();
  for (const auto& map : maps_) {
    if (is_vm) {
      maps->push_back(map->vm_map());
    } else {
      maps->push_back(map->file_map());
    }
  }
}

long Bloaty::GetFileSize(const std::string& filename) {
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


Bloaty::Bloaty() {
  std::vector<DataSource> all_sources;
#ifdef __APPLE__
  RegisterMachODataSources(&all_sources);
#else
  RegisterELFDataSources(&all_sources);
#endif

  for (const auto& source : all_sources) {
    if (!sources_map_.insert(std::make_pair(source.name, source)).second) {
      std::cerr << "Two data sources registered for name: "
                << source.name << "\n";
      exit(1);
    }
  }
}

void Bloaty::SetFilename(const std::string& filename) {
  if (!filename_.empty()) {
    std::cerr << "Only one filename can be specified.\n";
    exit(1);
  }
  filename_ = filename;
}

void Bloaty::AddDataSource(const std::string& name) {
  auto it = sources_map_.find(name);
  if (it == sources_map_.end()) {
    std::cerr << "bloaty: no such data source: " << name << "\n";
    exit(1);
  }

  sources_.push_back(&it->second);
  switch (it->second.type) {
    case DataSource::DATA_SOURCE_VM_RANGE: {
      std::unique_ptr<MemoryMap> map(new MemoryMap());
      maps_by_name_[name] = map.get();
      maps_.push_back(std::move(map));
      break;
    }

    case DataSource::DATA_SOURCE_VM_FILE_RANGE: {
      std::unique_ptr<MemoryFileMap> map(new MemoryFileMap());
      maps_by_name_[name] = map.get();
      maps_.push_back(std::move(map));
      break;
    }

    default:
      std::cerr << "bloaty: unknown source type?\n";
      exit(1);
  }
}

MemoryMap* Bloaty::FindMap(const std::string& name) const {
  auto it = maps_by_name_.find(name);
  if (it != maps_by_name_.end()) {
    return it->second;
  } else {
    return NULL;
  }
}

void Bloaty::ScanDataSources() {
  // Always scan segments first (even if we're not displaying it) because we use
  // it for VM -> File address translation.
  auto it = sources_map_.find("segments");
  if (it == sources_map_.end() ||
      it->second.type != DataSource::DATA_SOURCE_VM_FILE_RANGE) {
    std::cerr << "bloaty: no segments data source, can't translate VM->File.\n";
    exit(1);
  }
  VMFileRangeSink segment_sink(&segment_map_);
  it->second.func.vm_file_range(filename_, &segment_sink);

  for (size_t i = 0; i < sources_.size(); i++) {
    switch (sources_[i]->type) {
      case DataSource::DATA_SOURCE_VM_RANGE: {
        VMRangeSink sink(maps_[i].get(), &segment_map_);
        sources_[i]->func.vm_range(filename_, &sink);
        break;
      }

      case DataSource::DATA_SOURCE_VM_FILE_RANGE: {
        auto map = static_cast<MemoryFileMap*>(maps_[i].get());
        VMFileRangeSink sink(map);
        sources_[i]->func.vm_file_range(filename_, &sink);
        if (i == 0) {
          map->FillInUnmapped(GetFileSize(filename_));
        }
        break;
      }

      default:
        std::cerr << "bloaty: unknown source type?\n";
        exit(1);
    }
  }
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


}  // namespace bloaty

using namespace bloaty;

int main(int argc, char *argv[]) {
  bloaty::Bloaty bloaty;

  RE2 regex_pattern("(\\w+)\\+=/(.*)/(.*)/");

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0) {
      std::vector<std::string> names;
      Split(argv[++i], ',', &names);
      for (const auto& name : names) {
        bloaty.AddDataSource(name);
      }
    } else if (strcmp(argv[i], "-r") == 0) {
      std::string source, regex, substitution;
      if (!RE2::FullMatch(argv[++i], regex_pattern,
                          &source, &regex, &substitution)) {
        std::cerr << "Bad format for regex, should be: "
                  << "source=~/pattern/replacement/\n";
        exit(1);
      }

      auto map = bloaty.FindMap(source);
      if (!map) {
        std::cerr << "Data source '" << source << "' not found in previous "
                  << "-d option\n";
        exit(1);
      }

      map->AddRegex(regex, substitution);
    } else if (argv[i][0] == '-') {
      std::cerr << "Unknown option: " << argv[i] << "\n";
      exit(1);
    } else {
      bloaty.SetFilename(argv[i]);
    }
  }

  if (bloaty.GetSourceCount() == 0) {
    // Default when no sources are specified.
    bloaty.AddDataSource("sections");
  }

  bloaty.ScanDataSources();

  Rollup rollup;
  std::vector<const RangeMap*> range_maps;
  bloaty.GetRangeMaps(&range_maps, true);
  RangeMap::ComputeRollup(range_maps, true, &rollup);
  bloaty.GetRangeMaps(&range_maps, false);
  RangeMap::ComputeRollup(range_maps, false, &rollup);
  rollup.Print();
  return 0;
}
