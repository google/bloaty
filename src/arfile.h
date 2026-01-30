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

#ifndef BLOATY_ARFILE_H_
#define BLOATY_ARFILE_H_

#include <algorithm>
#include <string>
#include <iostream>
#include "absl/numeric/int128.h"
#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "bloaty.h"
#include "util.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

using absl::string_view;

namespace bloaty {

// ArFile //////////////////////////////////////////////////////////////////////

// For parsing .a files (static libraries).
//
// AR archives are used for static libraries and can contain multiple object
// files. The format is ancient but still widely used. There are three main
// variants:
//
// 1. GNU format:
//    - Symbol table: member named "/"
//    - Long filename table: member named "//"
//    - Long filename references: members named "/N" where N is offset into table
//    - Short filenames: slash-terminated in header (e.g., "foo.o/")
//
// 2. Darwin format:
//    - Symbol table: member named "__.SYMDEF" or "__.SYMDEF SORTED"
//    - Long filenames: embedded in member data, indicated by "#1/N" where N is length
//    - Short filenames: same as GNU (slash-terminated)
//
// 3. BSD format:
//    - Uses space-padded filenames instead of slash-terminated
//    - Currently not implemented (throws error if detected)
//
// Archive structure:
//   Magic: "!<arch>\n" (8 bytes)
//   For each member:
//     Header: 60 bytes (file_id, timestamp, owner, group, mode, size, end marker)
//     Data: size bytes (padded to even boundary for alignment)
//
// The best documentation for this file format is Wikipedia:
// https://en.wikipedia.org/wiki/Ar_(Unix)

class ArFile {
 public:
  ArFile(string_view data)
      : magic_(StrictSubstr(data, 0, kMagicSize)),
        contents_(data.substr(std::min<size_t>(data.size(), kMagicSize))) {}

  bool IsOpen() const { return magic() == string_view(kMagic); }

  string_view magic() const { return magic_; }
  string_view contents() const { return contents_; }

  struct MemberFile {
    enum {
      kSymbolTable,        // Stores a symbol table.
      kLongFilenameTable,  // Stores long filenames, users should ignore.
      kNormal,             // Regular data file.
    } file_type;

    enum {
     GNU,
     Darwin,
     BSD
    } format;

    string_view filename;  // Only when file_type == kNormal
    size_t size;
    string_view header;
    string_view contents;
  };

  class MemberReader {
   public:
    MemberReader(const ArFile& ar) : remaining_(ar.contents()) {}
    bool ReadMember(MemberFile* file);
    bool IsEof() const { return remaining_.size() == 0; }

   private:
    string_view Consume(size_t n) {
      n = (n % 2 == 0 ? n : n + 1);
      if (remaining_.size() < n) {
        THROW("premature end of file");
      }
      string_view ret = remaining_.substr(0, n);
      remaining_.remove_prefix(n);
      return ret;
    }

    string_view long_filenames_;
    string_view remaining_;
  };

 private:
  const string_view magic_;
  const string_view contents_;

  static constexpr const char* kMagic = "!<arch>\n";
  static constexpr int kMagicSize = 8;
};

inline bool IsArchiveFile(string_view data) {
  ArFile ar(data);
  return ar.IsOpen();
}

}  // namespace bloaty

#endif  // BLOATY_ARFILE_H_

