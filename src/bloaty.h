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

// This file contains APIs for use within Bloaty.  None of these APIs have any
// guarantees whatsoever about their stability!  The public API for bloaty is
// its command-line interface.

#ifndef BLOATY_H_
#define BLOATY_H_

#include <stdlib.h>
#define __STDC_LIMIT_MACROS
#define __STDC_FORMAT_MACROS
#include <stdint.h>
#include <inttypes.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "capstone/capstone.h"

#include "dwarf/debug_info.h"
#include "bloaty.pb.h"
#include "range_map.h"
#include "re.h"

namespace bloaty {

extern int verbose_level;

class NameMunger;
class Options;
struct DualMap;
struct DisassemblyInfo;

enum class DataSource {
  kArchiveMembers,
  kCompileUnits,
  kInlines,
  kInputFiles,
  kRawRanges,
  kSections,
  kSegments,

  // We always set this to one of the concrete symbol types below before
  // setting it on a sink.
  kSymbols,

  kRawSymbols,
  kFullSymbols,
  kShortSymbols
};

class InputFile {
 public:
  InputFile(const std::string& filename) : filename_(filename) {}
  InputFile(const InputFile&) = delete;
  InputFile& operator=(const InputFile&) = delete;
  virtual ~InputFile() {}

  const std::string& filename() const { return filename_; }
  absl::string_view data() const { return data_; }

 private:
  const std::string filename_;

 protected:
  absl::string_view data_;
};

class InputFileFactory {
 public:
  virtual ~InputFileFactory() {}

  // Throws if the file could not be opened.
  virtual std::unique_ptr<InputFile> OpenFile(
      const std::string& filename) const = 0;
};

class MmapInputFileFactory : public InputFileFactory {
 public:
  std::unique_ptr<InputFile> OpenFile(
      const std::string& filename) const override;
};

// NOTE: all sizes are uint64, even on 32-bit platforms:
//   - 32-bit platforms can have files >4GB in some cases.
//   - for object files (not executables/shared libs) we pack both a section
//     index and an address into the "vmaddr" value, and we need enough bits to
//     safely do this.

// A RangeSink allows data sources to assign labels to ranges of VM address
// space and/or file offsets.
class RangeSink {
public:
  RangeSink(const InputFile *file, const Options &options,
            DataSource data_source, const DualMap *translator,
            google::protobuf::Arena *arena);
  RangeSink(const RangeSink &) = delete;
  RangeSink &operator=(const RangeSink &) = delete;
  ~RangeSink();

  const Options &options() const { return options_; }

  void AddOutput(DualMap *map, const NameMunger *munger);

  DataSource data_source() const { return data_source_; }
  const InputFile &input_file() const { return *file_; }
  bool IsBaseMap() const { return translator_ == nullptr; }

  // If vmsize or filesize is zero, this mapping is presumed not to exist in
  // that domain.  For example, .bss mappings don't exist in the file, and
  // .debug_* mappings don't exist in memory.
  void AddRange(const char *analyzer, absl::string_view name, uint64_t vmaddr,
                uint64_t vmsize, uint64_t fileoff, uint64_t filesize);

  void AddRange(const char *analyzer, absl::string_view name, uint64_t vmaddr,
                uint64_t vmsize, absl::string_view file_range) {
    AddRange(analyzer, name, vmaddr, vmsize,
             file_range.data() - file_->data().data(), file_range.size());
  }

  void AddFileRange(const char* analyzer, absl::string_view name,
                    uint64_t fileoff, uint64_t filesize);

  // Like AddFileRange(), but the label is whatever label was previously
  // assigned to VM address |label_from_vmaddr|.  If no existing label is
  // assigned to |label_from_vmaddr|, this function does nothing.
  void AddFileRangeForVMAddr(const char* analyzer, uint64_t label_from_vmaddr,
                             absl::string_view file_range);
  void AddVMRangeForVMAddr(const char* analyzer, uint64_t label_from_vmaddr,
                           uint64_t addr, uint64_t size);

  // Applies this label from |from_file_range| to |file_range|, but only if the
  // entire |from_file_range| has a single label.  If not, this does nothing.
  void AddFileRangeForFileRange(const char* analyzer,
                                absl::string_view from_file_range,
                                absl::string_view file_range);

  void AddFileRange(const char* analyzer, absl::string_view name,
                    absl::string_view file_range) {
    // When separate debug files are being used, the DWARF analyzer will try to
    // add sections of the debug file.  We want to prevent this because we only
    // want to profile the main file (not the debug file), so we filter these
    // out.  This approach is simple to implement, but does result in some
    // useless work being done.  We may want to avoid doing this useless work in
    // the first place.
    if (FileContainsPointer(file_range.data())) {
      AddFileRange(analyzer, name, file_range.data() - file_->data().data(),
                   file_range.size());
    }
  }

