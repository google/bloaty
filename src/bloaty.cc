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

#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <assert.h>
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

#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "absl/strings/str_join.h"
#include "absl/strings/substitute.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/text_format.h"
#include "re2/re2.h"

#include "bloaty.h"
#include "bloaty.pb.h"
#include "demangle.h"

using absl::string_view;

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CHECK_SYSCALL(call) \
  if (call < 0) { \
    perror(#call " " __FILE__ ":" TOSTRING(__LINE__)); \
    exit(1); \
  }

static void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)

namespace bloaty {

void ThrowBloatyError(const char *str) {
  throw bloaty::Error(str);
}

// Use a global since we would have to plumb it through so many call-stacks
// otherwise.  We would make this thread_local but that's not supported on OS X
// right now.
int verbose_level = 0;

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
    {DataSource::kSymbols, "symbols",
     "symbols from symbol table (configure demangling with --demangle)"},
    {DataSource::kRawSymbols, "rawsymbols", "unmangled symbols"},
    {DataSource::kFullSymbols, "fullsymbols", "full demangled symbols"},
    {DataSource::kShortSymbols, "shortsymbols", "short demangled symbols"},
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

template <typename A, typename B>
void CheckedAdd(A* accum, B val) {
  // We've only implemented the portable version for a subset of possible types.
  static_assert(std::is_signed<A>::value, "requires signed A");
  static_assert(sizeof(A) == sizeof(B), "requires integers of the same type");
#if ABSL_HAVE_BUILTIN(__builtin_add_overflow)
  if (__builtin_add_overflow(*accum, val, accum)) {
    THROW("integer overflow");
  }
#else
  bool safe = *accum < 0 ? (val >= std::numeric_limits<B>::max() - *accum)
                         : (val <= std::numeric_limits<B>::max() - *accum);
  if (!safe) {
    THROW("integer overflow");
  }
  *accum += val;
#endif
}

std::string CSVEscape(string_view str) {
  bool need_escape = false;

  for (char ch : str) {
    if (ch == '"' || ch == ',') {
      need_escape = true;
      break;
    }
  }

  if (need_escape) {
    std::string ret = "\"";
    for (char ch : str) {
      if (ch == '"') {
        ret += "\"\"";
      } else {
        ret += ch;
      }
    }
    ret += "\"";
    return ret;
  } else {
    return std::string(str);
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

extern "C" char* __cxa_demangle(const char* mangled_name, char* buf, size_t* n,
                                int* status);

std::string ItaniumDemangle(string_view symbol, DataSource source) {
  if (source == DataSource::kRawSymbols) {
    // No demangling.
    return std::string(symbol);
  }

  string_view demangle_from = symbol;
  if (absl::StartsWith(demangle_from, "__Z")) {
    demangle_from.remove_prefix(1);
  }

  if (source == DataSource::kShortSymbols) {
    char demangled[1024];
    if (::Demangle(demangle_from.data(), demangled, sizeof(demangled))) {
      return std::string(demangled);
    } else {
      return std::string(symbol);
    }
  } else if (source == DataSource::kFullSymbols) {
    char* demangled =
        __cxa_demangle(demangle_from.data(), NULL, NULL, NULL);
    if (demangled) {
      std::string ret(demangled);
      free(demangled);
      return ret;
    } else {
      return std::string(symbol);
    }
  } else {
    BLOATY_UNREACHABLE();
  }
}


// NameMunger //////////////////////////////////////////////////////////////////

void NameMunger::AddRegex(const std::string& regex, const std::string& replacement) {
  auto re2 = absl::make_unique<RE2>(regex);
  regexes_.push_back(std::make_pair(std::move(re2), replacement));
}

std::string NameMunger::Munge(string_view name) const {
  re2::StringPiece piece(name.data(), name.size());
  std::string ret;

  if (!name.empty() && name[0] == '[') {
    // This is a special symbol, don't mangle.
    return std::string(name);
  }

  for (const auto& pair : regexes_) {
    if (RE2::Extract(piece, *pair.first, pair.second, &ret)) {
      return ret;
    }
  }

  return std::string(name);
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

  Rollup(Rollup&& other) = default;
  Rollup& operator=(Rollup&& other) = default;

  void AddSizes(const std::vector<std::string> names,
                uint64_t size, bool is_vmsize) {
    // We start at 1 to exclude the base map (see base_map_).
    AddInternal(names, 1, size, is_vmsize);
  }

  // Prints a graphical representation of the rollup.
  void CreateRollupOutput(const Options& options, RollupOutput* output) const {
    CreateDiffModeRollupOutput(nullptr, options, output);
    output->diff_mode_ = false;
  }

  void CreateDiffModeRollupOutput(Rollup* base, const Options& options,
                                  RollupOutput* output) const {
    RollupRow* row = &output->toplevel_row_;
    row->vmsize = vm_total_;
    row->filesize = file_total_;
    row->vmpercent = 100;
    row->filepercent = 100;
    output->diff_mode_ = true;
    CreateRows(row, base, options, true);
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

  // Add the values in "other" from this.
  void Add(const Rollup& other) {
    vm_total_ += other.vm_total_;
    file_total_ += other.file_total_;

    for (const auto& other_child : other.children_) {
      auto& child = children_[other_child.first];
      if (child.get() == NULL) {
        child.reset(new Rollup());
      }
      child->Add(*other_child.second);
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
                   uint64_t size, bool is_vmsize) {
    if (is_vmsize) {
      CheckedAdd(&vm_total_, size);
    } else {
      CheckedAdd(&file_total_, size);
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
    if (whole == 0) {
      if (part == 0) {
        return NAN;
      } else if (part > 0) {
        return INFINITY;
      } else {
        return -INFINITY;
      }
    } else {
      return static_cast<double>(part) / static_cast<double>(whole) * 100;
    }
  }

  void CreateRows(RollupRow* row, const Rollup* base, const Options& options,
                  bool is_toplevel) const;
  void SortAndAggregateRows(RollupRow* row, const Rollup* base,
                            const Options& options, bool is_toplevel) const;
};

void Rollup::CreateRows(RollupRow* row, const Rollup* base,
                        const Options& options, bool is_toplevel) const {
  if (base) {
    // For a diff, the percentage is a comparison against the previous size of
    // the same label at the same level.
    row->vmpercent = Percent(vm_total_, base->vm_total_);
    row->filepercent = Percent(file_total_, base->file_total_);
  }

  for (const auto& value : children_) {
    if (value.second->vm_total_ != 0 || value.second->file_total_ != 0) {
      row->sorted_children.emplace_back(value.first);
      RollupRow& child_row = row->sorted_children.back();
      child_row.vmsize = value.second->vm_total_;
      child_row.filesize = value.second->file_total_;
    }
  }

  SortAndAggregateRows(row, base, options, is_toplevel);
}

Rollup* Rollup::empty_;

void Rollup::SortAndAggregateRows(RollupRow* row, const Rollup* base,
                                  const Options& options,
                                  bool is_toplevel) const {
  std::vector<RollupRow>& child_rows = row->sorted_children;

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
    return;
  }

  // First sort by magnitude.
  for (auto& child : child_rows) {
    switch (options.sort_by()) {
      case Options::SORTBY_VMSIZE:
        child.sortkey = std::abs(child.vmsize);
        break;
      case Options::SORTBY_FILESIZE:
        child.sortkey = std::abs(child.filesize);
        break;
      case Options::SORTBY_BOTH:
        child.sortkey =
            std::max(std::abs(child.vmsize), std::abs(child.filesize));
        break;
      default:
        BLOATY_UNREACHABLE();
    }
  }

  std::sort(child_rows.begin(), child_rows.end(), &RollupRow::Compare);

  RollupRow others_row(others_label);
  others_row.other_count = child_rows.size() - options.max_rows_per_level();
  others_row.name = absl::Substitute("[$0 Others]", others_row.other_count);
  Rollup others_rollup;
  Rollup others_base;

  // Filter out everything but the top 'row_limit'.  Add rows that were filtered
  // out to "others_row".
  size_t i = child_rows.size() - 1;
  while (i >= options.max_rows_per_level()) {
    CheckedAdd(&others_row.vmsize, child_rows[i].vmsize);
    CheckedAdd(&others_row.filesize, child_rows[i].filesize);
    if (base) {
      auto it = base->children_.find(child_rows[i].name);
      if (it != base->children_.end()) {
        CheckedAdd(&others_base.vm_total_, it->second->vm_total_);
        CheckedAdd(&others_base.file_total_, it->second->file_total_);
      }
    }

    child_rows.erase(child_rows.end() - 1);
    i--;
  }

  if (std::abs(others_row.vmsize) > 0 || std::abs(others_row.filesize) > 0) {
    child_rows.push_back(others_row);
    CheckedAdd(&others_rollup.vm_total_, others_row.vmsize);
    CheckedAdd(&others_rollup.file_total_, others_row.filesize);
  }

  // Now sort by actual value (positive or negative).
  for (auto& child : child_rows) {
    switch (options.sort_by()) {
      case Options::SORTBY_VMSIZE:
        child.sortkey = child.vmsize;
        break;
      case Options::SORTBY_FILESIZE:
        child.sortkey = child.filesize;
        break;
      case Options::SORTBY_BOTH:
        if (std::abs(child.vmsize) > std::abs(child.filesize)) {
          child.sortkey = child.vmsize;
        } else {
          child.sortkey = child.filesize;
        }
        break;
      default:
        BLOATY_UNREACHABLE();
    }
  }

  std::sort(child_rows.begin(), child_rows.end(), &RollupRow::Compare);

  // For a non-diff, the percentage is compared to the total size of the parent.
  if (!base) {
    for (auto& child_row : child_rows) {
      child_row.vmpercent = Percent(child_row.vmsize, row->vmsize);
      child_row.filepercent = Percent(child_row.filesize, row->filesize);
    }
  }

  // Recurse into sub-rows, (except "Other", which isn't a real row).
  for (auto& child_row : child_rows) {
    const Rollup* child_rollup;
    const Rollup* child_base = nullptr;

    if (child_row.other_count > 0) {
      child_rollup = &others_rollup;
      if (base) {
        child_base = &others_base;
      }
    } else {
      auto it = children_.find(child_row.name);
      if (it == children_.end()) {
        THROWF("internal error, couldn't find name $0", child_row.name);
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

    child_rollup->CreateRows(&child_row, child_base, options, false);
  }
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

void RollupOutput::PrettyPrintRow(const RollupRow& row, size_t indent,
                                  size_t longest_label,
                                  std::ostream* out) const {
  *out << FixedWidthString("", indent) << " "
       << PercentString(row.vmpercent, diff_mode_) << " "
       << SiPrint(row.vmsize, diff_mode_) << " "
       << FixedWidthString(row.name, longest_label) << " "
       << SiPrint(row.filesize, diff_mode_) << " "
       << PercentString(row.filepercent, diff_mode_) << "\n";
}

void RollupOutput::PrettyPrintTree(const RollupRow& row, size_t indent,
                                   size_t longest_label,
                                   std::ostream* out) const {
  // Rows are printed before their sub-rows.
  PrettyPrintRow(row, indent, longest_label, out);

  if (row.vmsize || row.filesize) {
    for (const auto& child : row.sorted_children) {
      PrettyPrintTree(child, indent + 4, longest_label, out);
    }
  }
}

size_t RollupOutput::CalculateLongestLabel(const RollupRow& row,
                                           int indent) const {
  size_t ret = indent + row.name.size();

  for (const auto& child : row.sorted_children) {
    ret = std::max(ret, CalculateLongestLabel(child, indent + 4));
  }

  return ret;
}

void RollupOutput::PrettyPrint(size_t max_label_len, std::ostream* out) const {
  size_t longest_label = toplevel_row_.name.size();
  for (const auto& child : toplevel_row_.sorted_children) {
    longest_label = std::max(longest_label, CalculateLongestLabel(child, 0));
  }

  longest_label = std::min(longest_label, max_label_len);

  *out << "     VM SIZE    ";
  PrintSpaces(longest_label, out);
  *out << "    FILE SIZE";
  *out << "\n";

  *out << " -------------- ";
  PrintSpaces(longest_label, out);
  *out << " --------------";
  *out << "\n";

  for (const auto& child : toplevel_row_.sorted_children) {
    PrettyPrintTree(child, 0, longest_label, out);
  }

  // The "TOTAL" row comes after all other rows.
  PrettyPrintRow(toplevel_row_, 0, longest_label, out);
}

void RollupOutput::PrintRowToCSV(const RollupRow& row,
                                 std::vector<std::string> parent_labels,
                                 std::ostream* out) const {
  while (parent_labels.size() < source_names_.size()) {
    // If this label had no data at this level, append an empty string.
    parent_labels.push_back("");
  }

  parent_labels.push_back(std::to_string(row.vmsize));
  parent_labels.push_back(std::to_string(row.filesize));

  *out << absl::StrJoin(parent_labels, ",") << "\n";
}

void RollupOutput::PrintTreeToCSV(const RollupRow& row,
                                  std::vector<std::string> parent_labels,
                                  std::ostream* out) const {
  parent_labels.push_back(row.name);
  if (row.sorted_children.size() > 0) {
    for (const auto& child_row : row.sorted_children) {
      PrintTreeToCSV(child_row, parent_labels, out);
    }
  } else {
    PrintRowToCSV(row, parent_labels, out);
  }
}

void RollupOutput::PrintToCSV(std::ostream* out) const {
  std::vector<std::string> names(source_names_);
  names.push_back("vmsize");
  names.push_back("filesize");
  *out << absl::StrJoin(names, ",") << "\n";
  for (const auto& child_row : toplevel_row_.sorted_children) {
    PrintTreeToCSV(child_row, std::vector<std::string>(), out);
  }
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

bool RangeMap::TryGetLabel(uint64_t addr, std::string* label, uint64_t* offset) const {
  auto iter = FindContaining(addr);
  if (iter == mappings_.end()) {
    return false;
  } else {
    *label = iter->second.label;
    *offset = addr - iter->first;
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
    if (!range_map->IterIsEnd(iters.back())) {
      current = std::min(current, iters.back()->first);
    }
  }

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


// MmapInputFile ///////////////////////////////////////////////////////////////

class MmapInputFile : public InputFile {
 public:
  MmapInputFile(const std::string& filename);
  ~MmapInputFile() override;

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MmapInputFile);
};

class FileDescriptor {
 public:
  FileDescriptor(int fd) : fd_(fd) {}

  ~FileDescriptor() {
    if (fd_ >= 0 && close(fd_) < 0) {
      fprintf(stderr, "bloaty: error calling close(): %s\n", strerror(errno));
    }
  }

  int fd() { return fd_; }

 private:
  int fd_;
};

MmapInputFile::MmapInputFile(const std::string& filename)
    : InputFile(filename) {
  FileDescriptor fd(open(filename.c_str(), O_RDONLY));
  struct stat buf;
  const char *map;

  if (fd.fd() < 0) {
    THROWF("couldn't open file '$0': $1", filename, strerror(errno));
  }

  if (fstat(fd.fd(), &buf) < 0) {
    THROWF("couldn't stat file '$0': $1", filename, strerror(errno));
  }

  map = static_cast<char*>(
      mmap(nullptr, buf.st_size, PROT_READ, MAP_SHARED, fd.fd(), 0));

  if (map == MAP_FAILED) {
    THROWF("couldn't mmap file '$0': $1", filename, strerror(errno));
  }

  data_ = string_view(map, buf.st_size);
}

MmapInputFile::~MmapInputFile() {
  if (data_.data() != nullptr &&
      munmap(const_cast<char*>(data_.data()), data_.size()) != 0) {
    fprintf(stderr, "bloaty: error calling munmap(): %s\n", strerror(errno));
  }
}

std::unique_ptr<InputFile> MmapInputFileFactory::OpenFile(
    const std::string& filename) const {
  return absl::make_unique<MmapInputFile>(filename);
}


// RangeSink ///////////////////////////////////////////////////////////////////

RangeSink::RangeSink(const InputFile* file, DataSource data_source,
                     const DualMap* translator)
    : file_(file),
      data_source_(data_source),
      translator_(translator) {}

RangeSink::~RangeSink() {}

void RangeSink::AddOutput(DualMap* map, const NameMunger* munger) {
  outputs_.push_back(std::make_pair(map, munger));
}

void RangeSink::AddFileRange(string_view name, uint64_t fileoff,
                             uint64_t filesize) {
  if (verbose_level > 2) {
    fprintf(stderr, "[%s] AddFileRange(%.*s, %" PRIx64 ", %" PRIx64 ")\n",
            GetDataSourceLabel(data_source_), (int)name.size(), name.data(),
            fileoff, filesize);
  }
  for (auto& pair : outputs_) {
    const std::string label = pair.second->Munge(name);
    if (translator_) {
      pair.first->file_map.AddRangeWithTranslation(fileoff, filesize, label,
                                                    translator_->file_map,
                                                    &pair.first->vm_map);
    }
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
  for (auto& pair : outputs_) {
    const std::string label = pair.second->Munge(name);
    pair.first->vm_map.AddRangeWithTranslation(
        vmaddr, vmsize, label, translator_->vm_map, &pair.first->file_map);
  }
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

void RangeSink::AddRange(string_view name, uint64_t vmaddr, uint64_t vmsize,
                         uint64_t fileoff, uint64_t filesize) {
  if (verbose_level > 2) {
    fprintf(stderr, "[%s] AddRange(%.*s, %" PRIx64 ", %" PRIx64 ", %" PRIx64
            ", %" PRIx64 ")\n",
            GetDataSourceLabel(data_source_), (int)name.size(), name.data(),
            vmaddr, vmsize, fileoff, filesize);
  }
  for (auto& pair : outputs_) {
    const std::string label = pair.second->Munge(name);
    uint64_t common = std::min(vmsize, filesize);

    pair.first->vm_map.AddDualRange(vmaddr, common, fileoff, label);
    pair.first->file_map.AddDualRange(fileoff, common, vmaddr, label);

    pair.first->vm_map.AddRange(vmaddr + common, vmsize - common, label);
    pair.first->file_map.AddRange(fileoff + common, filesize - common, label);
  }
}


// ThreadSafeIterIndex /////////////////////////////////////////////////////////

class ThreadSafeIterIndex {
 public:
  ThreadSafeIterIndex(int max) : index_(0), max_(max) {}

  bool TryGetNext(int* index) {
    int ret = index_.fetch_add(1, std::memory_order_relaxed);
    if (ret >= max_) {
      return false;
    } else {
      *index = ret;
      return true;
    }
  }

  void Abort(string_view error) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_ = max_;
    error_ = std::string(error);
  }

  bool TryGetError(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) {
      return false;
    } else {
      *error = error_;
      return true;
    }
  }

 private:
  std::atomic<int> index_;
  std::string error_;
  std::mutex mutex_;
  const int max_;
};


// Bloaty //////////////////////////////////////////////////////////////////////

// Represents a program execution and associated state.

struct ConfiguredDataSource {
  ConfiguredDataSource(const DataSourceDefinition& definition_)
      : definition(definition_),
        effective_source(definition_.number),
        munger(new NameMunger()) {}

  const DataSourceDefinition& definition;
  // This will differ from definition.number for kSymbols, where we use the
  // --demangle flag to set the true/effective source.
  DataSource effective_source;
  std::unique_ptr<NameMunger> munger;
};

class Bloaty {
 public:
  Bloaty(const InputFileFactory& factory, const Options& options);

  void AddFilename(const std::string& filename, bool base_file);

  size_t GetSourceCount() const {
    return sources_.size() + (filename_position_ >= 0 ? 1 : 0);
  }

  void DefineCustomDataSource(const CustomDataSource& source);

  void AddDataSource(const std::string& name);
  void ScanAndRollup(const Options& options, RollupOutput* output);
  void DisassembleFunction(string_view function, const Options& options,
                           RollupOutput* output);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Bloaty);

  template <size_t T>
  void AddBuiltInSources(const DataSourceDefinition (&sources)[T],
                         const Options& options) {
    for (size_t i = 0; i < T; i++) {
      const DataSourceDefinition& source = sources[i];
      auto configured_source = absl::make_unique<ConfiguredDataSource>(source);

      if (configured_source->effective_source == DataSource::kSymbols) {
        configured_source->effective_source = EffectiveSymbolSource(options);
      }

      all_known_sources_[source.name] = std::move(configured_source);
    }
  }

  static DataSource EffectiveSymbolSource(const Options& options) {
    switch (options.demangle()) {
      case Options::DEMANGLE_NONE:
        return DataSource::kRawSymbols;
      case Options::DEMANGLE_SHORT:
        return DataSource::kShortSymbols;
      case Options::DEMANGLE_FULL:
        return DataSource::kFullSymbols;
      default:
        BLOATY_UNREACHABLE();
    }
  }

  void ScanAndRollupFiles(const std::vector<std::unique_ptr<ObjectFile>>& files,
                          Rollup* rollup) const;
  void ScanAndRollupFile(ObjectFile* file, Rollup* rollup) const;

  const InputFileFactory& file_factory_;

  // All data sources, indexed by name.
  // Contains both built-in sources and custom sources.
  std::map<std::string, std::unique_ptr<ConfiguredDataSource>>
      all_known_sources_;

  // Sources the user has actually selected, in the order selected.
  // Points to entries in all_known_sources_.
  std::vector<ConfiguredDataSource*> sources_;
  std::vector<std::string> source_names_;

  std::vector<std::unique_ptr<ObjectFile>> input_files_;
  std::vector<std::unique_ptr<ObjectFile>> base_files_;
  int filename_position_;
};

Bloaty::Bloaty(const InputFileFactory& factory, const Options& options)
    : file_factory_(factory), filename_position_(-1) {
  AddBuiltInSources(data_sources, options);
}

void Bloaty::AddFilename(const std::string& filename, bool is_base) {
  std::unique_ptr<InputFile> file(file_factory_.OpenFile(filename));
  auto object_file = TryOpenELFFile(file);

  if (!object_file.get()) {
    object_file = TryOpenMachOFile(file);
  }

  if (!object_file.get()) {
    THROWF("unknown file type for file '$0'", filename.c_str());
  }

  if (is_base) {
    base_files_.push_back(std::move(object_file));
  } else {
    input_files_.push_back(std::move(object_file));
  }
}

void Bloaty::DefineCustomDataSource(const CustomDataSource& source) {
  if (source.base_data_source() == "symbols") {
    THROW(
        "For custom data sources, use one of {rawsymbols, shortsymbols, "
        "fullsymbols} for base_data_source instead of 'symbols', so you aren't "
        "sensitive to the --demangle parameter.");
  }

  auto iter = all_known_sources_.find(source.base_data_source());

  if (iter == all_known_sources_.end()) {
    THROWF("custom data source '$0': no such base source '$1'", source.name(),
           source.base_data_source());
  } else if (!iter->second->munger->IsEmpty()) {
    THROWF("custom data source '$0' tries to depend on custom data source '$1'",
           source.name(), source.base_data_source());
  }

  all_known_sources_[source.name()] =
      absl::make_unique<ConfiguredDataSource>(iter->second->definition);
  NameMunger* munger = all_known_sources_[source.name()]->munger.get();
  for (const auto& regex : source.rewrite()) {
    munger->AddRegex(regex.pattern(), regex.replacement());
  }
}

void Bloaty::AddDataSource(const std::string& name) {
  source_names_.emplace_back(name);

  if (name == "inputfiles") {
    filename_position_ = sources_.size() + 1;
    return;
  }

  auto it = all_known_sources_.find(name);
  if (it == all_known_sources_.end()) {
    THROWF("no such data source: $0", name);
  }

  sources_.emplace_back(it->second.get());
}

// All of the DualMaps for a given file.
struct DualMaps {
 public:
  DualMaps() {
    // Base map.
    AppendMap();
  }

  DualMap* AppendMap() {
    maps_.emplace_back(new DualMap);
    return maps_.back().get();
  }

  void ComputeRollup(const std::string& filename, int filename_position,
                     Rollup* rollup) {
    RangeMap::ComputeRollup(VmMaps(), filename, filename_position,
                            [=](const std::vector<std::string>& keys,
                                uint64_t addr, uint64_t end) {
                              return rollup->AddSizes(keys, end - addr, true);
                            });
    RangeMap::ComputeRollup(FileMaps(), filename, filename_position,
                            [=](const std::vector<std::string>& keys,
                                uint64_t addr, uint64_t end) {
                              return rollup->AddSizes(keys, end - addr,
                                                      false);
                            });
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
    PrintMaps(FileMaps(), filename, filename_position);
  }

  void PrintVMMaps(const std::string& filename, int filename_position) {
    PrintMaps(VmMaps(), filename, filename_position);
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

  void PrintMapRow(string_view str, uint64_t start, uint64_t end) {
    printf("[%" PRIx64 ", %" PRIx64 "] %.*s\n", start, end, (int)str.size(),
           str.data());
  }

  DualMap* base_map() { return maps_[0].get(); }

 private:
  std::vector<const RangeMap*> VmMaps() const {
    std::vector<const RangeMap*> ret;
    for (const auto& map : maps_) {
      ret.push_back(&map->vm_map);
    }
    return ret;
  }

  std::vector<const RangeMap*> FileMaps() const {
    std::vector<const RangeMap*> ret;
    for (const auto& map : maps_) {
      ret.push_back(&map->file_map);
    }
    return ret;
  }

  std::vector<std::unique_ptr<DualMap>> maps_;
};

void Bloaty::ScanAndRollupFile(ObjectFile* file, Rollup* rollup) const {
  const std::string& filename = file->file_data().filename();
  DualMaps maps;
  RangeSink sink(&file->file_data(), DataSource::kSegments, nullptr);
  NameMunger empty_munger;
  sink.AddOutput(maps.base_map(), &empty_munger);
  file->ProcessBaseMap(&sink);
  maps.base_map()->file_map.AddRange(0, file->file_data().data().size(),
                                     "[None]");

  std::vector<std::unique_ptr<RangeSink>> sinks;
  std::vector<RangeSink*> sink_ptrs;

  for (auto source : sources_) {
    sinks.push_back(absl::make_unique<RangeSink>(
        &file->file_data(), source->effective_source, maps.base_map()));
    sinks.back()->AddOutput(maps.AppendMap(), source->munger.get());
    sink_ptrs.push_back(sinks.back().get());
  }

  file->ProcessFile(sink_ptrs);

  maps.ComputeRollup(filename, filename_position_, rollup);
  if (verbose_level > 0) {
    fprintf(stderr, "FILE MAP:\n");
    maps.PrintFileMaps(filename, filename_position_);
    fprintf(stderr, "VM MAP:\n");
    maps.PrintVMMaps(filename, filename_position_);
  }
}

void Bloaty::ScanAndRollupFiles(
    const std::vector<std::unique_ptr<ObjectFile>>& files,
    Rollup* rollup) const {
  int num_cpus = std::thread::hardware_concurrency();
  int num_threads = std::min(num_cpus, static_cast<int>(files.size()));
  std::vector<Rollup> rollups(num_threads);
  std::vector<std::thread> threads(num_threads);
  ThreadSafeIterIndex index(files.size());

  for (int i = 0; i < num_threads; i++) {
    threads[i] = std::thread([this, &index, &files](Rollup* r) {
      try {
        int j;
        while (index.TryGetNext(&j)) {
          ScanAndRollupFile(files[j].get(), r);
        }
      } catch (const bloaty::Error& e) {
        index.Abort(e.what());
      }
    }, &rollups[i]);
  }

  for (int i = 0; i < num_threads; i++) {
    threads[i].join();
    if (i == 0) {
      *rollup = std::move(rollups[i]);
    } else {
      rollup->Add(rollups[i]);
    }
  }

  std::string error;
  if (index.TryGetError(&error)) {
    THROW(error.c_str());
  }
}

void Bloaty::ScanAndRollup(const Options& options, RollupOutput* output) {
  if (input_files_.empty()) {
    THROW("no filename specified");
  }

  for (const auto& name : source_names_) {
    output->AddDataSourceName(name);
  }

  Rollup rollup;
  ScanAndRollupFiles(input_files_, &rollup);

  if (!base_files_.empty()) {
    Rollup base;
    ScanAndRollupFiles(base_files_, &base);
    rollup.Subtract(base);
    rollup.CreateDiffModeRollupOutput(&base, options, output);
  } else {
    rollup.CreateRollupOutput(options, output);
  }
}

void Bloaty::DisassembleFunction(string_view function, const Options& options,
                                 RollupOutput* output) {
  DisassemblyInfo info;
  for (auto& file : input_files_) {
    if (file->GetDisassemblyInfo(function, EffectiveSymbolSource(options),
                                 &info)) {
      output->SetDisassembly(::bloaty::DisassembleFunction(info));
      return;
    }
  }

  THROWF("Couldn't find function $0 to disassemble", function);
}

const char usage[] = R"(Bloaty McBloatface: a size profiler for binaries.

USAGE: bloaty [options] file... [-- base_file...]

Options:

  --csv              Output in CSV format instead of human-readable.
  -c <file>          Load configuration from <file>.
  -d <sources>       Comma-separated list of sources to scan.
  -C <mode>          How to demangle symbols.  Possible values are:
  --demangle=<mode>    --demangle=none   no demangling, print raw symbols
                       --demangle=short  demangle, but omit arg/return types
                       --demangle=full   print full demangled type
                     The default is --demangle=short.
  --disassemble=<function>
                     Disassemble this function (EXPERIMENTAL)
  -n <num>           How many rows to show per level before collapsing
                     other keys into '[Other]'.  Set to '0' for unlimited.
                     Defaults to 20.
  -s <sortby>        Whether to sort by VM or File size.  Possible values
                     are:
                       -s vm
                       -s file
                       -s both (the default: sorts by max(vm, file)).
  -v                 Verbose output.  Dumps warnings encountered during
                     processing and full VM/file maps at the end.
                     Add more v's (-vv, -vvv) for even more.
  -w                 Wide output; don't truncate long labels.
  --help             Display this message and exit.
  --list-sources     Show a list of available sources and exit.
)";

class ArgParser {
 public:
  ArgParser(int* argc, char** argv[])
      : argc_(*argc),
        argv_(*argv, *argv + *argc),
        out_argc_(argc),
        out_argv_(argv) {
    *out_argc_ = 0;
    ConsumeAndSaveArg();  // Executable name.
  }

  bool IsDone() { return index_ == argc_; }

  string_view Arg() {
    assert(!IsDone());
    return string_view(argv_[index_]);
  }

  string_view ConsumeArg() {
    string_view ret = Arg();
    index_++;
    return ret;
  }

  void ConsumeAndSaveArg() {
    (*out_argv_)[(*out_argc_)++] = argv_[index_++];
  }

  // Singular flag like --csv or -v.
  bool TryParseFlag(string_view flag) {
    if (Arg() == flag) {
      ConsumeArg();
      return true;
    } else {
      return false;
    }
  }

  // Option taking an argument, for example:
  //   -n 20
  //   --config=file.bloaty
  //
  // For --long-options we accept both:
  //   --long_option value
  //   --long_option=value
  bool TryParseOption(string_view flag, string_view* val) {
    assert(flag.size() > 1);
    bool is_long = flag[1] == '-';
    string_view arg = Arg();
    if (TryParseFlag(flag)) {
      if (IsDone()) {
        THROWF("option '$0' requires an argument", flag);
      }
      *val = ConsumeArg();
      return true;
    } else if (is_long && absl::ConsumePrefix(&arg, std::string(flag) + "=")) {
      *val = arg;
      index_++;
      return true;
    } else {
      return false;
    }
  }

  bool TryParseIntegerOption(string_view flag, int* val) {
    string_view val_str;
    return TryParseOption(flag, &val_str) && absl::SimpleAtoi(val_str, val);
  }

 public:
  int argc_;
  std::vector<char*> argv_;
  int* out_argc_;
  char*** out_argv_;
  int index_ = 0;
};

bool DoParseOptions(bool skip_unknown, int* argc, char** argv[],
                    Options* options, OutputOptions* output_options) {
  bool saw_separator = false;
  ArgParser args(argc, argv);
  string_view option;
  int int_option;

  while (!args.IsDone()) {
    if (args.TryParseFlag("--")) {
      if (saw_separator) {
        THROW("'--' option should only be specified once");
      }
      saw_separator = true;
    } else if (args.TryParseFlag("--csv")) {
      output_options->output_format = OutputFormat::kCSV;
    } else if (args.TryParseOption("-c", &option)) {
      std::ifstream input_file(std::string(option), std::ios::in);
      if (!input_file.is_open()) {
        THROWF("couldn't open file $0", option);
      }
      google::protobuf::io::IstreamInputStream stream(&input_file);
      if (!google::protobuf::TextFormat::Merge(&stream, options)) {
        THROWF("error parsing configuration out of file $0", option);
      }
    } else if (args.TryParseOption("-d", &option)) {
      std::vector<std::string> names = absl::StrSplit(option, ',');
      for (const auto& name : names) {
        options->add_data_source(name);
      }
    } else if (args.TryParseOption("-C", &option) ||
               args.TryParseOption("--demangle", &option)) {
      if (option == "none") {
        options->set_demangle(Options::DEMANGLE_NONE);
      } else if (option == "short") {
        options->set_demangle(Options::DEMANGLE_SHORT);
      } else if (option == "full") {
        options->set_demangle(Options::DEMANGLE_FULL);
      } else {
        THROWF("unknown value for --demangle: $0", option);
      }
    } else if (args.TryParseOption("--disassemble", &option)) {
      options->mutable_disassemble_function()->assign(std::string(option));
    } else if (args.TryParseIntegerOption("-n", &int_option)) {
      options->set_max_rows_per_level(int_option);
    } else if (args.TryParseOption("-s", &option)) {
      if (option == "vm") {
        options->set_sort_by(Options::SORTBY_VMSIZE);
      } else if (option == "file") {
        options->set_sort_by(Options::SORTBY_FILESIZE);
      } else if (option == "both") {
        options->set_sort_by(Options::SORTBY_BOTH);
      } else {
        THROWF("unknown value for -s: $0", option);
      }
    } else if (args.TryParseFlag("-v")) {
      options->set_verbose_level(1);
    } else if (args.TryParseFlag("-vv")) {
      options->set_verbose_level(2);
    } else if (args.TryParseFlag("-vvv")) {
      options->set_verbose_level(3);
    } else if (args.TryParseFlag("-w")) {
      output_options->max_label_len = SIZE_MAX;
    } else if (args.TryParseFlag("--list-sources")) {
      for (const auto& source : data_sources) {
        fprintf(stderr, "%s %s\n", FixedWidthString(source.name, 15).c_str(),
                source.description);
      }
      return false;
    } else if (args.TryParseFlag("--help")) {
      fputs(usage, stderr);
      return false;
    } else if (absl::StartsWith(args.Arg(), "-")) {
      if (skip_unknown) {
        args.ConsumeAndSaveArg();
      } else {
        THROWF("Unknown option: $0", args.Arg());
      }
    } else {
      if (saw_separator) {
        options->add_base_filename(std::string(args.ConsumeArg()));
      } else {
        options->add_filename(std::string(args.ConsumeArg()));
      }
    }
  }

  if (options->data_source_size() == 0 &&
      !options->has_disassemble_function()) {
    // Default when no sources are specified.
    options->add_data_source("sections");
  }

  return true;
}

bool ParseOptions(bool skip_unknown, int* argc, char** argv[], Options* options,
                  OutputOptions* output_options, std::string* error) {
  try {
    return DoParseOptions(skip_unknown, argc, argv, options, output_options);
  } catch (const bloaty::Error& e) {
    error->assign(e.what());
    return false;
  }
}

void BloatyDoMain(const Options& options, const InputFileFactory& file_factory,
                  RollupOutput* output) {
  bloaty::Bloaty bloaty(file_factory, options);

  if (options.filename_size() == 0) {
    THROW("must specify at least one file");
  }

  if (options.max_rows_per_level() < 1) {
    THROW("max_rows_per_level must be at least 1");
  }

  for (auto& filename : options.filename()) {
    bloaty.AddFilename(filename, false);
  }

  for (auto& base_filename : options.base_filename()) {
    bloaty.AddFilename(base_filename, true);
  }

  for (const auto& custom_data_source : options.custom_data_source()) {
    bloaty.DefineCustomDataSource(custom_data_source);
  }

  for (const auto& data_source : options.data_source()) {
    bloaty.AddDataSource(data_source);
  }

  verbose_level = options.verbose_level();

  if (options.data_source_size() > 0) {
    bloaty.ScanAndRollup(options, output);
  } else if (options.has_disassemble_function()) {
    bloaty.DisassembleFunction(options.disassemble_function(), options, output);
  }
}

bool BloatyMain(const Options& options, const InputFileFactory& file_factory,
                RollupOutput* output, std::string* error) {
  try {
    BloatyDoMain(options, file_factory, output);
    return true;
  } catch (const bloaty::Error& e) {
    error->assign(e.what());
    return false;
  }
}

}  // namespace bloaty
