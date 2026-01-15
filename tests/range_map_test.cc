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

#include "bloaty.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <tuple>

namespace bloaty {

class RangeMapTest : public ::testing::Test {
 protected:
  void CheckConsistencyFor(const bloaty::RangeMap& map) {
    VMAddr last_end(0, 0);
    for (auto it = map.mappings_.begin(); it != map.mappings_.end(); ++it) {
      ASSERT_GE(it->first, last_end);
      last_end = map.RangeEnd(it);
    }
  }

  void CheckConsistency() {
    CheckConsistencyFor(map_);
    CheckConsistencyFor(map2_);
    CheckConsistencyFor(map3_);
  }

  struct Row {
    std::vector<std::string> keys;
    VMAddr start;
    VMAddr end;
  };

  void AssertRollupEquals(const std::vector<const RangeMap*> maps,
                          const std::vector<Row>& rows) {
    int i = 0;
    RangeMap::ComputeRollup(
        maps, [&i, &rows](const std::vector<std::string>& keys, VMAddr start,
                          VMAddr end) {
          ASSERT_LT(i, rows.size());
          const auto& row = rows[i];
          ASSERT_EQ(row.keys, keys);
          ASSERT_EQ(row.start, start);
          ASSERT_EQ(row.end, end);
          i++;
        });
    ASSERT_EQ(rows.size(), i);
  }

  struct Entry {
    VMAddr addr;
    VMAddr end;
    VMAddr other_start;
    std::string label;
  };

  void AssertMapEquals(const bloaty::RangeMap& map,
                       const std::vector<Entry>& entries) {
    auto iter = map.mappings_.begin();
    size_t i = 0;
    for (; i < entries.size() && iter != map.mappings_.end(); ++i, ++iter) {
      const auto& entry = entries[i];
      ASSERT_EQ(entry.addr, iter->first) << i;
      ASSERT_EQ(entry.end, map.RangeEnd(iter)) << i;
      ASSERT_EQ(entry.other_start, iter->second.other_start) << i;
      ASSERT_EQ(entry.label, iter->second.label) << i;
    }
    ASSERT_EQ(i, entries.size());
    ASSERT_EQ(iter, map.mappings_.end());

    // Also test that ComputeRollup yields the same thing.
    i = 0;
    RangeMap::ComputeRollup({&map},
                            [&i, &entries](const std::vector<std::string>& keys,
                                           VMAddr start, VMAddr end) {
                              ASSERT_LT(i, entries.size());
                              const auto& entry = entries[i];
                              ASSERT_EQ(entry.addr, start);
                              ASSERT_EQ(entry.end, end);
                              ASSERT_EQ(entry.label, keys[0]);
                              i++;
                            });
    ASSERT_EQ(entries.size(), i);
  }

  void AssertMainMapEquals(const std::vector<Entry>& entries) {
    AssertMapEquals(map_, entries);
  }

  bloaty::RangeMap map_;
  bloaty::RangeMap map2_;
  bloaty::RangeMap map3_;

