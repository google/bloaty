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

#include <array>
#include <cmath>
#include <cinttypes>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
#define CHECK_RETURN(call) if (!(call)) { return false; }

namespace bloaty {

size_t max_label_len = 80;
int verbose_level = 0;

enum class SortBy {
  kVM, kFile, kBoth
} sortby = SortBy::kBoth;

struct DataSourceDefinition {
  DataSource number;
  const char* name;
  const char* description;
};

constexpr DataSourceDefinition data_sources[] = {
  {DataSource::kArchiveMembers, "armembers", "the .o files in a .a file"},
  {DataSource::kCompileUnits, "compileunits",
   "source file for the .o file (translation unit). requires debug info."},
  // Not a real data source, so we give it a junk DataSource::kInlines value
  {DataSource::kInlines, "inputfiles",
   "the filename specified on the Bloaty command-line"},
  {DataSource::kInlines, "inlines",
   "source line/file where inlined code came from.  requires debug info."},
  {DataSource::kSections, "sections", "object file section"},
  {DataSource::kSegments, "segments", "load commands in the binary"},
  {DataSource::kSymbols, "symbols", "symbols from symbol table"},
};

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

const char* GetDataSourceLabel(DataSource source) {
  for (size_t i = 0; i < ARRAY_SIZE(data_sources); i++) {
    if (data_sources[i].number == source) {
      return data_sources[i].name;
    }
  }
  fprintf(stderr, "Unknown data source label: %d\n", static_cast<int>(source));
  exit(1);
  return nullptr;
}

int SignOf(long val) {
  if (val < 0) {
    return -1;
  } else if (val > 0) {
    return 1;
  } else {
    return 0;
  }
}

template <class Func>
class RankComparator {
 public:
  RankComparator(Func func) : func_(func) {}

  template <class T>
  bool operator()(const T& a, const T& b) { return func_(a) < func_(b); }

 private:
  Func func_;
};

template <class Func>
RankComparator<Func> MakeRankComparator(Func func) {
  return RankComparator<Func>(func);
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
    if (name[name.size() - 1] != ')') {
      // (anonymous namespace)::ctype_w
      stripped_ = &name;
      return false;
    }

    int nesting = 0;
    for (size_t n = name.size() - 1; n < name.size(); --n) {
      if (name[n] == '(') {
        if (--nesting == 0) {
          storage_ = name.substr(0, n);
          stripped_ = &storage_;
          return true;
        }
      } else if (name[n] == ')') {
        ++nesting;
      }
    }


    stripped_ = &name;
    return false;
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
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Demangler);

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


// NameMunger //////////////////////////////////////////////////////////////////

// Use to transform input names according to the user's configuration.
// For example, the user can use regexes.
class NameMunger {
 public:
  NameMunger() {}

  // Adds a regex that will be applied to all names.  All regexes will be
  // applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);

