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

#include "range_map.h"

#include "bloaty.h"

namespace bloaty {

constexpr uint64_t RangeMap::kUnknownSize;

template <class T>
uint64_t RangeMap::TranslateWithEntry(T iter, uint64_t addr) const {
  assert(EntryContains(iter, addr));
  assert(iter->second.HasTranslation());
  return addr - iter->first + iter->second.other_start;
}

template <class T>
bool RangeMap::TranslateAndTrimRangeWithEntry(T iter, uint64_t addr,
                                              uint64_t size, uint64_t* trimmed_addr,
                                              uint64_t* translated_addr,
                                              uint64_t* trimmed_size) const {
  addr = std::max(addr, iter->first);
  *trimmed_addr = addr;

  if (size == kUnknownSize) {
    *trimmed_size = kUnknownSize;
  } else {
    uint64_t end = std::min(addr + size, iter->first + iter->second.size);
    if (addr >= end) {
      *trimmed_size = 0;
      return false;
    }
    *trimmed_size = end - addr;
  }

  if (!iter->second.HasTranslation()) {
    return false;
  }

  *translated_addr = TranslateWithEntry(iter, addr);
  return true;
}

RangeMap::Map::const_iterator RangeMap::FindContaining(uint64_t addr) const {
  auto it = mappings_.upper_bound(addr);  // Entry directly after.
  if (it == mappings_.begin() || (--it, !EntryContains(it, addr))) {
    return mappings_.end();
  } else {
    return it;
  }
}

RangeMap::Map::iterator RangeMap::FindContainingOrAfter(uint64_t addr) {
  auto after = mappings_.upper_bound(addr);
  auto it = after;
  if (it != mappings_.begin() && (--it, EntryContains(it, addr))) {
    return it;  // Containing
  } else {
    return after;  // May be end().
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

bool RangeMap::TryGetLabel(uint64_t addr, std::string* label) const {
  auto iter = FindContaining(addr);
  if (iter == mappings_.end()) {
    return false;
  } else {
    *label = iter->second.label;
    return true;
  }
}

bool RangeMap::TryGetLabelForRange(uint64_t addr, uint64_t size,
                                   std::string* label) const {
  uint64_t end = addr + size;
  if (end < addr) {
    return false;
  }
  auto iter = FindContaining(addr);
  if (iter == mappings_.end()) {
    return false;
  } else {
    *label = iter->second.label;
    while (iter != mappings_.end() && iter->first + iter->second.size < end) {
      if (iter->second.label != *label) {
        return false;
      }
      ++iter;
    }
    return iter != mappings_.end();
  }
}

bool RangeMap::TryGetSize(uint64_t addr, uint64_t* size) const {
  auto iter = mappings_.find(addr);
  if (iter == mappings_.end()) {
    return false;
  } else {
    *size = iter->second.size;
    return true;
  }
}

std::string RangeMap::DebugString() const {
  std::string ret;
  for (auto it = mappings_.begin(); it != mappings_.end(); ++it) {
    absl::StrAppend(&ret, EntryDebugString(it), "\n");
  }
  return ret;
}

void RangeMap::AddRange(uint64_t addr, uint64_t size, const std::string& val) {
  AddDualRange(addr, size, kNoTranslation, val);
}

template <class T>
void RangeMap::MaybeSetLabel(T iter, const std::string& label, uint64_t addr,
                             uint64_t size) {
  assert(EntryContains(iter, addr));
  if (iter->second.size == kUnknownSize && size != kUnknownSize) {
    assert(addr + size >= addr);
    assert(addr + size >= iter->first);
    assert(addr >= iter->first);
    if (addr == iter->first) {
      T next = std::next(iter);
      uint64_t end = addr + size;
      if (!IterIsEnd(next)) {
        end = std::min(end, next->first);
      }
      uint64_t new_size = end - iter->first;
      if (verbose_level > 2) {
        printf("  updating mapping (%s) with new size %" PRIx64 "\n",
               EntryDebugString(addr, size, UINT64_MAX, label).c_str(),
               new_size);
      }
      // This new defined range encompassess all of the unknown-length range, so
      // just define the range to have our end.
      iter->second.size = new_size;
      CheckConsistency(iter);
    }
  } else if (verbose_level > 2) {
    printf("  skipping existing mapping (%s)\n",
           EntryDebugString(iter).c_str());
  }
}

void RangeMap::AddDualRange(uint64_t addr, uint64_t size, uint64_t otheraddr,
                            const std::string& label) {
  if (verbose_level > 2) {
    printf("%p AddDualRange([%" PRIx64 ", %" PRIx64 "], %" PRIx64 ", %s)\n",
           this, addr, size, otheraddr, label.c_str());
  }

  if (size == 0) return;

  auto it = FindContainingOrAfter(addr);

  if (size == kUnknownSize) {
    assert(otheraddr == kNoTranslation);
    if (it != mappings_.end() && EntryContainsStrict(it, addr)) {
      MaybeSetLabel(it, label, addr, kUnknownSize);
    } else {
      auto iter = mappings_.emplace_hint(
          it, std::make_pair(addr, Entry(label, kUnknownSize, kNoTranslation)));
      if (verbose_level > 2) {
        printf("  added entry: %s\n", EntryDebugString(iter).c_str());
      }
    }
    return;
  }

  const uint64_t base = addr;
  uint64_t end = addr + size;
  assert(end >= addr);

  while (1) {
    // Advance past existing entries that intersect this range until we find a
    // gap.
    while (addr < end && !IterIsEnd(it) && EntryContains(it, addr)) {
      assert(end >= addr);
      MaybeSetLabel(it, label, addr, end - addr);
      addr = RangeEndUnknownLimit(it, addr);
      ++it;
    }

    if (addr >= end) {
      return;
    }

    // We found a gap and need to create an entry.  Need to make sure the new
    // entry doesn't extend into a range that was previously defined.
    uint64_t this_end = end;
    if (it != mappings_.end() && end > it->first) {
      assert(it->first >= addr);
      this_end = std::min(end, it->first);
    }

    uint64_t other = (otheraddr == kNoTranslation) ? kNoTranslation
                                                   : addr - base + otheraddr;
    assert(this_end >= addr);
    auto iter = mappings_.emplace_hint(
        it, std::make_pair(addr, Entry(label, this_end - addr, other)));
    if (verbose_level > 2) {
      printf("  added entry: %s\n", EntryDebugString(iter).c_str());
    }
    CheckConsistency(iter);
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
bool RangeMap::AddRangeWithTranslation(uint64_t addr, uint64_t size,
                                       const std::string& val,
                                       const RangeMap& translator,
                                       bool verbose,
                                       RangeMap* other) {
  auto it = translator.FindContaining(addr);
  uint64_t end;
  if (size == kUnknownSize) {
    end = addr + 1;
  } else {
    end = addr + size;
    assert(end >= addr);
  }
  uint64_t total_size = 0;

  // TODO: optionally warn about when we span ranges of the translator.  In some
  // cases this would be a bug (ie. symbols VM->file).  In other cases it's
  // totally normal (ie. archive members file->VM).
  while (!translator.IterIsEnd(it) && it->first < end) {
    uint64_t translated_addr;
    uint64_t trimmed_addr;
    uint64_t trimmed_size;
    if (translator.TranslateAndTrimRangeWithEntry(
            it, addr, size, &trimmed_addr, &translated_addr, &trimmed_size)) {
      if (verbose_level > 2 || verbose) {
        printf("  -> translates to: [%" PRIx64 " %" PRIx64 "]\n", translated_addr,
               trimmed_size);
      }
      other->AddRange(translated_addr, trimmed_size, val);
    }
    AddRange(trimmed_addr, trimmed_size, val);
    total_size += trimmed_size;
    ++it;
  }

  return total_size == size;
}

void RangeMap::Compress() {
  auto prev = mappings_.begin();
  auto it = prev;
  while (it != mappings_.end()) {
    if (prev->first + prev->second.size == it->first &&
        (prev->second.label == it->second.label ||
         (!prev->second.HasFallbackLabel() && it->second.IsShortFallback()))) {
      prev->second.size += it->second.size;
      mappings_.erase(it++);
    } else {
      prev = it;
      ++it;
    }
  }
}

bool RangeMap::CoversRange(uint64_t addr, uint64_t size) const {
  auto it = FindContaining(addr);
  uint64_t end = addr + size;
  assert(end >= addr);

  while (true) {
    if (addr >= end) {
      return true;
    } else if (it == mappings_.end() || !EntryContains(it, addr)) {
      return false;
    }
    addr = RangeEnd(it);
    it++;
  }
}

uint64_t RangeMap::GetMaxAddress() const {
  if (mappings_.empty()) {
    return 0;
  } else {
    auto& entry = *mappings_.rbegin();
    return entry.first + entry.second.size;
  }
}

}  // namespace bloaty
