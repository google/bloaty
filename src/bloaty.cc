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

#include <stddef.h>

// For some reason this isn't getting defined by zconf.h in 32-bit builds.
// It's very hard to figure out why. For the moment this seems to fix it,
// but ideally we'd have a better solution here.
typedef size_t z_size_t;
#include <zlib.h>

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
#if !defined(_MSC_VER)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <Windows.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include "absl/debugging/internal/demangle.h"
#include "absl/memory/memory.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "absl/strings/str_join.h"
#include "absl/strings/substitute.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/text_format.h"

#include "bloaty.h"
#include "bloaty.pb.h"
#include "re.h"
#include "util.h"

using absl::string_view;

namespace bloaty {

// Use a global since we would have to plumb it through so many call-stacks
// otherwise.  We would make this thread_local but that's not supported on OS X
// right now.
int verbose_level = 0;
ShowDomain show = ShowDomain::kShowBoth;

struct DataSourceDefinition {
  DataSource number;
  const char* name;
  const char* description;
};

constexpr DataSourceDefinition data_sources[] = {
    {DataSource::kArchiveMembers, "armembers", "the .o files in a .a file"},
    {DataSource::kCompileUnits, "compileunits",
     "source file for the .o file (translation unit). requires debug info."},
    {DataSource::kInputFiles, "inputfiles",
     "the filename specified on the Bloaty command-line"},
    {DataSource::kInlines, "inlines",
     "source line/file where inlined code came from.  requires debug info."},
    {DataSource::kSections, "sections", "object file section"},
    {DataSource::kSegments, "segments", "load commands in the binary"},
    // We require that all symbols sources are >= kSymbols.
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

void CheckedAdd(int64_t* accum, int64_t val) {
#if ABSL_HAVE_BUILTIN(__builtin_add_overflow)
  if (__builtin_add_overflow(*accum, val, accum)) {
    THROW("integer overflow");
  }
#else
  bool safe = *accum < 0
                  ? (val >= std::numeric_limits<int64_t>::max() - *accum)
                  : (val <= std::numeric_limits<int64_t>::max() - *accum);
  if (!safe) {
    THROW("integer overflow");
  }
  *accum += val;
#endif
}

static std::string CSVEscape(string_view str) {
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

extern "C" char* __cxa_demangle(const char* mangled_name, char* buf, size_t* n,
                                int* status);

std::string ItaniumDemangle(string_view symbol, DataSource source) {
  if (source != DataSource::kShortSymbols &&
      source != DataSource::kFullSymbols) {
    // No demangling.
    return std::string(symbol);
  }

  string_view demangle_from = symbol;
  if (absl::StartsWith(demangle_from, "__Z")) {
    demangle_from.remove_prefix(1);
  }

  if (source == DataSource::kShortSymbols) {
    char demangled[1024];
    if (absl::debugging_internal::Demangle(demangle_from.data(), demangled,
                                           sizeof(demangled))) {
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
    printf("Unexpected source: %d\n", (int)source);
    BLOATY_UNREACHABLE();
  }
}


// NameMunger //////////////////////////////////////////////////////////////////

void NameMunger::AddRegex(const std::string& regex, const std::string& replacement) {
  auto reg = absl::make_unique<ReImpl>(regex);
  regexes_.push_back(std::make_pair(std::move(reg), replacement));
}

std::string NameMunger::Munge(string_view name) const {
  std::string name_str(name);
  std::string ret(name);

  for (const auto& pair : regexes_) {
    if (ReImpl::Extract(name_str, *pair.first, pair.second, &ret)) {
      return ret;
    }
  }

  return name_str;
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
  Rollup(const Rollup&) = delete;
  Rollup& operator=(const Rollup&) = delete;

  Rollup(Rollup&& other) = default;
  Rollup& operator=(Rollup&& other) = default;

  void AddSizes(const std::vector<std::string>& names,
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
    row->filtered_vmsize = filtered_vm_total_;
    row->filtered_filesize = filtered_file_total_;
    row->vmpercent = 100;
    row->filepercent = 100;
    output->diff_mode_ = true;
    CreateRows(row, base, options, true);
  }

  void SetFilterRegex(const ReImpl* regex) {
    filter_regex_ = regex;
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

  int64_t file_total() const { return file_total_; }
  int64_t filtered_file_total() const { return filtered_file_total_; }

 private:
  int64_t vm_total_ = 0;
  int64_t file_total_ = 0;
  int64_t filtered_vm_total_ = 0;
  int64_t filtered_file_total_ = 0;

  const ReImpl* filter_regex_ = nullptr;

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
  void AddInternal(const std::vector<std::string>& names, size_t i,
                   uint64_t size, bool is_vmsize) {
    if (filter_regex_ != nullptr) {
      // filter_regex_ is only set in the root rollup, which checks the full
      // label hierarchy for a match to determine whether a region should be
      // considered.
      bool any_matched = false;

      for (const auto& name : names) {
        if (ReImpl::PartialMatch(name, *filter_regex_)) {
          any_matched = true;
          break;
        }
      }

      if (!any_matched) {
        // Ignore this region in the rollup and don't visit sub-rollups.
        if (is_vmsize) {
          CheckedAdd(&filtered_vm_total_, size);
        } else {
          CheckedAdd(&filtered_file_total_, size);
        }
        return;
      }
    }

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

  static double Percent(int64_t part, int64_t whole) {
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

namespace {

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

bool ShowFile(const OutputOptions& options) {
  return options.show != ShowDomain::kShowVM;
}

bool ShowVM(const OutputOptions& options) {
  return options.show != ShowDomain::kShowFile;
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

std::string SiPrint(int64_t size, bool force_sign) {
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
    ret = std::to_string(static_cast<int64_t>(size_d)) + prefixes[n];
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

}  // namespace

void RollupOutput::Print(const OutputOptions& options, std::ostream* out) {
  if (!source_names_.empty()) {
    switch (options.output_format) {
      case bloaty::OutputFormat::kPrettyPrint:
        PrettyPrint(options, out);
        break;
      case bloaty::OutputFormat::kCSV:
        PrintToCSV(out, /*tabs=*/false);
        break;
      case bloaty::OutputFormat::kTSV:
        PrintToCSV(out, /*tabs=*/true);
        break;
      default:
        BLOATY_UNREACHABLE();
    }
  }

  if (!disassembly_.empty()) {
    *out << disassembly_;
  }
}

void RollupOutput::PrettyPrintRow(const RollupRow& row, size_t indent,
                                  const OutputOptions& options,
                                  std::ostream* out) const {
  if (&row != &toplevel_row_) {
    // Avoid printing this row if it is only zero.
    // This can happen when using --domain if the row is zero for this domain.
    if ((!ShowFile(options) && row.vmsize == 0) ||
        (!ShowVM(options) && row.filesize == 0)) {
      return;
    }
  }

  *out << FixedWidthString("", indent) << " ";

  if (ShowFile(options)) {
    *out << PercentString(row.filepercent, diff_mode_) << " "
         << SiPrint(row.filesize, diff_mode_) << " ";
  }

  if (ShowVM(options)) {
    *out << PercentString(row.vmpercent, diff_mode_) << " "
         << SiPrint(row.vmsize, diff_mode_) << " ";
  }

  *out << "   " << row.name << "\n";
}

bool RollupOutput::IsSame(const std::string& a, const std::string& b) {
  if (a == b) {
    return true;
  }

  if (absl::EndsWith(b, a + "]") || absl::EndsWith(a, b + "]")) {
    return true;
  }

  return false;
}

void RollupOutput::PrettyPrintTree(const RollupRow& row, size_t indent,
                                   const OutputOptions& options,
                                   std::ostream* out) const {
  // Rows are printed before their sub-rows.
  PrettyPrintRow(row, indent, options, out);

  if (!row.vmsize && !row.filesize) {
    return;
  }

  if (row.sorted_children.size() == 1 &&
      row.sorted_children[0].sorted_children.size() == 0 &&
      IsSame(row.name, row.sorted_children[0].name)) {
    return;
  }

  for (const auto& child : row.sorted_children) {
    PrettyPrintTree(child, indent + 2, options, out);
  }
}

void RollupOutput::PrettyPrint(const OutputOptions& options,
                               std::ostream* out) const {
  if (ShowFile(options)) {
    *out << "    FILE SIZE   ";
  }

  if (ShowVM(options)) {
    *out << "     VM SIZE    ";
  }

  *out << "\n";

  if (ShowFile(options)) {
    *out << " -------------- ";
  }

  if (ShowVM(options)) {
    *out << " -------------- ";
  }

  *out << "\n";

  for (const auto& child : toplevel_row_.sorted_children) {
    PrettyPrintTree(child, 0, options, out);
  }

  // The "TOTAL" row comes after all other rows.
  PrettyPrintRow(toplevel_row_, 0, options, out);

  uint64_t file_filtered = 0;
  uint64_t vm_filtered = 0;
  if (ShowFile(options)) {
    file_filtered = toplevel_row_.filtered_filesize;
  }
  if (ShowVM(options)) {
    vm_filtered = toplevel_row_.filtered_vmsize;
  }

  if (vm_filtered == 0 && file_filtered == 0) {
    return;
  }

  *out << "Filtering enabled (source_filter); omitted";

  if (file_filtered > 0 && vm_filtered > 0) {
    *out << " file =" << SiPrint(file_filtered, /*force_sign=*/false)
         << ", vm =" << SiPrint(vm_filtered, /*force_sign=*/false);
  } else if (file_filtered > 0) {
    *out << SiPrint(file_filtered, /*force_sign=*/false);
  } else {
    *out << SiPrint(vm_filtered, /*force_sign=*/false);
  }

   *out << " of entries\n";
}

void RollupOutput::PrintRowToCSV(const RollupRow& row,
                                 std::vector<std::string> parent_labels,
                                 std::ostream* out, bool tabs) const {
  while (parent_labels.size() < source_names_.size()) {
    // If this label had no data at this level, append an empty string.
    parent_labels.push_back("");
  }

  parent_labels.push_back(std::to_string(row.vmsize));
  parent_labels.push_back(std::to_string(row.filesize));

  std::string sep = tabs ? "\t" : ",";
  *out << absl::StrJoin(parent_labels, sep) << "\n";
}

void RollupOutput::PrintTreeToCSV(const RollupRow& row,
                                  std::vector<std::string> parent_labels,
                                  std::ostream* out, bool tabs) const {
  if (tabs) {
    parent_labels.push_back(row.name);
  } else {
    parent_labels.push_back(CSVEscape(row.name));
  }

  if (row.sorted_children.size() > 0) {
    for (const auto& child_row : row.sorted_children) {
      PrintTreeToCSV(child_row, parent_labels, out, tabs);
    }
  } else {
    PrintRowToCSV(row, parent_labels, out, tabs);
  }
}

void RollupOutput::PrintToCSV(std::ostream* out, bool tabs) const {
  std::vector<std::string> names(source_names_);
  names.push_back("vmsize");
  names.push_back("filesize");
  std::string sep = tabs ? "\t" : ",";
  *out << absl::StrJoin(names, sep) << "\n";
  for (const auto& child_row : toplevel_row_.sorted_children) {
    PrintTreeToCSV(child_row, std::vector<std::string>(), out, tabs);
  }
}

// RangeMap ////////////////////////////////////////////////////////////////////

constexpr uint64_t RangeSink::kUnknownSize;


// MmapInputFile ///////////////////////////////////////////////////////////////

#if !defined(_MSC_VER)
class MmapInputFile : public InputFile {
 public:
  MmapInputFile(const std::string& filename);
  MmapInputFile(const MmapInputFile&) = delete;
  MmapInputFile& operator=(const MmapInputFile&) = delete;
  ~MmapInputFile() override;
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

#else // !_MSC_VER

// MmapInputFile ///////////////////////////////////////////////////////////////

class Win32MMapInputFile : public InputFile {
 public:
  Win32MMapInputFile(const std::string& filename);
  Win32MMapInputFile(const Win32MMapInputFile&) = delete;
  Win32MMapInputFile& operator=(const Win32MMapInputFile&) = delete;
  ~Win32MMapInputFile() override;
};

class Win32Handle {
 public:
  Win32Handle(HANDLE h) : h_(h) {}

  ~Win32Handle() {
    if (h_ && h_ != INVALID_HANDLE_VALUE && !CloseHandle(h_)) {
      fprintf(stderr, "bloaty: error calling CloseHandle(): %d\n",
              GetLastError());
    }
  }

  HANDLE h() { return h_; }

 private:
  HANDLE h_;
};

Win32MMapInputFile::Win32MMapInputFile(const std::string& filename)
    : InputFile(filename) {
  Win32Handle fd(::CreateFileA(filename.c_str(), FILE_GENERIC_READ,
                               FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL));
  LARGE_INTEGER li = {};
  const char* map;

  if (fd.h() == INVALID_HANDLE_VALUE) {
    THROWF("couldn't open file '$0': $1", filename, ::GetLastError());
  }

  if (!::GetFileSizeEx(fd.h(), &li)) {
    THROWF("couldn't stat file '$0': $1", filename, ::GetLastError());
  }

  Win32Handle mapfd(
      ::CreateFileMappingA(fd.h(), NULL, PAGE_READONLY, 0, 0, nullptr));
  if (!mapfd.h()) {
    THROWF("couldn't create file mapping '$0': $1", filename, ::GetLastError());
  }

  map = static_cast<char*>(::MapViewOfFile(mapfd.h(), FILE_MAP_READ, 0, 0, 0));
  if (!map) {
    THROWF("couldn't MapViewOfFile file '$0': $1", filename, ::GetLastError());
  }

  data_ = string_view(map, li.QuadPart);
}

Win32MMapInputFile::~Win32MMapInputFile() {
  if (data_.data() != nullptr && !::UnmapViewOfFile(data_.data())) {
    fprintf(stderr, "bloaty: error calling UnmapViewOfFile(): %d\n",
            ::GetLastError());
  }
}

std::unique_ptr<InputFile> MmapInputFileFactory::OpenFile(
    const std::string& filename) const {
  return absl::make_unique<Win32MMapInputFile>(filename);
}

#endif

// RangeSink ///////////////////////////////////////////////////////////////////

RangeSink::RangeSink(const InputFile *file, const Options &options,
                     DataSource data_source, const DualMap *translator,
                     google::protobuf::Arena *arena)
    : file_(file), options_(options), data_source_(data_source),
      translator_(translator), arena_(arena) {}

RangeSink::~RangeSink() {}

uint64_t debug_vmaddr = -1;
uint64_t debug_fileoff = -1;

bool RangeSink::ContainsVerboseVMAddr(uint64_t vmaddr, uint64_t vmsize) {
  return options_.verbose_level() > 1 ||
         (options_.has_debug_vmaddr() && options_.debug_vmaddr() >= vmaddr &&
          options_.debug_vmaddr() < (vmaddr + vmsize));
}

bool RangeSink::ContainsVerboseFileOffset(uint64_t fileoff, uint64_t filesize) {
  return options_.verbose_level() > 1 ||
         (options_.has_debug_fileoff() && options_.debug_fileoff() >= fileoff &&
          options_.debug_fileoff() < (fileoff + filesize));
}

bool RangeSink::IsVerboseForVMRange(uint64_t vmaddr, uint64_t vmsize) {
  if (vmsize == RangeMap::kUnknownSize) {
    vmsize = UINT64_MAX - vmaddr;
  }

  if (vmaddr + vmsize < vmaddr) {
    THROWF("Overflow in vm range, vmaddr=$0, vmsize=$1", vmaddr, vmsize);
  }

  if (ContainsVerboseVMAddr(vmaddr, vmsize)) {
    return true;
  }

  if (translator_ && options_.has_debug_fileoff()) {
    RangeMap vm_map;
    RangeMap file_map;
    bool contains = false;
    vm_map.AddRangeWithTranslation(vmaddr, vmsize, "", translator_->vm_map,
                                   false, &file_map);
    file_map.ForEachRange(
        [this, &contains](uint64_t fileoff, uint64_t filesize) {
          if (ContainsVerboseFileOffset(fileoff, filesize)) {
            contains = true;
          }
        });
    return contains;
  }

  return false;
}

bool RangeSink::IsVerboseForFileRange(uint64_t fileoff, uint64_t filesize) {
  if (filesize == RangeMap::kUnknownSize) {
    filesize = UINT64_MAX - fileoff;
  }

  if (fileoff + filesize < fileoff) {
    THROWF("Overflow in file range, fileoff=$0, filesize=$1", fileoff,
           filesize);
  }

  if (ContainsVerboseFileOffset(fileoff, filesize)) {
    return true;
  }

  if (translator_ && options_.has_debug_vmaddr()) {
    RangeMap vm_map;
    RangeMap file_map;
    bool contains = false;
    file_map.AddRangeWithTranslation(fileoff, filesize, "",
                                     translator_->file_map, false, &vm_map);
    vm_map.ForEachRange([this, &contains](uint64_t vmaddr, uint64_t vmsize) {
      if (ContainsVerboseVMAddr(vmaddr, vmsize)) {
        contains = true;
      }
    });
    return contains;
  }

  return false;
}

void RangeSink::AddOutput(DualMap* map, const NameMunger* munger) {
  outputs_.push_back(std::make_pair(map, munger));
}

void RangeSink::AddFileRange(const char* analyzer, string_view name,
                             uint64_t fileoff, uint64_t filesize) {
  bool verbose = IsVerboseForFileRange(fileoff, filesize);
  if (verbose) {
    printf("[%s, %s] AddFileRange(%.*s, %" PRIx64 ", %" PRIx64 ")\n",
           GetDataSourceLabel(data_source_), analyzer, (int)name.size(),
           name.data(), fileoff, filesize);
  }
  for (auto& pair : outputs_) {
    const std::string label = pair.second->Munge(name);
    if (translator_) {
      bool ok = pair.first->file_map.AddRangeWithTranslation(
          fileoff, filesize, label, translator_->file_map, verbose,
          &pair.first->vm_map);
      if (!ok) {
        WARN("File range ($0, $1) for label $2 extends beyond base map",
             fileoff, filesize, name);
      }
    } else {
      pair.first->file_map.AddRange(fileoff, filesize, label);
    }
  }
}

void RangeSink::AddFileRangeForVMAddr(const char* analyzer,
                                      uint64_t label_from_vmaddr,
                                      string_view file_range) {
  uint64_t file_offset = file_range.data() - file_->data().data();
  bool verbose = IsVerboseForFileRange(file_offset, file_range.size());
  if (verbose) {
    printf("[%s, %s] AddFileRangeForVMAddr(%" PRIx64 ", [%" PRIx64 ", %zx])\n",
           GetDataSourceLabel(data_source_), analyzer, label_from_vmaddr,
           file_offset, file_range.size());
  }
  assert(translator_);
  for (auto& pair : outputs_) {
    std::string label;
    if (pair.first->vm_map.TryGetLabel(label_from_vmaddr, &label)) {
      bool ok = pair.first->file_map.AddRangeWithTranslation(
          file_offset, file_range.size(), label, translator_->file_map, verbose,
          &pair.first->vm_map);
      if (!ok) {
        WARN("File range ($0, $1) for label $2 extends beyond base map",
             file_offset, file_range.size(), label);
      }
    } else if (verbose_level > 1) {
      printf("No label found for vmaddr %" PRIx64 "\n", label_from_vmaddr);
    }
  }
}

void RangeSink::AddFileRangeForFileRange(const char* analyzer,
                                         absl::string_view from_file_range,
                                         absl::string_view file_range) {
  uint64_t file_offset = file_range.data() - file_->data().data();
  uint64_t from_file_offset = from_file_range.data() - file_->data().data();
  bool verbose = IsVerboseForFileRange(file_offset, file_range.size());
  if (verbose) {
    printf("[%s, %s] AddFileRangeForFileRange([%" PRIx64 ", %zx], [%" PRIx64
           ", %zx])\n",
           GetDataSourceLabel(data_source_), analyzer, from_file_offset,
           from_file_range.size(), file_offset, file_range.size());
  }
  assert(translator_);
  for (auto& pair : outputs_) {
    std::string label;
    if (pair.first->file_map.TryGetLabelForRange(
            from_file_offset, from_file_range.size(), &label)) {
      bool ok = pair.first->file_map.AddRangeWithTranslation(
          file_offset, file_range.size(), label, translator_->file_map, verbose,
          &pair.first->vm_map);
      if (!ok) {
        WARN("File range ($0, $1) for label $2 extends beyond base map",
             file_offset, file_range.size(), label);
      }
    } else if (verbose_level > 1) {
      printf("No label found for file range [%" PRIx64 ", %zx]\n",
             from_file_offset, from_file_range.size());
    }
  }
}

void RangeSink::AddVMRangeForVMAddr(const char* analyzer,
                                    uint64_t label_from_vmaddr, uint64_t addr,
                                    uint64_t size) {
  bool verbose = IsVerboseForVMRange(addr, size);
  if (verbose) {
    printf("[%s, %s] AddVMRangeForVMAddr(%" PRIx64 ", [%" PRIx64 ", %" PRIx64
           "])\n",
           GetDataSourceLabel(data_source_), analyzer, label_from_vmaddr, addr,
           size);
  }
  assert(translator_);
  for (auto& pair : outputs_) {
    std::string label;
    if (pair.first->vm_map.TryGetLabel(label_from_vmaddr, &label)) {
      bool ok = pair.first->vm_map.AddRangeWithTranslation(
          addr, size, label, translator_->vm_map, verbose,
          &pair.first->file_map);
      if (!ok && verbose_level > 1) {
        WARN("VM range ($0, $1) for label $2 extends beyond base map", addr,
             size, label);
      }
    } else if (verbose_level > 1) {
      printf("No label found for vmaddr %" PRIx64 "\n", label_from_vmaddr);
    }
  }
}

void RangeSink::AddVMRange(const char* analyzer, uint64_t vmaddr,
                           uint64_t vmsize, const std::string& name) {
  bool verbose = IsVerboseForVMRange(vmaddr, vmsize);
  if (verbose) {
    printf("[%s, %s] AddVMRange(%.*s, %" PRIx64 ", %" PRIx64 ")\n",
           GetDataSourceLabel(data_source_), analyzer, (int)name.size(),
           name.data(), vmaddr, vmsize);
  }
  assert(translator_);
  for (auto& pair : outputs_) {
    const std::string label = pair.second->Munge(name);
    bool ok = pair.first->vm_map.AddRangeWithTranslation(
        vmaddr, vmsize, label, translator_->vm_map, verbose,
        &pair.first->file_map);
    if (!ok) {
      WARN("VM range ($0, $1) for label $2 extends beyond base map", vmaddr,
           vmsize, name);
    }
  }
}

void RangeSink::AddVMRangeAllowAlias(const char* analyzer, uint64_t vmaddr,
                                     uint64_t size, const std::string& name) {
  // TODO: maybe track alias (but what would we use it for?)
  // TODO: verify that it is in fact an alias.
  AddVMRange(analyzer, vmaddr, size, name);
}

void RangeSink::AddVMRangeIgnoreDuplicate(const char* analyzer, uint64_t vmaddr,
                                          uint64_t vmsize,
                                          const std::string& name) {
  // TODO suppress warning that AddVMRange alone might trigger.
  AddVMRange(analyzer, vmaddr, vmsize, name);
}

void RangeSink::AddRange(const char* analyzer, string_view name,
                         uint64_t vmaddr, uint64_t vmsize, uint64_t fileoff,
                         uint64_t filesize) {
  if (vmsize == RangeMap::kUnknownSize || filesize == RangeMap::kUnknownSize) {
    // AddRange() is used for segments and sections; the mappings that establish
    // the file <-> vm mapping.  The size should always be known.  Moreover it
    // would be unclear how the logic should work if the size was *not* known.
    THROW("AddRange() does not allow unknown size.");
  }

  if (IsVerboseForVMRange(vmaddr, vmsize) ||
      IsVerboseForFileRange(fileoff, filesize)) {
    printf("[%s, %s] AddRange(%.*s, %" PRIx64 ", %" PRIx64 ", %" PRIx64
           ", %" PRIx64 ")\n",
           GetDataSourceLabel(data_source_), analyzer, (int)name.size(),
           name.data(), vmaddr, vmsize, fileoff, filesize);
  }

  if (translator_) {
    if (!translator_->vm_map.CoversRange(vmaddr, vmsize) ||
        !translator_->file_map.CoversRange(fileoff, filesize)) {
      THROW("Tried to add range that is not covered by base map.");
    }
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

uint64_t RangeSink::TranslateFileToVM(const char* ptr) {
  assert(translator_);
  uint64_t offset = ptr - file_->data().data();
  uint64_t translated;
  if (!FileContainsPointer(ptr) ||
      !translator_->file_map.Translate(offset, &translated)) {
    THROWF("Can't translate file offset ($0) to VM, contains: $1, map:\n$2",
           offset, FileContainsPointer(ptr),
           translator_->file_map.DebugString().c_str());
  }
  return translated;
}

absl::string_view RangeSink::TranslateVMToFile(uint64_t address) {
  assert(translator_);
  uint64_t translated;
  if (!translator_->vm_map.Translate(address, &translated) ||
      translated > file_->data().size()) {
    THROW("Can't translate VM pointer to file");
  }
  return file_->data().substr(translated);
}

absl::string_view RangeSink::ZlibDecompress(absl::string_view data,
                                            uint64_t uncompressed_size) {
  if (!arena_) {
    THROW("This range sink isn't prepared to zlib decompress.");
  }
  uint64_t mb = 1 << 20;
  // Limit for uncompressed size is 30x the compressed size + 128MB.
  if (uncompressed_size > static_cast<uint64_t>(data.size()) * 30 + (128 * mb)) {
    fprintf(stderr,
            "warning: ignoring compressed debug data, implausible uncompressed "
            "size (compressed: %zu, uncompressed: %" PRIu64 ")\n",
            data.size(), uncompressed_size);
    return absl::string_view();
  }
  unsigned char *dbuf =
      arena_->google::protobuf::Arena::CreateArray<unsigned char>(
          arena_, uncompressed_size);
  uLongf zliblen = uncompressed_size;
  if (uncompress(dbuf, &zliblen, (unsigned char*)(data.data()), data.size()) != Z_OK) {
    THROW("Error decompressing debug info");
  }
  string_view sv(reinterpret_cast<char *>(dbuf), zliblen);
  return sv;
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
  Bloaty(const Bloaty&) = delete;
  Bloaty& operator=(const Bloaty&) = delete;

  void AddFilename(const std::string& filename, bool base_file);
  void AddDebugFilename(const std::string& filename);

  size_t GetSourceCount() const { return sources_.size(); }

  void DefineCustomDataSource(const CustomDataSource& source);

  void AddDataSource(const std::string& name);
  void ScanAndRollup(const Options& options, RollupOutput* output);
  void DisassembleFunction(string_view function, const Options& options,
                           RollupOutput* output);

 private:
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

  void ScanAndRollupFiles(const std::vector<std::string>& filenames,
                          std::vector<std::string>* build_ids,
                          Rollup* rollup) const;
  void ScanAndRollupFile(const std::string& filename, Rollup* rollup,
                         std::vector<std::string>* out_build_ids) const;

  std::unique_ptr<ObjectFile> GetObjectFile(const std::string& filename) const;

  const InputFileFactory& file_factory_;
  const Options options_;

  // All data sources, indexed by name.
  // Contains both built-in sources and custom sources.
  std::map<std::string, std::unique_ptr<ConfiguredDataSource>>
      all_known_sources_;

  // Sources the user has actually selected, in the order selected.
  // Points to entries in all_known_sources_.
  std::vector<ConfiguredDataSource*> sources_;
  std::vector<std::string> source_names_;

  struct InputFileInfo {
    std::string filename_;
    std::string build_id_;
  };
  std::vector<InputFileInfo> input_files_;
  std::vector<InputFileInfo> base_files_;
  std::map<std::string, std::string> debug_files_;

  // For allocating memory, like to decompress compressed sections.
  std::unique_ptr<google::protobuf::Arena> arena_;
};

Bloaty::Bloaty(const InputFileFactory &factory, const Options &options)
    : file_factory_(factory), options_(options),
      arena_(std::make_unique<google::protobuf::Arena>()) {
  AddBuiltInSources(data_sources, options);
}

std::unique_ptr<ObjectFile> Bloaty::GetObjectFile(
    const std::string& filename) const {
  std::unique_ptr<InputFile> file(file_factory_.OpenFile(filename));
  auto object_file = TryOpenELFFile(file);

  if (!object_file.get()) {
    object_file = TryOpenMachOFile(file);
  }

  if (!object_file.get()) {
    object_file = TryOpenWebAssemblyFile(file);
  }

  if (!object_file.get()) {
    object_file = TryOpenPEFile(file);
  }

  if (!object_file.get()) {
    THROWF("unknown file type for file '$0'", filename.c_str());
  }

  return object_file;
}

void Bloaty::AddFilename(const std::string& filename, bool is_base) {
  auto object_file = GetObjectFile(filename);
  std::string build_id = object_file->GetBuildId();

  if (is_base) {
    base_files_.push_back({filename, build_id});
  } else {
    input_files_.push_back({filename, build_id});
  }
}

void Bloaty::AddDebugFilename(const std::string& filename) {
  auto object_file = GetObjectFile(filename);
  std::string build_id = object_file->GetBuildId();
  if (build_id.size() == 0) {
    THROWF("File '$0' has no build ID, cannot be used as a debug file",
           filename);
  }
  debug_files_[build_id] = filename;
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
    THROWF("custom data source '$0': no such base source '$1'.\nTry --list-sources to see valid sources.", source.name(),
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
  auto it = all_known_sources_.find(name);
  if (it == all_known_sources_.end()) {
    THROWF("no such data source: $0.\nTry --list-sources to see valid sources.", name);
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

  void ComputeRollup(Rollup* rollup) {
    for (auto& map : maps_) {
      map->vm_map.Compress();
      map->file_map.Compress();
    }
    RangeMap::ComputeRollup(VmMaps(), [=](const std::vector<std::string>& keys,
                                          uint64_t addr, uint64_t end) {
      return rollup->AddSizes(keys, end - addr, true);
    });
    RangeMap::ComputeRollup(
        FileMaps(),
        [=](const std::vector<std::string>& keys, uint64_t addr, uint64_t end) {
          return rollup->AddSizes(keys, end - addr, false);
        });
  }

  void PrintMaps(const std::vector<const RangeMap*> maps) {
    uint64_t last = 0;
    uint64_t max = maps[0]->GetMaxAddress();
    int hex_digits = max > 0 ? std::ceil(std::log2(max) / 4) : 0;
    RangeMap::ComputeRollup(maps, [&](const std::vector<std::string>& keys,
                                      uint64_t addr, uint64_t end) {
      if (addr > last) {
        PrintMapRow("[-- Nothing mapped --]", last, addr, hex_digits);
      }
      PrintMapRow(KeysToString(keys), addr, end, hex_digits);
      last = end;
    });
    printf("\n");
  }

  void PrintFileMaps() { PrintMaps(FileMaps()); }
  void PrintVMMaps() { PrintMaps(VmMaps()); }

  std::string KeysToString(const std::vector<std::string>& keys) {
    std::string ret;

    // Start at offset 1 to skip the base map.
    for (size_t i = 1; i < keys.size(); i++) {
      if (i > 1) {
        ret += "\t";
      }
      ret += keys[i];
    }

    return ret;
  }

  void PrintMapRow(string_view str, uint64_t start, uint64_t end, int hex_digits) {
    printf("%.*" PRIx64 "-%.*" PRIx64 "\t %s\t\t%.*s\n", hex_digits, start,
           hex_digits, end, LeftPad(std::to_string(end - start), 10).c_str(),
           (int)str.size(), str.data());
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

void Bloaty::ScanAndRollupFile(const std::string &filename, Rollup* rollup,
                               std::vector<std::string>* out_build_ids) const {
  auto file = GetObjectFile(filename);

  DualMaps maps;
  std::vector<std::unique_ptr<RangeSink>> sinks;
  std::vector<RangeSink*> sink_ptrs;
  std::vector<RangeSink*> filename_sink_ptrs;

  // Base map always goes first.
  sinks.push_back(absl::make_unique<RangeSink>(
      &file->file_data(), options_, DataSource::kSegments, nullptr, nullptr));
  NameMunger empty_munger;
  sinks.back()->AddOutput(maps.base_map(), &empty_munger);
  sink_ptrs.push_back(sinks.back().get());

  for (auto source : sources_) {
    sinks.push_back(absl::make_unique<RangeSink>(&file->file_data(), options_,
                                                 source->effective_source,
                                                 maps.base_map(), arena_.get()));
    sinks.back()->AddOutput(maps.AppendMap(), source->munger.get());
    // We handle the kInputFiles data source internally, without handing it off
    // to the file format implementation.  This seems slightly simpler, since
    // the file format has to deal with armembers too.
    if (source->effective_source == DataSource::kInputFiles) {
      filename_sink_ptrs.push_back(sinks.back().get());
    } else {
      sink_ptrs.push_back(sinks.back().get());
    }
  }

  std::unique_ptr<ObjectFile> debug_file;
  std::string build_id = file->GetBuildId();
  if (!build_id.empty()) {
    auto iter = debug_files_.find(build_id);
    if (iter != debug_files_.end()) {
      debug_file = GetObjectFile(iter->second);
      file->set_debug_file(debug_file.get());
      out_build_ids->push_back(build_id);
    }
  }

  int64_t filesize_before = rollup->file_total() +
      rollup->filtered_file_total();
  file->ProcessFile(sink_ptrs);

  // kInputFile source: Copy the base map to the filename sink(s).
  for (auto sink : filename_sink_ptrs) {
    maps.base_map()->vm_map.ForEachRange(
        [sink](uint64_t start, uint64_t length) {
          sink->AddVMRange("inputfile_vmcopier", start, length,
                           sink->input_file().filename());
        });
    maps.base_map()->file_map.ForEachRange(
        [sink](uint64_t start, uint64_t length) {
          sink->AddFileRange("inputfile_filecopier",
                             sink->input_file().filename(), start, length);
        });
  }

  maps.ComputeRollup(rollup);

  // The ObjectFile implementation must guarantee this.
  int64_t filesize = rollup->file_total() +
      rollup->filtered_file_total() - filesize_before;
  (void)filesize;
  assert(filesize == file->file_data().data().size());

  if (verbose_level > 0 || options_.dump_raw_map()) {
    printf("Maps for %s:\n\n", filename.c_str());
    if (show != ShowDomain::kShowVM) {
      printf("FILE MAP:\n");
      maps.PrintFileMaps();
    }
    if (show != ShowDomain::kShowFile) {
      printf("VM MAP:\n");
      maps.PrintVMMaps();
    }
  }
}

void Bloaty::ScanAndRollupFiles(
    const std::vector<std::string>& filenames,
    std::vector<std::string>* build_ids,
    Rollup * rollup) const {
  int num_cpus = std::thread::hardware_concurrency();
  int num_threads = std::min(num_cpus, static_cast<int>(filenames.size()));

  struct PerThreadData {
    Rollup rollup;
    std::vector<std::string> build_ids;
  };

  std::vector<PerThreadData> thread_data(num_threads);
  std::vector<std::thread> threads(num_threads);
  ThreadSafeIterIndex index(filenames.size());

  std::unique_ptr<ReImpl> regex = nullptr;
  if (options_.has_source_filter()) {
    regex = absl::make_unique<ReImpl>(options_.source_filter());
  }

  for (int i = 0; i < num_threads; i++) {
    thread_data[i].rollup.SetFilterRegex(regex.get());

    threads[i] = std::thread([this, &index, &filenames](PerThreadData* data) {
      try {
        int j;
        while (index.TryGetNext(&j)) {
          ScanAndRollupFile(filenames[j], &data->rollup, &data->build_ids);
        }
      } catch (const bloaty::Error& e) {
        index.Abort(e.what());
      }
    }, &thread_data[i]);
  }

  for (int i = 0; i < num_threads; i++) {
    threads[i].join();
    PerThreadData* data = &thread_data[i];
    if (i == 0) {
      *rollup = std::move(data->rollup);
    } else {
      rollup->Add(data->rollup);
    }

    build_ids->insert(build_ids->end(),
                      data->build_ids.begin(),
                      data->build_ids.end());
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
  std::vector<std::string> build_ids;
  std::vector<std::string> input_filenames;
  for (const auto& file_info : input_files_) {
    input_filenames.push_back(file_info.filename_);
  }
  ScanAndRollupFiles(input_filenames, &build_ids, &rollup);

  if (!base_files_.empty()) {
    Rollup base;
    std::vector<std::string> base_filenames;
    for (const auto& file_info : base_files_) {
      base_filenames.push_back(file_info.filename_);
    }
    ScanAndRollupFiles(base_filenames, &build_ids, &base);
    rollup.Subtract(base);
    rollup.CreateDiffModeRollupOutput(&base, options, output);
  } else {
    rollup.CreateRollupOutput(options, output);
  }

  for (const auto& build_id : build_ids) {
    debug_files_.erase(build_id);
  }

  // Error out if some --debug-files were not used.
  if (!debug_files_.empty()) {
    std::string input_files;
    std::string unused_debug;
    for (const auto& pair : debug_files_) {
      unused_debug += absl::Substitute(
          "$0   $1\n",
          absl::BytesToHexString(pair.first).c_str(),
          pair.second.c_str());
    }

    for (const auto& file_info : input_files_) {
      input_files += absl::Substitute(
          "$0   $1\n", absl::BytesToHexString(file_info.build_id_).c_str(),
          file_info.filename_.c_str());
    }
    for (const auto& file_info : base_files_) {
      input_files += absl::Substitute(
          "$0   $1\n", absl::BytesToHexString(file_info.build_id_).c_str(),
          file_info.filename_.c_str());
    }
    THROWF(
        "Debug file(s) did not match any input file:\n$0\nInput Files:\n$1",
        unused_debug.c_str(), input_files.c_str());
  }
}

void Bloaty::DisassembleFunction(string_view function, const Options& options,
                                 RollupOutput* output) {
  DisassemblyInfo info;
  for (const auto& file_info : input_files_) {
    auto file = GetObjectFile(file_info.filename_);
    if (file->GetDisassemblyInfo(function, EffectiveSymbolSource(options),
                                 &info)) {
      output->SetDisassembly(::bloaty::DisassembleFunction(info));
      return;
    }
  }

  THROWF("Couldn't find function $0 to disassemble", function);
}

const char usage[] = R"(Bloaty McBloatface: a size profiler for binaries.

USAGE: bloaty [OPTION]... FILE... [-- BASE_FILE...]

Options:

  --csv              Output in CSV format instead of human-readable.
  --tsv              Output in TSV format instead of human-readable.
  -c FILE            Load configuration from <file>.
  -d SOURCE,SOURCE   Comma-separated list of sources to scan.
  --debug-file=FILE  Use this file for debug symbols and/or symbol table.
  -C MODE            How to demangle symbols.  Possible values are:
  --demangle=MODE      --demangle=none   no demangling, print raw symbols
                       --demangle=short  demangle, but omit arg/return types
                       --demangle=full   print full demangled type
                     The default is --demangle=short.
  --disassemble=FUNCTION
                     Disassemble this function (EXPERIMENTAL)
  --domain=DOMAIN    Which domains to show.  Possible values are:
                       --domain=vm
                       --domain=file
                       --domain=both (the default)
  -n NUM             How many rows to show per level before collapsing
                     other keys into '[Other]'.  Set to '0' for unlimited.
                     Defaults to 20.
  -s SORTBY          Whether to sort by VM or File size.  Possible values
                     are:
                       -s vm
                       -s file
                       -s both (the default: sorts by max(vm, file)).
  -w                 Wide output; don't truncate long labels.
  --help             Display this message and exit.
  --list-sources     Show a list of available sources and exit.
  --source-filter=PATTERN
                     Only show keys with names matching this pattern.

Options for debugging Bloaty:

  --debug-vmaddr=ADDR
  --debug-fileoff=OFF
                     Print extended debugging information for the given
                     VM address and/or file offset.
  -v                 Verbose output.  Dumps warnings encountered during
                     processing and full VM/file maps at the end.
                     Add more v's (-vv, -vvv) for even more.
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
    if (!TryParseOption(flag, &val_str)) {
      return false;
    }

    if (!absl::SimpleAtoi(val_str, val)) {
      THROWF("option '$0' had non-integral argument: $1", flag, val_str);
    }

    return true;
  }

  bool TryParseUint64Option(string_view flag, uint64_t* val) {
    string_view val_str;
    if (!TryParseOption(flag, &val_str)) {
      return false;
    }

    try {
      *val = std::stoull(std::string(val_str), nullptr, 0);
    } catch (...) {
      THROWF("option '$0' had non-integral argument: $1", flag, val_str);
    }

    return true;
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
  uint64_t uint64_option;
  bool has_domain = false;

  while (!args.IsDone()) {
    if (args.TryParseFlag("--")) {
      if (saw_separator) {
        THROW("'--' option should only be specified once");
      }
      saw_separator = true;
    } else if (args.TryParseFlag("--csv")) {
      output_options->output_format = OutputFormat::kCSV;
    } else if (args.TryParseFlag("--tsv")) {
      output_options->output_format = OutputFormat::kTSV;
    } else if (args.TryParseFlag("--raw-map")) {
      options->set_dump_raw_map(true);
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
    } else if (args.TryParseOption("--debug-file", &option)) {
      options->add_debug_filename(std::string(option));
    } else if (args.TryParseUint64Option("--debug-fileoff", &uint64_option)) {
      if (options->has_debug_fileoff()) {
        THROW("currently we only support a single debug fileoff");
      }
      options->set_debug_fileoff(uint64_option);
    } else if (args.TryParseUint64Option("--debug-vmaddr", &uint64_option)) {
      if (options->has_debug_vmaddr()) {
        THROW("currently we only support a single debug vmaddr");
      }
      options->set_debug_vmaddr(uint64_option);
    } else if (args.TryParseOption("--disassemble", &option)) {
      options->mutable_disassemble_function()->assign(std::string(option));
    } else if (args.TryParseIntegerOption("-n", &int_option)) {
      if (int_option == 0) {
        options->set_max_rows_per_level(INT64_MAX);
      } else {
        options->set_max_rows_per_level(int_option);
      }
    } else if (args.TryParseOption("--domain", &option)) {
      has_domain = true;
      if (option == "vm") {
        show = output_options->show = ShowDomain::kShowVM;
      } else if (option == "file") {
        show = output_options->show = ShowDomain::kShowFile;
      } else if (option == "both") {
        show = output_options->show = ShowDomain::kShowBoth;
      } else {
        THROWF("unknown value for --domain: $0", option);
      }
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
    } else if (args.TryParseOption("--source-filter", &option)) {
      options->set_source_filter(std::string(option));
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
      puts(usage);
      return false;
    } else if (args.TryParseFlag("--version")) {
      printf("Bloaty McBloatface 1.1\n");
      exit(0);
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

  if (has_domain && !options->has_sort_by()) {
    // Default to sorting by what we are showing.
    switch (output_options->show) {
      case ShowDomain::kShowFile:
        options->set_sort_by(Options::SORTBY_FILESIZE);
        break;
      case ShowDomain::kShowVM:
        options->set_sort_by(Options::SORTBY_VMSIZE);
        break;
      case ShowDomain::kShowBoth:
        options->set_sort_by(Options::SORTBY_BOTH);
        break;
    }
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

  for (auto& debug_filename : options.debug_filename()) {
    bloaty.AddDebugFilename(debug_filename);
  }

  for (const auto& custom_data_source : options.custom_data_source()) {
    bloaty.DefineCustomDataSource(custom_data_source);
  }

  for (const auto& data_source : options.data_source()) {
    bloaty.AddDataSource(data_source);
  }

  if (options.has_source_filter()) {
    ReImpl re(options.source_filter());
    if (!re.ok()) {
      THROW("invalid regex for source_filter");
    }
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