  std::string Munge(StringPiece name) const;

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(NameMunger);
  std::vector<std::pair<std::unique_ptr<RE2>, std::string>> regexes_;
};

void NameMunger::AddRegex(const std::string& regex, const std::string& replacement) {
  std::unique_ptr<RE2> re2(new RE2(StringPiece(regex)));
  regexes_.push_back(std::make_pair(std::move(re2), replacement));
}

std::string NameMunger::Munge(StringPiece name) const {
  std::string ret(name.as_string());

  for (const auto& pair : regexes_) {
    if (RE2::Replace(&ret, *pair.first, pair.second)) {
      return ret;
    }
  }

  return ret;
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
// Rollup is the generic data structure, before we apply output massaging like
// collapsing excess elements into "[Other]" or sorting.

std::string others_label = "[Other]";

class Rollup {
 public:
  Rollup() {}

  void AddSizes(const std::vector<std::string> names,
                uint64_t size, bool is_vmsize) {
    // We start at 1 to exclude the base map (see base_map_).
    AddInternal(names, 1, size, is_vmsize);
  }

  // Prints a graphical representation of the rollup.
  bool ComputeWithBase(int row_limit, RollupOutput* row) const {
    return ComputeWithBase(nullptr, row_limit, row);
  }

  bool ComputeWithBase(Rollup* base, int row_limit, RollupOutput* output) const {
    RollupRow* row = &output->toplevel_row_;
    row->vmsize = vm_total_;
    row->filesize = file_total_;
    row->vmpercent = 100;
    row->filepercent = 100;
    output->longest_label_ = 0;
    CHECK_RETURN(
        CreateRows(0, row, &output->longest_label_, base, row_limit, true));
    return true;
  }

  // Subtract the values in "other" from this.
  void Subtract(const Rollup& other) {
    vm_total_ -= other.vm_total_;
    file_total_ -= other.file_total_;

    for (const auto& other_child : other.children_) {
      auto& child = children_[other_child.first];
      if (child.get() == NULL) {
        child.reset(new Rollup());
      }
      child->Subtract(*other_child.second);
    }
  }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Rollup);

  int64_t vm_total_ = 0;
  int64_t file_total_ = 0;

  // Putting Rollup by value seems to work on some compilers/libs but not
  // others.
  typedef std::unordered_map<std::string, std::unique_ptr<Rollup>> ChildMap;
  ChildMap children_;
  static Rollup* empty_;

  static Rollup* GetEmpty() {
    if (!empty_) {
      empty_ = new Rollup();
    }
    return empty_;
  }

  // Adds "size" bytes to the rollup under the label names[i].
  // If there are more entries names[i+1, i+2, etc] add them to sub-rollups.
  void AddInternal(const std::vector<std::string> names, size_t i,
                   int64_t size, bool is_vmsize) {
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

  static double Percent(ssize_t part, size_t whole) {
    return static_cast<double>(part) / static_cast<double>(whole) * 100;
  }

  bool CreateRows(size_t indent, RollupRow* row, size_t* longest_label,
                  const Rollup* base, int row_limit, bool is_toplevel) const;
  bool ComputeRows(size_t indent, RollupRow* row,
                   std::vector<RollupRow>* children, size_t* longest_label,
                   const Rollup* base, int row_limit, bool is_toplevel) const;
};

bool Rollup::CreateRows(size_t indent, RollupRow* row, size_t* longest_label,
                        const Rollup* base, int row_limit,
                        bool is_toplevel) const {
  if (base) {
    row->vmpercent = Percent(vm_total_, base->vm_total_);
    row->filepercent = Percent(file_total_, base->file_total_);
    row->diff_mode = true;
  }

  for (const auto& value : children_) {
    std::vector<RollupRow>* row_to_append = &row->sorted_children;
    int vm_sign = SignOf(value.second->vm_total_);
    int file_sign = SignOf(value.second->file_total_);
    if (vm_sign < 0 || file_sign < 0) {
      assert(base);
    }

    if (vm_sign + file_sign < 0) {
      row_to_append = &row->shrinking;
    } else if (vm_sign != file_sign && vm_sign + file_sign == 0) {
      row_to_append = &row->mixed;
    }

    if (value.second->vm_total_ != 0 || value.second->file_total_ != 0) {
      row_to_append->push_back(RollupRow(value.first));
      row_to_append->back().vmsize = value.second->vm_total_;
      row_to_append->back().filesize = value.second->file_total_;
    }
  }

  CHECK_RETURN(ComputeRows(indent, row, &row->sorted_children, longest_label,
                           base, row_limit, is_toplevel));
  CHECK_RETURN(ComputeRows(indent, row, &row->shrinking, longest_label, base,
                           row_limit, is_toplevel));
  CHECK_RETURN(ComputeRows(indent, row, &row->mixed, longest_label, base,
                           row_limit, is_toplevel));
  return true;
}

Rollup* Rollup::empty_;

bool Rollup::ComputeRows(size_t indent, RollupRow* row,
                         std::vector<RollupRow>* children,
                         size_t* longest_label, const Rollup* base,
                         int row_limit, bool is_toplevel) const {
  std::vector<RollupRow>& child_rows = *children;

  // We don't want to output a solitary "[None]" or "[Unmapped]" row except at
  // the top level.
  if (!is_toplevel && child_rows.size() == 1 &&
      (child_rows[0].name == "[None]" || child_rows[0].name == "[Unmapped]")) {
    child_rows.clear();
  }

  // We don't want to output a single row that has exactly the same size and
  // label as the parent.
  if (child_rows.size() == 1 && child_rows[0].name == row->name) {
    child_rows.clear();
  }

  if (child_rows.empty()) {
    return true;
  }

  // Our overall sorting rank.
  auto rank =
      [](const RollupRow& row) {
        int64_t val_to_rank;
        switch (sortby) {
          case SortBy::kVM:
            val_to_rank = std::abs(row.vmsize);
            break;
          case SortBy::kFile:
            val_to_rank = std::abs(row.filesize);
            break;
          case SortBy::kBoth:
            val_to_rank =
                std::max(std::abs(row.vmsize), std::abs(row.filesize));
            break;
          default:
            assert(false);
            val_to_rank = -1;
            break;
        }

        // Reverse so that numerically we always sort high-to-low.
        int64_t numeric_rank = INT64_MAX - val_to_rank;

        // Use name to break ties in numeric rank (names sort low-to-high).
        return std::make_tuple(numeric_rank, row.name);
      };

  // Our sorting rank for the first pass, when we are deciding what to put in
  // [Other].  Certain things we don't want to put in [Other], so we rank them
  // highest.
  auto collapse_rank =
      [rank](const RollupRow& row) {
        bool top_name = (row.name != "[None]");
        return std::make_tuple(top_name, rank(row));
      };

  std::sort(child_rows.begin(), child_rows.end(),
            MakeRankComparator(collapse_rank));

  RollupRow others_row(others_label);
  Rollup others_rollup;
  Rollup others_base;

  // Filter out everything but the top 'row_limit'.  Add rows that were filtered
  // out to "others_row".
  size_t i = child_rows.size() - 1;
  while (i >= row_limit) {
    others_row.vmsize += child_rows[i].vmsize;
    others_row.filesize += child_rows[i].filesize;
    if (base) {
      auto it = base->children_.find(child_rows[i].name);
      if (it != base->children_.end()) {
        others_base.vm_total_ += it->second->vm_total_;
        others_base.file_total_ += it->second->file_total_;
      }
    }

    child_rows.erase(child_rows.end() - 1);
    i--;
  }

  if (std::abs(others_row.vmsize) > 0 || std::abs(others_row.filesize) > 0) {
    child_rows.push_back(others_row);
    others_rollup.vm_total_ += others_row.vmsize;
    others_rollup.file_total_ += others_row.filesize;
  }

  // Sort all rows (including "other") and include sort by name.
  std::sort(child_rows.begin(), child_rows.end(), MakeRankComparator(rank));

  // Compute percents for all rows (including "Other")
  if (!base) {
    for (auto& child_row : child_rows) {
      child_row.vmpercent = Percent(child_row.vmsize, row->vmsize);
      child_row.filepercent = Percent(child_row.filesize, row->filesize);
    }
  }

  // Recurse into sub-rows, (except "Other", which isn't a real row).
  Demangler demangler;
  NameStripper stripper;
  for (auto& child_row : child_rows) {
    const Rollup* child_rollup;
    const Rollup* child_base = nullptr;

    if (child_row.name == others_label) {
      child_rollup = &others_rollup;
      if (base) {
        child_base = &others_base;
      }
    } else {
      auto it = children_.find(child_row.name);
      if (it == children_.end()) {
        std::cerr << "Should never happen, couldn't find name: "
                  << child_row.name << "\n";
        return false;
      }
      child_rollup = it->second.get();
      assert(child_rollup);

      if (base) {
        auto it = base->children_.find(child_row.name);
        if (it == base->children_.end()) {
          child_base = GetEmpty();
        } else {
          child_base = it->second.get();
        }
      }
    }

    auto demangled = demangler.Demangle(child_row.name);
    stripper.StripName(demangled);
    size_t allowed_label_len =
        std::min(stripper.stripped().size(), max_label_len);
    child_row.name = stripper.stripped();
    *longest_label = std::max(*longest_label, allowed_label_len + indent);
    CHECK_RETURN(child_rollup->CreateRows(indent + 4, &child_row, longest_label,
                                          child_base, row_limit, false));
  }
  return true;
}


// RollupOutput ////////////////////////////////////////////////////////////////

// RollupOutput represents rollup data after we have applied output massaging
// like collapsing excess rows into "[Other]" and sorted the output.  Once the
// data is in this format, we can print it to the screen (or verify the output
// in unit tests).

void PrintSpaces(size_t n, std::ostream* out) {
  for (size_t i = 0; i < n; i++) {
    *out << " ";
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

std::string LeftPad(const std::string& input, size_t size) {
  std::string ret = input;
  while (ret.size() < size) {
    ret = " " + ret;
  }

  return ret;
}

std::string DoubleStringPrintf(const char *fmt, double d) {
  char buf[1024];
  snprintf(buf, sizeof(buf), fmt, d);
  return std::string(buf);
}

std::string SiPrint(ssize_t size, bool force_sign) {
  const char *prefixes[] = {"", "Ki", "Mi", "Gi", "Ti"};
  size_t num_prefixes = 5;
  size_t n = 0;
  double size_d = size;
  while (fabs(size_d) > 1024 && n < num_prefixes - 2) {
    size_d /= 1024;
    n++;
  }

  std::string ret;

  if (fabs(size_d) > 100 || n == 0) {
    ret = std::to_string(static_cast<ssize_t>(size_d)) + prefixes[n];
    if (force_sign && size > 0) {
      ret = "+" + ret;
    }
  } else if (fabs(size_d) > 10) {
    if (force_sign) {
      ret = DoubleStringPrintf("%+0.1f", size_d) + prefixes[n];
    } else {
      ret = DoubleStringPrintf("%0.1f", size_d) + prefixes[n];
    }
  } else {
    if (force_sign) {
      ret = DoubleStringPrintf("%+0.2f", size_d) + prefixes[n];
    } else {
      ret = DoubleStringPrintf("%0.2f", size_d) + prefixes[n];
    }
  }

  return LeftPad(ret, 7);
}

std::string PercentString(double percent, bool diff_mode) {
  if (diff_mode) {
    if (percent == 0 || std::isnan(percent)) {
      return " [ = ]";
    } else if (percent == -100) {
      return " [DEL]";
    } else if (std::isinf(percent)) {
      return " [NEW]";
    } else {
      // We want to keep this fixed-width even if the percent is very large.
      std::string str;
      if (percent > 1000) {
        int digits = log10(percent) - 1;
        str = DoubleStringPrintf("%+2.0f", percent / pow(10, digits)) + "e" +
              std::to_string(digits) + "%";
      } else if (percent > 10) {
        str = DoubleStringPrintf("%+4.0f%%", percent);
      } else {
        str = DoubleStringPrintf("%+5.1F%%", percent);
      }

      return LeftPad(str, 6);
    }
  } else {
    return DoubleStringPrintf("%5.1F%%", percent);
  }
}

void RollupOutput::PrintRow(const RollupRow& row, size_t indent,
                            std::ostream* out) const {
  *out << FixedWidthString("", indent) << " "
       << PercentString(row.vmpercent, row.diff_mode) << " "
       << SiPrint(row.vmsize, row.diff_mode) << " "
       << FixedWidthString(row.name, longest_label_) << " "
       << SiPrint(row.filesize, row.diff_mode) << " "
       << PercentString(row.filepercent, row.diff_mode) << "\n";
}

void RollupOutput::PrintTree(const RollupRow& row, size_t indent,
                             std::ostream* out) const {
  // Rows are printed before their sub-rows.
  PrintRow(row, indent, out);

  // For now we don't print "confounding" sub-entries.  For example, if we're
  // doing a two-level analysis "-d section,symbol", and a section is growing
  // but a symbol *inside* the section is shrinking, we don't print the
  // shrinking symbol.  Mainly we do this to prevent the output from being too
  // confusing.  If we can find a clear, non-confusing way to present the
  // information we can add it back.

  if (row.vmsize > 0 || row.filesize > 0) {
    for (const auto& child : row.sorted_children) {
      PrintTree(child, indent + 4, out);
    }
  }

  if (row.vmsize < 0 || row.filesize < 0) {
    for (const auto& child : row.shrinking) {
      PrintTree(child, indent + 4, out);
    }
  }

  if ((row.vmsize < 0) != (row.filesize < 0)) {
    for (const auto& child : row.mixed) {
      PrintTree(child, indent + 4, out);
    }
  }
}

void RollupOutput::Print(std::ostream* out) const {
  *out << "     VM SIZE    ";
  PrintSpaces(longest_label_, out);
  *out << "    FILE SIZE";
  *out << "\n";

  if (toplevel_row_.diff_mode) {
    *out << " ++++++++++++++ ";
    *out << FixedWidthString("GROWING", longest_label_);
    *out << " ++++++++++++++";
    *out << "\n";
  } else {
    *out << " -------------- ";
    PrintSpaces(longest_label_, out);
    *out << " --------------";
    *out << "\n";
  }

  for (const auto& child : toplevel_row_.sorted_children) {
    PrintTree(child, 0, out);
  }

  if (toplevel_row_.diff_mode) {
    if (toplevel_row_.shrinking.size() > 0) {
      *out << "\n";
      *out << " -------------- ";
      *out << FixedWidthString("SHRINKING", longest_label_);
      *out << " --------------";
      *out << "\n";
      for (const auto& child : toplevel_row_.shrinking) {
        PrintTree(child, 0, out);
      }
    }

    if (toplevel_row_.mixed.size() > 0) {
      *out << "\n";
      *out << " -+-+-+-+-+-+-+ ";
      *out << FixedWidthString("MIXED", longest_label_);
      *out << " +-+-+-+-+-+-+-";
      *out << "\n";
      for (const auto& child : toplevel_row_.mixed) {
        PrintTree(child, 0, out);
      }
    }

    // Always output an extra row before "TOTAL".
    *out << "\n";
  }

  // The "TOTAL" row comes after all other rows.
  PrintRow(toplevel_row_, 0, out);
}


// RangeMap ////////////////////////////////////////////////////////////////////

template <class T>
uint64_t RangeMap::TranslateWithEntry(T iter, uint64_t addr) {
  assert(EntryContains(iter, addr));
  assert(iter->second.HasTranslation());
  return addr - iter->first + iter->second.other_start;
}

template <class T>
bool RangeMap::TranslateAndTrimRangeWithEntry(T iter, uint64_t addr,
                                              uint64_t end, uint64_t* out_addr,
                                              uint64_t* out_size) {
  addr = std::max(addr, iter->first);
  end = std::min(end, iter->second.end);

  if (addr >= end || !iter->second.HasTranslation()) return false;

  *out_addr = TranslateWithEntry(iter, addr);
  *out_size = end - addr;
  return true;
}

RangeMap::Map::iterator RangeMap::FindContaining(uint64_t addr) {
  auto it = mappings_.upper_bound(addr);  // Entry directly after.
  if (it == mappings_.begin() || (--it, !EntryContains(it, addr))) {
    return mappings_.end();
  } else {
    return it;
  }
}

RangeMap::Map::const_iterator RangeMap::FindContaining(uint64_t addr) const {
  auto it = mappings_.upper_bound(addr);  // Entry directly after.
  if (it == mappings_.begin() || (--it, !EntryContains(it, addr))) {
    return mappings_.end();
  } else {
    return it;
  }
}

RangeMap::Map::const_iterator RangeMap::FindContainingOrAfter(
    uint64_t addr) const {
  auto after = mappings_.upper_bound(addr);
  auto it = after;
  if (it != mappings_.begin() && (--it, EntryContains(it, addr))) {
    return it;  // Containing
  } else {
    return after;  // May be end().
  }
}

bool RangeMap::Translate(uint64_t addr, uint64_t* translated) const {
  auto iter = FindContaining(addr);
  if (iter == mappings_.end() || !iter->second.HasTranslation()) {
    return false;
  } else {
    *translated = TranslateWithEntry(iter, addr);
    return true;
  }
}

void RangeMap::AddRange(uint64_t addr, uint64_t size, const std::string& val) {
  AddDualRange(addr, size, UINT64_MAX, val);
}

void RangeMap::AddDualRange(uint64_t addr, uint64_t size, uint64_t otheraddr,
                            const std::string& val) {
  if (size == 0) return;

  const uint64_t base = addr;
  uint64_t end = addr + size;
  auto it = FindContainingOrAfter(addr);


  while (1) {
    while (it != mappings_.end() && EntryContains(it, addr)) {
      if (verbose_level > 1) {
        fprintf(stderr,
                "WARN: adding mapping [%" PRIx64 "x, %" PRIx64 "x] for label"
                "%s, this conflicts with existing mapping [%" PRIx64 ", %"
                PRIx64 "] for label %s\n",
                addr, end, val.c_str(), it->first, it->second.end,
                it->second.label.c_str());
      }
      addr = it->second.end;
      ++it;
    }

    if (addr >= end) {
      return;
    }

    uint64_t this_end = end;
    if (it != mappings_.end() && end > it->first) {
      this_end = std::min(end, it->first);
      if (verbose_level > 1) {
        fprintf(stderr,
                "WARN(2): adding mapping [%" PRIx64 ", %" PRIx64 "] for label "
                "%s, this conflicts with existing mapping [%" PRIx64 ", %"
                PRIx64 "] for label %s\n",
                addr, end, val.c_str(), it->first, it->second.end,
                it->second.label.c_str());
      }
    }

    uint64_t other =
        (otheraddr == UINT64_MAX) ? UINT64_MAX : addr - base + otheraddr;
    mappings_.insert(it, std::make_pair(addr, Entry(val, this_end, other)));
    addr = this_end;
  }
}

// In most cases we don't expect the range we're translating to span mappings
// in the translator.  For example, we would never expect a symbol to span
// sections.
//
// However there are some examples.  An archive member (in the file domain) can
// span several section mappings.  If we really wanted to get particular here,
// we could pass a parameter indicating whether such spanning is expected, and
// warn if not.
void RangeMap::AddRangeWithTranslation(uint64_t addr, uint64_t size,
                                       const std::string& val,
                                       const RangeMap& translator,
                                       RangeMap* other) {
  AddRange(addr, size, val);

  auto it = translator.FindContainingOrAfter(addr);
  uint64_t end = addr + size;

  // TODO: optionally warn about when we span ranges of the translator.  In some
  // cases this would be a bug (ie. symbols VM->file).  In other cases it's
  // totally normal (ie. archive members file->VM).
  while (it != translator.mappings_.end() && it->first < end) {
    uint64_t this_addr;
    uint64_t this_size;
    if (translator.TranslateAndTrimRangeWithEntry(it, addr, end, &this_addr,
                                                  &this_size)) {
      if (verbose_level > 2) {
        fprintf(stderr, "  -> translates to: [%" PRIx64 " %" PRIx64 "]\n",
                this_addr, this_size);
      }
      other->AddRange(this_addr, this_size, val);
    }
    ++it;
  }
}

template <class Func>
void RangeMap::ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                             const std::string& filename,
                             int filename_position, Func func) {
  assert(range_maps.size() > 0);

  std::vector<Map::const_iterator> iters;
  std::vector<std::string> keys;
  uint64_t current = UINTPTR_MAX;

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
    uint64_t next_break = UINTPTR_MAX;
    bool have_data = false;
    keys.clear();
    size_t i;

    for (i = 0; i < iters.size(); i++) {
      auto& iter = iters[i];

      if (filename_position >= 0 &&
          static_cast<unsigned>(filename_position) == i) {
        keys.push_back(filename);
      }

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
        keys.push_back(iter->second.label);
        next_break = std::min(next_break, RangeEnd(iter));
      }
    }

    if (filename_position >= 0 &&
        static_cast<unsigned>(filename_position) == i) {
      keys.push_back(filename);
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
      func(keys, current, next_break);
    }

    current = next_break;
  }
}


// MemoryMap ///////////////////////////////////////////////////////////////////

// Contains a RangeMap for VM space and file space.

class MemoryMap {
 public:
  MemoryMap(std::unique_ptr<NameMunger>&& munger) : munger_(std::move(munger)) {}
  virtual ~MemoryMap() {}

