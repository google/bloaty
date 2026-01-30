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


#include <algorithm>
#include <string>
#include <iostream>
#include "absl/numeric/int128.h"
#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "arfile.h"
#include "bloaty.h"
#include "util.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

using absl::string_view;

namespace bloaty {

size_t StringViewToSize(string_view str) {
  // Trim trailing whitespace (AR format allows space-padding in numeric fields)
  while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) {
    str.remove_suffix(1);
  }
  size_t ret;
  if (!absl::SimpleAtoi(str, &ret)) {
    THROWF("couldn't convert string '$0' to integer.", str);
  }
  return ret;
}

bool ArFile::MemberReader::ReadMember(MemberFile* file) {
  struct Header {
    char file_id[16];
    char modified_timestamp[12];
    char owner_id[6];
    char group_id[6];
    char mode[8];
    char size[10];
    char end[2];
  };

  if (remaining_.size() == 0) {
    return false;
  } else if (remaining_.size() < sizeof(Header)) {
    THROW("Premature EOF in AR data");
  }

  const Header* header = reinterpret_cast<const Header*>(remaining_.data());
  file->header = Consume(sizeof(Header));

  string_view file_id(&header->file_id[0], sizeof(header->file_id));
  string_view size_str(&header->size[0], sizeof(header->size));
  file->size = StringViewToSize(size_str);
  file->contents = Consume(file->size);
  file->file_type = MemberFile::kNormal;
  file->format = MemberFile::GNU;

  if (file_id[0] == '/') {
    // Special filename, internal to the format.
    if (file_id[1] == ' ') {
      file->file_type = MemberFile::kSymbolTable;
    } else if (file_id[1] == '/') {
      file->file_type = MemberFile::kLongFilenameTable;
      long_filenames_ = file->contents;
    } else if (isdigit(file_id[1])) {
      size_t offset = StringViewToSize(file_id.substr(1));
      size_t end = long_filenames_.find('/', offset);

      if (end == std::string::npos) {
        THROW("Unterminated long filename");
      }

      file->filename = long_filenames_.substr(offset, end - offset);
    } else {
      THROW("Unexpected special filename in AR archive");
    }
  } else if (file_id[0] == '#' && file_id[1] == '1' &&
             file_id[2] == '/') {
      // Darwin-style long filename: #1/N where N is the embedded filename length
      file->format = MemberFile::Darwin;
      size_t offset = StringViewToSize(file_id.substr(3));

      // Validate that the filename length doesn't exceed member content size
      if (offset > file->contents.size()) {
        THROWF("Darwin long filename offset ($0) exceeds member size ($1)",
               offset, file->contents.size());
      }

      string_view filename_data = file->contents.substr(0, offset);
      size_t null_pos = filename_data.find('\0');
      if (null_pos != string_view::npos) {
        file->filename = filename_data.substr(0, null_pos);
      } else {
        file->filename = filename_data;
      }

      // Darwin archives use "__.SYMDEF" or "__.SYMDEF SORTED" for symbol tables
      // (GNU uses "/" for the same purpose)
      if (file->filename == "__.SYMDEF" || file->filename == "__.SYMDEF SORTED") {
        file->file_type = MemberFile::kSymbolTable;
      } else {
        file->contents = file->contents.substr(offset);
      }
  } else {
    // Normal filename, slash-terminated.
    size_t slash = file_id.find('/');

    if (slash == std::string::npos) {
      file->format = MemberFile::BSD;
      THROW("BSD-style AR not yet implemented");
    }

    file->filename = file_id.substr(0, slash);
  }

  return true;
}

}  // bloaty namespace