  const VMAddr kNoTranslation = RangeMap::kNoTranslation();
  const uint64_t kUnknownSize = RangeMap::kUnknownSize;
};

TEST_F(RangeMapTest, AddRange) {
  CheckConsistency();
  AssertMainMapEquals({});

  map_.AddRange(VMAddr(4), 3, "foo");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"}
  });

  map_.AddRange(VMAddr(30), 5, "bar");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "bar"}
  });

  map_.AddRange(VMAddr(50), 0, "baz");  // No-op due to 0 size.
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "bar"}
  });

  map_.AddRange(VMAddr(20), 5, "baz");
  map_.AddRange(VMAddr(25), 5, "baz2");
  map_.AddRange(VMAddr(40), 5, "quux");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"},
    {VMAddr(20), VMAddr(25), kNoTranslation, "baz"},
    {VMAddr(25), VMAddr(30), kNoTranslation, "baz2"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "bar"},
    {VMAddr(40), VMAddr(45), kNoTranslation, "quux"}
  });

  map_.AddRange(VMAddr(21), 25, "overlapping");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"},
    {VMAddr(20), VMAddr(25), kNoTranslation, "baz"},
    {VMAddr(25), VMAddr(30), kNoTranslation, "baz2"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "bar"},
    {VMAddr(35), VMAddr(40), kNoTranslation, "overlapping"},
    {VMAddr(40), VMAddr(45), kNoTranslation, "quux"},
    {VMAddr(45), VMAddr(46), kNoTranslation, "overlapping"}
  });

  map_.AddRange(VMAddr(21), 25, "overlapping no-op");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"},
    {VMAddr(20), VMAddr(25), kNoTranslation, "baz"},
    {VMAddr(25), VMAddr(30), kNoTranslation, "baz2"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "bar"},
    {VMAddr(35), VMAddr(40), kNoTranslation, "overlapping"},
    {VMAddr(40), VMAddr(45), kNoTranslation, "quux"},
    {VMAddr(45), VMAddr(46), kNoTranslation, "overlapping"}
  });

  map_.AddRange(VMAddr(0), 100, "overlap everything");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(0), VMAddr(4), kNoTranslation, "overlap everything"},
    {VMAddr(4), VMAddr(7), kNoTranslation, "foo"},
    {VMAddr(7), VMAddr(20), kNoTranslation, "overlap everything"},
    {VMAddr(20), VMAddr(25), kNoTranslation, "baz"},
    {VMAddr(25), VMAddr(30), kNoTranslation, "baz2"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "bar"},
    {VMAddr(35), VMAddr(40), kNoTranslation, "overlapping"},
    {VMAddr(40), VMAddr(45), kNoTranslation, "quux"},
    {VMAddr(45), VMAddr(46), kNoTranslation, "overlapping"},
    {VMAddr(46), VMAddr(100), kNoTranslation, "overlap everything"},
  });
}

TEST_F(RangeMapTest, UnknownSize) {
  map_.AddRange(VMAddr(5), kUnknownSize, "foo");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(5), VMAddr(UINT64_MAX), kNoTranslation, "foo"}
  });

  map_.AddRange(VMAddr(100), 15, "bar");
  map_.AddRange(VMAddr(200), kUnknownSize, "baz");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(5), VMAddr(100), kNoTranslation, "foo"},
    {VMAddr(100), VMAddr(115), kNoTranslation, "bar"},
    {VMAddr(200), VMAddr(UINT64_MAX), kNoTranslation, "baz"}
  });

  map2_.AddRange(VMAddr(5), 110, "base0");
  map2_.AddRange(VMAddr(200), 50, "base1");

  AssertRollupEquals({&map2_, &map_}, {
    {{"base0", "foo"}, VMAddr(5), VMAddr(100)},
    {{"base0", "bar"}, VMAddr(100), VMAddr(115)},
    {{"base1", "baz"}, VMAddr(200), VMAddr(250)},
  });
}

TEST_F(RangeMapTest, UnknownSize2) {
  // This case is slightly weird, but we do consider the "100" below to be a
  // hard fact even if the size is unknown, so the "[95, 105]: bar" range
  // doesn't override it.
  map_.AddRange(VMAddr(100), kUnknownSize, "foo");
  map_.AddRange(VMAddr(95), 10, "bar");
  AssertMainMapEquals({
    {VMAddr(95), VMAddr(100), kNoTranslation, "bar"},
    {VMAddr(100), VMAddr(105), kNoTranslation, "foo"},
  });
}

TEST_F(RangeMapTest, UnknownSize3) {
  map_.AddRange(VMAddr(100), kUnknownSize, "foo");
  map_.AddRange(VMAddr(150), kUnknownSize, "bar");
  // This tells us the ultimate size of "foo", and we keep the "foo" label even
  // though the new label is "baz".
  map_.AddRange(VMAddr(100), 100, "baz");
  AssertMainMapEquals({
    {VMAddr(100), VMAddr(150), kNoTranslation, "foo"},
    {VMAddr(150), VMAddr(200), kNoTranslation, "bar"},
  });
}