  bool FindAtAddr(uint64_t vmaddr, std::string* name) const;
  bool FindContainingAddr(uint64_t vmaddr, uint64_t* start,
                          std::string* name) const;

  const RangeMap* file_map() const { return &file_map_; }
  const RangeMap* vm_map() const { return &vm_map_; }
  RangeMap* file_map() { return &file_map_; }
  RangeMap* vm_map() { return &vm_map_; }

 protected:
  std::string ApplyNameRegexes(StringPiece name);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MemoryMap);
  friend class RangeSink;

  RangeMap vm_map_;
  RangeMap file_map_;
  std::unique_ptr<NameMunger> munger_;
};

std::string MemoryMap::ApplyNameRegexes(StringPiece name) {
  return munger_ ? munger_->Munge(name) : std::string(name.as_string());
}


// MmapInputFile ///////////////////////////////////////////////////////////////

class MmapInputFile : public InputFile {
 public:
  MmapInputFile(const std::string& filename) : InputFile(filename) {}
  ~MmapInputFile() override;

  bool Open();

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MmapInputFile);
};

bool MmapInputFile::Open() {
  int fd = open(filename().c_str(), O_RDONLY);
  struct stat buf;
  const char *map;

  if (fd < 0) {
    fprintf(stderr, "bloaty: couldn't open file '%s': %s\n", filename().c_str(),
            strerror(errno));
    return false;
  }

  if (fstat(fd, &buf) < 0) {
    fprintf(stderr, "bloaty: couldn't stat file '%s': %s\n", filename().c_str(),
            strerror(errno));
    return false;
  }

  map = static_cast<char*>(
      mmap(nullptr, buf.st_size, PROT_READ, MAP_SHARED, fd, 0));

  if (map == MAP_FAILED) {
    fprintf(stderr, "bloaty: couldn't mmap file '%s': %s\n", filename().c_str(),
            strerror(errno));
    return false;
  }

  data_.set(map, buf.st_size);
  return true;
}