  // The VM-only functions below may not be used to populate the base map!

  // Adds a region to the memory map.  It should not overlap any previous
  // region added with Add(), but it should overlap the base memory map.
  void AddVMRange(const char* analyzer, uint64_t vmaddr, uint64_t vmsize,
                  const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this name becomes an alias of the
  // previous name.
  //
  // This is for things like symbol tables that sometimes map multiple names to
  // the same physical function.
  void AddVMRangeAllowAlias(const char* analyzer, uint64_t vmaddr,
                            uint64_t size, const std::string& name);

  // Like Add(), but allows that this addr/size might have previously been added
  // already under a different name.  If so, this add is simply ignored.
  //
  // This is for cases like sourcefiles.  Sometimes a single function appears to
  // come from multiple source files.  But if it does, we don't want to alias
  // the entire source file to another, because it's probably only part of the
  // source file that overlaps.
  void AddVMRangeIgnoreDuplicate(const char* analyzer, uint64_t vmaddr,
                                 uint64_t size, const std::string& name);

  const DualMap& MapAtIndex(size_t index) const {
    return *outputs_[index].first;
  }

  // Translates the given pointer (which must be within the range of
  // input_file().data()) to a VM address.
  uint64_t TranslateFileToVM(const char* ptr);
  absl::string_view TranslateVMToFile(uint64_t address);

  // Decompresses zlib-formatted data and returns the decompressed data.
  // Since the decompressed data is not actually part of the file, any
  // Add*Range() calls to this region will be no-ops.
  absl::string_view ZlibDecompress(absl::string_view contents,
                                   uint64_t uncompressed_size);

  static constexpr uint64_t kUnknownSize = RangeMap::kUnknownSize;

 private:
  bool FileContainsPointer(const void* ptr) const {
    absl::string_view file_data = file_->data();
    return ptr >= file_data.data() && ptr < file_data.data() + file_data.size();
  }

  bool ContainsVerboseVMAddr(uint64_t vmaddr, uint64_t vmsize);
  bool ContainsVerboseFileOffset(uint64_t fileoff, uint64_t filesize);
  bool IsVerboseForVMRange(uint64_t vmaddr, uint64_t vmsize);
  bool IsVerboseForFileRange(uint64_t fileoff, uint64_t filesize);

  const InputFile* file_;
  const Options options_;
  DataSource data_source_;
  const DualMap* translator_;
  std::vector<std::pair<DualMap*, const NameMunger*>> outputs_;
  google::protobuf::Arena *arena_;
};

// NameMunger //////////////////////////////////////////////////////////////////

// Use to transform input names according to the user's configuration.
// For example, the user can use regexes.
class NameMunger {
 public:
  NameMunger() {}
  NameMunger(const NameMunger&) = delete;
  NameMunger& operator=(const NameMunger&) = delete;

  // Adds a regex that will be applied to all names.  All regexes will be
  // applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);
  std::string Munge(absl::string_view name) const;

  bool IsEmpty() const { return regexes_.empty(); }

 private:
  std::vector<std::pair<std::unique_ptr<ReImpl>, std::string>> regexes_;
};

typedef std::map<absl::string_view, std::pair<uint64_t, uint64_t>> SymbolTable;

// Represents an object/executable file in a format like ELF, Mach-O, PE, etc.
// To support a new file type, implement this interface.
class ObjectFile {
 public:
  ObjectFile(std::unique_ptr<InputFile> file_data)
      : file_data_(std::move(file_data)), debug_file_(this) {}
  virtual ~ObjectFile() {}

  virtual std::string GetBuildId() const = 0;

  // Process this file, pushing data to |sinks| as appropriate for each data
  // source.  If any debug files match the build id for this file, it will be
  // given here, otherwise it is |this|.
  virtual void ProcessFile(const std::vector<RangeSink*>& sinks) const = 0;

  virtual bool GetDisassemblyInfo(absl::string_view symbol,
                                  DataSource symbol_source,
                                  DisassemblyInfo* info) const = 0;

  const InputFile& file_data() const { return *file_data_; }

  // Sets the debug file for |this|.  |file| must outlive this instance.
  void set_debug_file(const ObjectFile* file) {
    assert(debug_file_->GetBuildId() == GetBuildId());
    debug_file_ = file;
  }

  const ObjectFile& debug_file() const { return *debug_file_; }