TEST_F(RangeMapTest, UnknownSize4) {
  map_.AddRange(VMAddr(100), kUnknownSize, "foo");
  map_.AddRange(VMAddr(150), 100, "bar");
  // This tells us the ultimate size of "foo", and we keep the "foo" label even
  // though the new label is "baz".
  map_.AddRange(VMAddr(100), 100, "baz");
  AssertMainMapEquals({
    {VMAddr(100), VMAddr(150), kNoTranslation, "foo"},
    {VMAddr(150), VMAddr(250), kNoTranslation, "bar"},
  });
}

TEST_F(RangeMapTest, Bug1) {
  map_.AddRange(VMAddr(100), 20, "foo");
  map_.AddRange(VMAddr(120), 20, "bar");
  map_.AddRange(VMAddr(100), 15, "baz");
  AssertMainMapEquals({
    {VMAddr(100), VMAddr(120), kNoTranslation, "foo"},
    {VMAddr(120), VMAddr(140), kNoTranslation, "bar"},
  });
}

TEST_F(RangeMapTest, Bug2) {
  map_.AddRange(VMAddr(100), kUnknownSize, "foo");
  map_.AddRange(VMAddr(200), 50, "bar");
  map_.AddRange(VMAddr(150), 10, "baz");
  AssertMainMapEquals({
    {VMAddr(100), VMAddr(150), kNoTranslation, "foo"},
    {VMAddr(150), VMAddr(160), kNoTranslation, "baz"},
    {VMAddr(200), VMAddr(250), kNoTranslation, "bar"},
  });
}

TEST_F(RangeMapTest, Bug3) {
  map_.AddRange(VMAddr(100), kUnknownSize, "foo");
  map_.AddRange(VMAddr(200), kUnknownSize, "bar");
  map_.AddRange(VMAddr(150), 10, "baz");
  AssertMainMapEquals({
    {VMAddr(100), VMAddr(150), kNoTranslation, "foo"},
    {VMAddr(150), VMAddr(160), kNoTranslation, "baz"},
    {VMAddr(200), VMAddr(UINT64_MAX), kNoTranslation, "bar"},
  });
}

TEST_F(RangeMapTest, GetLabel) {
  map_.AddRange(VMAddr(100), kUnknownSize, "foo");
  map_.AddRange(VMAddr(200), 50, "bar");
  map_.AddRange(VMAddr(150), 10, "baz");
  AssertMainMapEquals({
    {VMAddr(100), VMAddr(150), kNoTranslation, "foo"},
    {VMAddr(150), VMAddr(160), kNoTranslation, "baz"},
    {VMAddr(200), VMAddr(250), kNoTranslation, "bar"},
  });

  std::string label;

  ASSERT_TRUE(map_.TryGetLabel(VMAddr(100), &label));
  ASSERT_EQ(label, "foo");
  ASSERT_TRUE(map_.TryGetLabel(VMAddr(155), &label));
  ASSERT_EQ(label, "baz");
  ASSERT_TRUE(map_.TryGetLabel(VMAddr(249), &label));
  ASSERT_EQ(label, "bar");
  ASSERT_FALSE(map_.TryGetLabel(VMAddr(250), &label));

  ASSERT_TRUE(map_.TryGetLabelForRange(VMAddr(100), 10, &label));
  ASSERT_EQ(label, "foo");
  ASSERT_TRUE(map_.TryGetLabelForRange(VMAddr(155), 3, &label));
  ASSERT_EQ(label, "baz");
  ASSERT_TRUE(map_.TryGetLabelForRange(VMAddr(200), 50, &label));
  ASSERT_EQ(label, "bar");
  ASSERT_FALSE(map_.TryGetLabelForRange(VMAddr(200), 51, &label));
}