MmapInputFile::~MmapInputFile() {
  if (data_.data() != nullptr) {
    if (munmap(const_cast<char*>(data_.data()), data_.size()) != 0) {
      fprintf(stderr, "bloaty: error calling munmap(): %s\n", strerror(errno));
    }
  }
}

std::unique_ptr<InputFile> MmapInputFileFactory::TryOpenFile(
    const std::string& filename) const {
  std::unique_ptr<MmapInputFile> file(new MmapInputFile(filename));
  if (!file->Open()) {
    file.reset();
  }
  return std::move(file);
}


// RangeSink ///////////////////////////////////////////////////////////////////

RangeSink::RangeSink(const InputFile* file, DataSource data_source,
                     const MemoryMap* translator, MemoryMap* map)
    : file_(file),
      data_source_(data_source),
      translator_(translator),
      map_(map) {
  assert(map_);
}

RangeSink::~RangeSink() {}

void RangeSink::AddFileRange(StringPiece name, uint64_t fileoff,
                             uint64_t filesize) {
  if (verbose_level > 2) {
    fprintf(stderr, "[%s] AddFileRange(%.*s, %" PRIx64 ", %" PRIx64 ")\n",
            GetDataSourceLabel(data_source_), (int)name.size(), name.data(),
            fileoff, filesize);
  }
  const std::string label = map_->ApplyNameRegexes(name);
  if (translator_) {
    map_->file_map()->AddRangeWithTranslation(
        fileoff, filesize, label, *translator_->file_map(), map_->vm_map());
  }
}

