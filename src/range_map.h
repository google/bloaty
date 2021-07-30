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

// RagneMap maps
//
//   [uint64_t, uint64_t) -> std::string, [optional other range base]
//
// where ranges must be non-overlapping.
//
// This is used to map the address space (either pointer offsets or file
// offsets).
//
// The other range base allows us to use this RangeMap to translate addresses
// from this domain to another one (like vm_addr -> file_addr or vice versa).
//
// This type is only exposed in the .h file for unit testing purposes.

#ifndef BLOATY_RANGE_MAP_H_
#define BLOATY_RANGE_MAP_H_

#include <assert.h>
#include <stdint.h>

#include <exception>
#include <map>
#include <stdexcept>
#include <vector>

#include "absl/strings/str_cat.h"

namespace bloaty {

class RangeMapTest;

class RangeMap {
 public:
  RangeMap() = default;
  RangeMap(RangeMap&& other) = default;
  RangeMap& operator=(RangeMap&& other) = default;
  RangeMap(RangeMap& other) = delete;
  RangeMap& operator=(RangeMap& other) = delete;

  // Adds a range to this map.
  void AddRange(uint64_t addr, uint64_t size, const std::string& val);

  // Adds a range to this map (in domain D1) that also corresponds to a
  // different range in a different map (in domain D2).  The correspondance will
  // be noted to allow us to translate into the other domain later.
  void AddDualRange(uint64_t addr, uint64_t size, uint64_t otheraddr,
                    const std::string& val);

  // Adds a range to this map (in domain D1), and also adds corresponding ranges
  // to |other| (in domain D2), using |translator| (in domain D1) to translate
  // D1->D2.  The translation is performed using information from previous
  // AddDualRange() calls on |translator|.
  //
  // Returns true if the entire range [addr, size] was present in the
  // |translator| map.  (This does not necessarily mean that every part of the
  // range was actually translated).  If the return value is false, then the
  // contents of |this| and |other| are undefined (Bloaty will bail in this
  // case).
  bool AddRangeWithTranslation(uint64_t addr, uint64_t size,
                               const std::string& val,
                               const RangeMap& translator, bool verbose,
                               RangeMap* other);

  // Collapses adjacent ranges with the same label. This reduces memory usage
  // and removes redundant noise from the output when dumping a full memory map
  // (in normal Bloaty output it makes no difference, because all labels with
  // the same name are added together).
  //
  // TODO(haberman): see if we can do this at insertion time instead, so it
  // doesn't require a second pass.
  void Compress();

  // Returns whether this RangeMap fully covers the given range.
  bool CoversRange(uint64_t addr, uint64_t size) const;

  // Returns the maximum address contained in this map.
  uint64_t GetMaxAddress() const;

  // Translates |addr| into the other domain, returning |true| if this was
  // successful.
  bool Translate(uint64_t addr, uint64_t *translated) const;

  // Looks for a range within this map that contains |addr|.  If found, returns
  // true and sets |label| to the corresponding label, and |offset| to the
  // offset from the beginning of this range.
  bool TryGetLabel(uint64_t addr, std::string* label) const;
  bool TryGetLabelForRange(uint64_t addr, uint64_t size,
                           std::string* label) const;

  // Looks for a range that starts exactly on |addr|.  If it exists, returns
  // true and sets |size| to its size.
  bool TryGetSize(uint64_t addr, uint64_t* size) const;

  std::string DebugString() const;

  static std::string EntryDebugString(uint64_t addr, uint64_t size,
                                      uint64_t other_start,
                                      const std::string& label) {
    std::string end =
        size == kUnknownSize ? "?" : absl::StrCat(absl::Hex(addr + size));
    std::string ret = absl::StrCat("[", absl::Hex(addr), ", ", end,
                                   "] (size=", absl::Hex(size), "): ", label);
    if (other_start != UINT64_MAX) {
      absl::StrAppend(&ret, ", other_start=", absl::Hex(other_start));
    }
    return ret;
  }