 private:
  std::unique_ptr<InputFile> file_data_;
  const ObjectFile* debug_file_;
};

std::unique_ptr<ObjectFile> TryOpenELFFile(std::unique_ptr<InputFile>& file);
std::unique_ptr<ObjectFile> TryOpenMachOFile(std::unique_ptr<InputFile>& file);
std::unique_ptr<ObjectFile> TryOpenWebAssemblyFile(std::unique_ptr<InputFile>& file);
std::unique_ptr<ObjectFile> TryOpenPEFile(std::unique_ptr<InputFile>& file);

// Provided by dwarf.cc.  To use these, a module should fill in a dwarf::File
// and then call these functions.
void ReadDWARFCompileUnits(const dwarf::File& file, const DualMap& map,
                           RangeSink* sink);
void ReadDWARFInlines(const dwarf::File& file, RangeSink* sink,
                      bool include_line);
void ReadEhFrame(absl::string_view contents, RangeSink* sink);
void ReadEhFrameHdr(absl::string_view contents, RangeSink* sink);

// Demangle C++ symbols according to the Itanium ABI.  The |source| argument
// controls what demangling mode we are using.
std::string ItaniumDemangle(absl::string_view symbol, DataSource source);


// DualMap /////////////////////////////////////////////////////////////////////

// Contains a RangeMap for VM space and file space for a given file.

struct DualMap {
  RangeMap vm_map;
  RangeMap file_map;
};

struct DisassemblyInfo {
  absl::string_view text;
  DualMap symbol_map;
  cs_arch arch;
  cs_mode mode;
  uint64_t start_address;
};

std::string DisassembleFunction(const DisassemblyInfo& info);
void DisassembleFindReferences(const DisassemblyInfo& info, RangeSink* sink);

// Top-level API ///////////////////////////////////////////////////////////////

// This should only be used by main.cc and unit tests.

class Rollup;

struct RollupRow {
  RollupRow(const std::string& name_) : name(name_) {}

  std::string name;
  int64_t vmsize = 0;
  int64_t filesize = 0;
  int64_t filtered_vmsize = 0;
  int64_t filtered_filesize = 0;
  int64_t other_count = 0;
  int64_t sortkey;
  double vmpercent;
  double filepercent;
  std::vector<RollupRow> sorted_children;

  static bool Compare(const RollupRow& a, const RollupRow& b) {
    // Sort value high-to-low.
    if (a.sortkey != b.sortkey) {
      return a.sortkey > b.sortkey;
    }
    // Sort name low to high.
    return a.name < b.name;
  }
};

enum class OutputFormat {
  kPrettyPrint,
  kCSV,
  kTSV,
};

enum class ShowDomain {
  kShowFile,
  kShowVM,
  kShowBoth,
};

struct OutputOptions {
  OutputFormat output_format = OutputFormat::kPrettyPrint;
  size_t max_label_len = 80;
  ShowDomain show = ShowDomain::kShowBoth;
};

struct RollupOutput {
 public:
  RollupOutput() : toplevel_row_("TOTAL") {}
  RollupOutput(const RollupOutput&) = delete;
  RollupOutput& operator=(const RollupOutput&) = delete;

  void AddDataSourceName(absl::string_view name) {
    source_names_.emplace_back(std::string(name));
  }

  const std::vector<std::string>& source_names() const { return source_names_; }
  void Print(const OutputOptions& options, std::ostream* out);
  void SetDisassembly(absl::string_view disassembly) {
    disassembly_ = std::string(disassembly);
  }

  absl::string_view GetDisassembly() { return disassembly_; }

  // For debugging.
  const RollupRow& toplevel_row() const { return toplevel_row_; }
  bool diff_mode() const { return diff_mode_; }

 private:
  friend class Rollup;

  std::vector<std::string> source_names_;
  RollupRow toplevel_row_;
  std::string disassembly_;

  // When we are in diff mode, rollup sizes are relative to the baseline.
  bool diff_mode_ = false;

  static bool IsSame(const std::string& a, const std::string& b);
  void PrettyPrint(const OutputOptions& options, std::ostream* out) const;
  void PrintToCSV(std::ostream* out, bool tabs) const;
  void PrettyPrintRow(const RollupRow& row, size_t indent,
                      const OutputOptions& options, std::ostream* out) const;
  void PrettyPrintTree(const RollupRow& row, size_t indent,
                       const OutputOptions& options, std::ostream* out) const;
  void PrintRowToCSV(const RollupRow& row,
                     std::vector<std::string> parent_labels,
                     std::ostream* out, bool tabs) const;
  void PrintTreeToCSV(const RollupRow& row,
                      std::vector<std::string> parent_labels,
                      std::ostream* out, bool tabs) const;
};

bool ParseOptions(bool skip_unknown, int* argc, char** argv[], Options* options,
                  OutputOptions* output_options, std::string* error);
bool BloatyMain(const Options& options, const InputFileFactory& file_factory,
                RollupOutput* output, std::string* error);

}  // namespace bloaty

#endif