void RangeSink::AddVMRange(uint64_t vmaddr, uint64_t vmsize,
                           const std::string& name) {
  if (verbose_level > 2) {
    fprintf(stderr, "[%s] AddVMRange(%.*s, %" PRIx64 ", %" PRIx64 ")\n",
            GetDataSourceLabel(data_source_), (int)name.size(), name.data(),
            vmaddr, vmsize);
  }
  assert(translator_);
  const std::string label = map_->ApplyNameRegexes(name);
  map_->vm_map()->AddRangeWithTranslation(
      vmaddr, vmsize, label, *translator_->vm_map(), map_->file_map());
}

void RangeSink::AddVMRangeAllowAlias(uint64_t vmaddr, uint64_t size,
                                     const std::string& name) {
  // TODO: maybe track alias (but what would we use it for?)
  // TODO: verify that it is in fact an alias.
  AddVMRange(vmaddr, size, name);
}

void RangeSink::AddVMRangeIgnoreDuplicate(uint64_t vmaddr, uint64_t vmsize,
                                          const std::string& name) {
  // TODO suppress warning that AddVMRange alone might trigger.
  AddVMRange(vmaddr, vmsize, name);
}

void RangeSink::AddRange(StringPiece name, uint64_t vmaddr, uint64_t vmsize,
                         uint64_t fileoff, uint64_t filesize) {
  if (verbose_level > 2) {
    fprintf(stderr, "[%s] AddRange(%.*s, %" PRIx64 ", %" PRIx64 ", %" PRIx64
            ", %" PRIx64 ")\n",
            GetDataSourceLabel(data_source_), (int)name.size(), name.data(),
            vmaddr, vmsize, fileoff, filesize);
  }
  const std::string label = map_->ApplyNameRegexes(name);
  uint64_t common = std::min(vmsize, filesize);

  map_->vm_map()->AddDualRange(vmaddr, common, fileoff, label);
  map_->file_map()->AddDualRange(fileoff, common, vmaddr, label);

  map_->vm_map()->AddRange(vmaddr + common, vmsize - common, label);
  map_->file_map()->AddRange(fileoff + common, filesize - common, label);
}