TEST_F(RangeMapTest, Translation) {
  map_.AddDualRange(VMAddr(20), 5, VMAddr(120), "foo");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(20), VMAddr(25), VMAddr(120), "foo"}
  });

  ASSERT_TRUE(map2_.AddRangeWithTranslation(VMAddr(20), 5, "translate me", map_, false,
                                            &map3_));

  CheckConsistency();
  AssertMapEquals(map2_, {
    {VMAddr(20), VMAddr(25), kNoTranslation, "translate me"}
  });
  AssertMapEquals(map3_, {
    {VMAddr(120), VMAddr(125), kNoTranslation, "translate me"}
  });

  map_.AddDualRange(VMAddr(1000), 30, VMAddr(1100), "bar");
  ASSERT_TRUE(map2_.AddRangeWithTranslation(VMAddr(1000), 5, "translate me2", map_,
                                            false, &map3_));
  AssertMapEquals(map2_, {
    {VMAddr(20), VMAddr(25), kNoTranslation, "translate me"},
    {VMAddr(1000), VMAddr(1005), kNoTranslation, "translate me2"}
  });
  AssertMapEquals(map3_, {
    {VMAddr(120), VMAddr(125), kNoTranslation, "translate me"},
    {VMAddr(1100), VMAddr(1105), kNoTranslation, "translate me2"}
  });

  // Starts before base map.
  ASSERT_FALSE(map2_.AddRangeWithTranslation(VMAddr(15), 8, "translate me", map_, false,
                                             &map3_));

  // Extends past base map.
  ASSERT_FALSE(map2_.AddRangeWithTranslation(VMAddr(22), 15, "translate me", map_,
                                             false, &map3_));

  // Starts and ends in base map, but skips range in the middle.
  ASSERT_FALSE(map2_.AddRangeWithTranslation(VMAddr(20), 1000, "translate me", map_,
                                             false, &map3_));
}

TEST_F(RangeMapTest, Translation2) {
  map_.AddRange(VMAddr(5), 5, "foo");
  map_.AddDualRange(VMAddr(20), 5, VMAddr(120), "bar");
  map_.AddRange(VMAddr(25), 5, "baz");
  map_.AddDualRange(VMAddr(30), 5, VMAddr(130), "quux");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(5), VMAddr(10), kNoTranslation, "foo"},
    {VMAddr(20), VMAddr(25), VMAddr(120), "bar"},
    {VMAddr(25), VMAddr(30), kNoTranslation, "baz"},
    {VMAddr(30), VMAddr(35), VMAddr(130), "quux"}
  });

  ASSERT_TRUE(map2_.AddRangeWithTranslation(VMAddr(20), 15, "translate me", map_, false,
                                            &map3_));
  CheckConsistency();
  AssertMapEquals(map2_, {
    {VMAddr(20), VMAddr(25), kNoTranslation, "translate me"},
    {VMAddr(25), VMAddr(30), kNoTranslation, "translate me"},
    {VMAddr(30), VMAddr(35), kNoTranslation, "translate me"}
  });
  AssertMapEquals(map3_, {
    {VMAddr(120), VMAddr(125), kNoTranslation, "translate me"},
    {VMAddr(130), VMAddr(135), kNoTranslation, "translate me"}
  });
}

TEST_F(RangeMapTest, UnknownTranslation) {
  map_.AddDualRange(VMAddr(20), 10, VMAddr(120), "foo");
  CheckConsistency();
  AssertMainMapEquals({
    {VMAddr(20), VMAddr(30), VMAddr(120), "foo"}
  });

  map2_.AddRangeWithTranslation(VMAddr(25), kUnknownSize, "translate me", map_, false,
                                &map3_);
  CheckConsistency();
  AssertMapEquals(map2_, {
    {VMAddr(25), VMAddr(UINT64_MAX), kNoTranslation, "translate me"}
  });
  AssertMapEquals(map3_, {
    {VMAddr(125), VMAddr(UINT64_MAX), kNoTranslation, "translate me"}
  });

  map2_.AddRange(VMAddr(20), 10, "fallback");

  AssertRollupEquals({&map_, &map2_}, {
    {{"foo", "fallback"}, VMAddr(20), VMAddr(25)},
    {{"foo", "translate me"}, VMAddr(25), VMAddr(30)},
  });
}

}  // namespace bloaty