  template <class T>
  std::string EntryDebugString(T it) const {
    if (it == mappings_.end()) {
      return "[end]";
    } else {
      return EntryDebugString(it->first, it->second.size,
                              it->second.other_start, it->second.label);
    }
  }

  template <class Func>
  static void ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                            Func func);

  template <class Func>
  void ForEachRange(Func func) const {
    for (auto iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
      func(iter->first, RangeEnd(iter) - iter->first);
    }
  }

  template <class Func>
  void ForEachRangeWithStart(uint64_t start, Func func) const {
    for (auto iter = FindContaining(start); iter != mappings_.end(); ++iter) {
      if (!func(iter->second.label, iter->first,
                RangeEnd(iter) - iter->first)) {
        return;
      }
    }
  }

  static constexpr uint64_t kUnknownSize = UINT64_MAX;

 private:
  friend class RangeMapTest;
  static const uint64_t kNoTranslation = UINT64_MAX;

  struct Entry {
    Entry(const std::string& label_, uint64_t size_, uint64_t other_)
        : label(label_), size(size_), other_start(other_) {}
    std::string label;
    uint64_t size;
    uint64_t other_start;  // kNoTranslation if there is no mapping.

    bool HasTranslation() const { return other_start != kNoTranslation; }
    bool HasFallbackLabel() const { return !label.empty() && label[0] == '['; }

    // We assume that short regions that were unattributed (have fallback
    // labels) are actually padding. We could probably make this heuristic
    // a bit more robust.
    bool IsShortFallback() const { return size <= 16 && HasFallbackLabel(); }
  };

  typedef std::map<uint64_t, Entry> Map;
  Map mappings_;

  template <class T>
  void CheckConsistency(T iter) const {
    assert(iter->first + iter->second.size > iter->first);
    assert(iter == mappings_.begin() ||
           RangeEnd(std::prev(iter)) <= iter->first);
    assert(std::next(iter) == mappings_.end() ||
           RangeEnd(iter) <= std::next(iter)->first);
  }

  template <class T>
  bool EntryContains(T iter, uint64_t addr) const {
    return addr >= iter->first && addr < RangeEnd(iter);
  }

  template <class T>
  bool EntryContainsStrict(T iter, uint64_t addr) const {
    if (iter->second.size == kUnknownSize) {
      return iter->first == addr;
    } else {
      return addr >= iter->first && addr < RangeEnd(iter);
    }
  }

  template <class T>
  void MaybeSetLabel(T iter, const std::string& label, uint64_t addr,
                     uint64_t end);

  // When the size is unknown return |unknown| for the end.
  uint64_t RangeEndUnknownLimit(Map::const_iterator iter,
                                uint64_t unknown) const {
    if (iter->second.size == kUnknownSize) {
      Map::const_iterator next = std::next(iter);
      if (IterIsEnd(next) || next->first > unknown) {
        return unknown;
      } else {
        return next->first;
      }
    } else {
      uint64_t ret = iter->first + iter->second.size;
      assert(ret > iter->first);
      return ret;
    }
  }

  uint64_t RangeEnd(Map::const_iterator iter) const {
    return RangeEndUnknownLimit(iter, UINT64_MAX);
  }

  bool IterIsEnd(Map::const_iterator iter) const {
    return iter == mappings_.end();
  }

  template <class T>
  uint64_t TranslateWithEntry(T iter, uint64_t addr) const;

  template <class T>
  bool TranslateAndTrimRangeWithEntry(T iter, uint64_t addr, uint64_t size,
                                      uint64_t* trimmed_addr,
                                      uint64_t* translated_addr,
                                      uint64_t* trimmed_size) const;

  // Finds the entry that contains |addr|.  If no such mapping exists, returns
  // mappings_.end().
  Map::const_iterator FindContaining(uint64_t addr) const;

  // Finds the entry that contains |addr|, or the very next entry (which may be
  // mappings_.end()).
  Map::iterator FindContainingOrAfter(uint64_t addr);
  Map::const_iterator FindContainingOrAfter(uint64_t addr) const;
};