// Bloaty //////////////////////////////////////////////////////////////////////

// Represents a program execution and associated state.

struct ConfiguredDataSource {
  ConfiguredDataSource(const DataSourceDefinition& definition)
      : source(definition.number), munger(new NameMunger()) {}
  DataSource source;
  std::unique_ptr<NameMunger> munger;
};

class Bloaty {
 public:
  Bloaty(const InputFileFactory& factory);

  bool AddFilename(const std::string& filename, bool base_file);

  ConfiguredDataSource* FindDataSource(const std::string& name) const;
  size_t GetSourceCount() const {
    return sources_.size() + (filename_position_ >= 0 ? 1 : 0);
  }

  bool AddDataSource(const std::string& name);
  bool ScanAndRollup(RollupOutput* output);
  void PrintDataSources() const {
    for (const auto& source : all_known_sources_) {
      const auto& definition = source.second;
      fprintf(stderr, "%s\n", definition.name);
    }
  }

  void SetRowLimit(int n) { row_limit_ = (n == 0) ? INT_MAX : n; }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Bloaty);

  template <size_t T>
  void MakeAllSourcesMap(const DataSourceDefinition (&sources)[T]) {
    for (size_t i = 0; i < T; i++) {
      auto& source = sources[i];
      all_known_sources_[source.name] = source;
    }
  }

  bool ScanAndRollupFile(const InputFile& file, Rollup* rollup);

  const InputFileFactory& file_factory_;
  std::map<std::string, DataSourceDefinition> all_known_sources_;
  std::vector<ConfiguredDataSource> sources_;
  std::map<std::string, ConfiguredDataSource*> sources_by_name_;
  std::vector<std::unique_ptr<InputFile>> input_files_;
  std::vector<std::unique_ptr<InputFile>> base_files_;
  int row_limit_;
  int filename_position_;
};

Bloaty::Bloaty(const InputFileFactory& factory)
    : file_factory_(factory), row_limit_(20), filename_position_(-1) {
  MakeAllSourcesMap(data_sources);
}

bool Bloaty::AddFilename(const std::string& filename, bool is_base) {
  std::unique_ptr<InputFile> file(file_factory_.TryOpenFile(filename));
  if (!file.get()) {
    std::cerr << "bloaty: couldn't open file '" << filename << "'\n";
    return false;
  }

  if (is_base) {
    base_files_.push_back(std::move(file));
  } else {
    input_files_.push_back(std::move(file));
  }

  return true;
}

bool Bloaty::AddDataSource(const std::string& name) {
  if (name == "inputfiles") {
    filename_position_ = sources_.size() + 1;
    return true;
  }

  auto it = all_known_sources_.find(name);
  if (it == all_known_sources_.end()) {
    std::cerr << "bloaty: no such data source: " << name << "\n";
    return false;
  }

  sources_.emplace_back(it->second);
  sources_by_name_[name] = &sources_.back();
  return true;
}

ConfiguredDataSource* Bloaty::FindDataSource(const std::string& name) const {
  auto it = sources_by_name_.find(name);
  if (it != sources_by_name_.end()) {
    return it->second;
  } else {
    return NULL;
  }
}

bool Bloaty::ScanAndRollupFile(const InputFile& file, Rollup* rollup) {
  const std::string& filename = file.filename();
  auto file_handler = TryOpenELFFile(file);

  if (!file_handler.get()) {
    file_handler = TryOpenMachOFile(file);
  }

  if (!file_handler.get()) {
    fprintf(stderr, "bloaty: Unknown file type: %s\n", filename.c_str());
    return false;
  }

  struct Maps {
   public:
    Maps() : base_map_(nullptr) { PushMap(&base_map_); }

    void PushAndOwnMap(MemoryMap* map) {
      maps_.emplace_back(map);
      PushMap(map);
    }

    void PushMap(MemoryMap* map) {
      vm_maps_.push_back(map->vm_map());
      file_maps_.push_back(map->file_map());
    }

    void ComputeRollup(const std::string& filename, int filename_position,
                       Rollup* rollup) {
      RangeMap::ComputeRollup(
          vm_maps_, filename, filename_position,
          [=](const std::vector<std::string>& keys, uint64_t addr,
              uint64_t end) { rollup->AddSizes(keys, end - addr, true); });
      RangeMap::ComputeRollup(
          file_maps_, filename, filename_position,
          [=](const std::vector<std::string>& keys, uint64_t addr,
              uint64_t end) { rollup->AddSizes(keys, end - addr, false); });
    }

    void PrintMaps(const std::vector<const RangeMap*> maps,
                   const std::string& filename, int filename_position) {
      uint64_t last = 0;
      RangeMap::ComputeRollup(maps, filename, filename_position,
                              [&](const std::vector<std::string>& keys,
                                  uint64_t addr, uint64_t end) {
                                if (addr > last) {
                                  PrintMapRow("NO ENTRY", last, addr);
                                }
                                PrintMapRow(KeysToString(keys), addr, end);
                                last = end;
                              });
    }

    void PrintFileMaps(const std::string& filename, int filename_position) {
      PrintMaps(file_maps_, filename, filename_position);
    }

    void PrintVMMaps(const std::string& filename, int filename_position) {
      PrintMaps(vm_maps_, filename, filename_position);
    }

    std::string KeysToString(const std::vector<std::string>& keys) {
      std::string ret;

      for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) {
          ret += ", ";
        }
        ret += keys[i];
      }

      return ret;
    }

    void PrintMapRow(StringPiece str, uint64_t start, uint64_t end) {
      printf("[%" PRIx64 ", %" PRIx64 "] %.*s\n", start, end, (int)str.size(),
             str.data());
    }

    MemoryMap* base_map() { return &base_map_; }

   private:
    MemoryMap base_map_;
    std::vector<std::unique_ptr<MemoryMap>> maps_;
    std::vector<const RangeMap*> vm_maps_;
    std::vector<const RangeMap*> file_maps_;

  } maps;

  RangeSink sink(&file, DataSource::kSegments, nullptr, maps.base_map());
  file_handler->ProcessBaseMap(&sink);
  maps.base_map()->file_map()->AddRange(0, file.data().size(), "[None]");

  std::vector<std::unique_ptr<RangeSink>> sinks;
  std::vector<RangeSink*> sink_ptrs;

  for (size_t i = 0; i < sources_.size(); i++) {
    auto& source = sources_[i];
    auto map = new MemoryMap(std::move(source.munger));
    maps.PushAndOwnMap(map);
    sinks.push_back(std::unique_ptr<RangeSink>(
        new RangeSink(&file, source.source, maps.base_map(), map)));
    sink_ptrs.push_back(sinks.back().get());
  }

  CHECK_RETURN(file_handler->ProcessFile(sink_ptrs));

  maps.ComputeRollup(filename, filename_position_, rollup);
  if (verbose_level > 0) {
    fprintf(stderr, "FILE MAP:\n");
    maps.PrintFileMaps(filename, filename_position_);
    fprintf(stderr, "VM MAP:\n");
    maps.PrintVMMaps(filename, filename_position_);
  }
  return true;
}