template <class Func>
void RangeMap::ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                             Func func) {
  assert(range_maps.size() > 0);
  std::vector<Map::const_iterator> iters;

  if (range_maps[0]->mappings_.empty()) {
    for (int i = 0; i < range_maps.size(); i++) {
      const RangeMap* range_map = range_maps[i];
      if (!range_map->mappings_.empty()) {
        printf(
            "Error, range (%s) exists at index %d, but base map is empty\n",
            range_map->EntryDebugString(range_map->mappings_.begin()).c_str(),
            i);
        assert(false);
        throw std::runtime_error("Range extends beyond base map.");
      }
    }
    return;
  }

  iters.reserve(range_maps.size());
  for (auto range_map : range_maps) {
    iters.push_back(range_map->mappings_.begin());
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
  //   -----  -----  -----             ---------------
  //
  //
  //   -----  -----  -----             ---------------
  //     C      Z      3                    C,Z,3
  //   -----  -----  -----             ---------------
  //
  // All input maps must cover exactly the same domain.

  // Outer loop: once per continuous (gapless) region.
  while (true) {
    std::vector<std::string> keys;
    uint64_t current = 0;

    if (range_maps[0]->IterIsEnd(iters[0])) {
      // Termination condition: all iterators must be at end.
      for (int i = 0; i < range_maps.size(); i++) {
        if (!range_maps[i]->IterIsEnd(iters[i])) {
          printf(
              "Error, range (%s) extends beyond final base map range "
              "(%s)\n",
              range_maps[i]->EntryDebugString(iters[i]).c_str(),
              range_maps[0]->EntryDebugString(std::prev(iters[0])).c_str());
          assert(false);
          throw std::runtime_error("Range extends beyond base map.");
        }
      }
      return;
    } else {
      // Starting a new continuous range: all iterators must start at the same
      // place.
      current = iters[0]->first;
      for (int i = 0; i < range_maps.size(); i++)  {
        if (range_maps[i]->IterIsEnd(iters[i])) {
          printf(
              "Error, no more ranges for index %d but we need one "
              "to match (%s)\n",
              i, range_maps[0]->EntryDebugString(iters[0]).c_str());
          assert(false);
          throw std::runtime_error("No more ranges.");
        } else if (iters[i]->first != current) {
          printf(
              "Error, range (%s) doesn't match the beginning of base range "
              "(%s)\n",
              range_maps[i]->EntryDebugString(iters[i]).c_str(),
              range_maps[0]->EntryDebugString(iters[0]).c_str());
          assert(false);
          throw std::runtime_error("No more ranges.");
        }
        keys.push_back(iters[i]->second.label);
      }
    }

    bool continuous = true;

    // Inner loop: once per range within the continuous region.
    while (continuous) {
      uint64_t next_break = UINT64_MAX;

      for (int i = 0; i < iters.size(); i++) {
        next_break = std::min(next_break, range_maps[i]->RangeEnd(iters[i]));
      }

      func(keys, current, next_break);

      // Advance all iterators with ranges ending at next_break.
      for (int i = 0; i < iters.size(); i++) {
        const RangeMap& map = *range_maps[i];
        Map::const_iterator& iter = iters[i];
        uint64_t end = continuous ? map.RangeEnd(iter)
                                  : map.RangeEndUnknownLimit(iter, next_break);

        if (end != next_break) {
          continue;
        }
        ++iter;

        // Test for discontinuity.
        if (map.IterIsEnd(iter) || iter->first != next_break) {
          if (i > 0 && continuous) {
            printf(
                "Error, gap between ranges (%s) and (%s) fails to cover base "
                "range (%s)\n",
                map.EntryDebugString(std::prev(iter)).c_str(),
                map.EntryDebugString(iter).c_str(),
                range_maps[0]->EntryDebugString(iters[0]).c_str());
            assert(false);
            throw std::runtime_error("Entry range extends beyond base range");
          }
          assert(i == 0 || !continuous);
          continuous = false;
        } else {
          assert(continuous);
          keys[i] = iter->second.label;
        }
      }
      current = next_break;
    }
  }
}

}  // namespace bloaty

#endif  // BLOATY_RANGE_MAP_H_