bool Bloaty::ScanAndRollup(RollupOutput* output) {
  if (input_files_.empty()) {
    fputs("bloaty: no filename specified, exiting.\n", stderr);
    return false;
  }

  Rollup rollup;

  for (const auto& file : input_files_) {
    CHECK_RETURN(ScanAndRollupFile(*file, &rollup));
  }

  if (!base_files_.empty()) {
    Rollup base;

    for (const auto& base_file : base_files_) {
      CHECK_RETURN(ScanAndRollupFile(*base_file, &base));
    }

    rollup.Subtract(base);
    CHECK_RETURN(rollup.ComputeWithBase(&base, row_limit_, output));
  } else {
    CHECK_RETURN(rollup.ComputeWithBase(nullptr, row_limit_, output));
  }

  return true;
}

const char usage[] = R"(Bloaty McBloatface: a size profiler for binaries.

USAGE: bloaty [options] file... [-- base_file...]

Options:

  -d <sources>     Comma-separated list of sources to scan.
  -n <num>         How many rows to show per level before collapsing
                   other keys into '[Other]'.  Set to '0' for unlimited.
                   Defaults to 20.
  -r <regex>       Add regex to the list of regexes.
                   Format for regex is:
                     SOURCE:s/PATTERN/REPLACEMENT/
  -s <sortby>      Whether to sort by VM or File size.  Possible values
                   are:
                     -s vm
                     -s file
                     -s both (the default: sorts by max(vm, file)).
  -v               Verbose output.  Dumps warnings encountered during
                   processing and full VM/file maps at the end.
                   Add more v's (-vv, -vvv) for even more.
  -w               Wide output; don't truncate long labels.
  --help           Display this message and exit.
  --list-sources   Show a list of available sources and exit.
)";

bool CheckNextArg(int i, int argc, const char *option) {
  if (i + 1 >= argc) {
    fprintf(stderr, "bloaty: option '%s' requires an argument\n", option);
    return false;
  }
  return true;
}

void Split(const std::string& str, char delim, std::vector<std::string>* out) {
  std::stringstream stream(str);
  std::string item;
  while (std::getline(stream, item, delim)) {
    out->push_back(item);
  }
}

bool BloatyMain(int argc, char* argv[], const InputFileFactory& file_factory,
                RollupOutput* output) {
  bloaty::Bloaty bloaty(file_factory);

  RE2 regex_pattern("(\\w+)\\:s/(.*)/(.*)/");
  bool base_files = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      if (base_files) {
        std::cerr << "bloaty: '--' option should only be specified once.\n";
        return false;
      }
      base_files = true;
    } else if (strcmp(argv[i], "-d") == 0) {
      if (!CheckNextArg(i, argc, "-d")) {
        return false;
      }
      std::vector<std::string> names;
      Split(argv[++i], ',', &names);
      for (const auto& name : names) {
        CHECK_RETURN(bloaty.AddDataSource(name));
      }
    } else if (strcmp(argv[i], "-r") == 0) {
      std::string source_name, regex, substitution;
      if (!RE2::FullMatch(argv[++i], regex_pattern,
                          &source_name, &regex, &substitution)) {
        std::cerr << "Bad format for regex, should be: "
                  << "source+=/pattern/replacement/\n";
        return false;
      }

      auto source = bloaty.FindDataSource(source_name);
      if (!source) {
        std::cerr << "Data source '" << source_name
                  << "' not found in previous "
                  << "-d option\n";
        return false;
      }

      source->munger->AddRegex(regex, substitution);
    } else if (strcmp(argv[i], "-n") == 0) {
      if (!CheckNextArg(i, argc, "-n")) {
        return false;
      }
      bloaty.SetRowLimit(strtod(argv[++i], NULL));
    } else if (strcmp(argv[i], "-s") == 0) {
      if (!CheckNextArg(i, argc, "-s")) {
        return false;
      }
      i++;
      if (strcmp(argv[i], "vm") == 0) {
        sortby = SortBy::kVM;
      } else if (strcmp(argv[i], "file") == 0) {
        sortby = SortBy::kFile;
      } else if (strcmp(argv[i], "both") == 0) {
        sortby = SortBy::kBoth;
      } else {
        std::cerr << "Unknown value for -s: " << argv[i] << "\n";
        return false;
      }
    } else if (strcmp(argv[i], "-v") == 0) {
      verbose_level = 1;
    } else if (strcmp(argv[i], "-vv") == 0) {
      verbose_level = 2;
    } else if (strcmp(argv[i], "-vvv") == 0) {
      verbose_level = 3;
    } else if (strcmp(argv[i], "-w") == 0) {
      max_label_len = SIZE_MAX;
    } else if (strcmp(argv[i], "--list-sources") == 0) {
      bloaty.PrintDataSources();
      return false;
    } else if (strcmp(argv[i], "--help") == 0) {
      fputs(usage, stderr);
      return false;
    } else if (argv[i][0] == '-') {
      std::cerr << "Unknown option: " << argv[i] << "\n";
      return false;
    } else {
      CHECK_RETURN(bloaty.AddFilename(argv[i], base_files));
    }
  }

  if (bloaty.GetSourceCount() == 0) {
    // Default when no sources are specified.
    bloaty.AddDataSource("sections");
  }

  CHECK_RETURN(bloaty.ScanAndRollup(output));
  return true;
}

}  // namespace bloaty
