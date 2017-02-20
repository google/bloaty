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
  void CheckConsistencyFor(const bloaty::RangeMap& /*map*/) {
    uint64_t last_end = 0;
    for (const auto& entry : map_.mappings_) {
      ASSERT_GT(entry.second.end, entry.first);
      ASSERT_GE(entry.first, last_end);
    }
  }

  void CheckConsistency() {
    CheckConsistencyFor(map_);
    CheckConsistencyFor(map2_);
  }

  typedef std::tuple<uint64_t, uint64_t, uint64_t, std::string> Entry;

  void AssertMapEquals(const bloaty::RangeMap& map,
                       const std::vector<Entry>& entries) {
    auto iter = map.mappings_.begin();
    size_t i = 0;
    for (; i < entries.size() && iter != map.mappings_.end(); ++i, ++iter) {
      const auto& entry = entries[i];
      ASSERT_EQ(std::get<0>(entry), iter->first) << i;
      ASSERT_EQ(std::get<1>(entry), iter->second.end) << i;
      ASSERT_EQ(std::get<2>(entry), iter->second.other_start) << i;
      ASSERT_EQ(std::get<3>(entry), iter->second.label) << i;
    }
    ASSERT_EQ(i, entries.size());
    ASSERT_EQ(iter, map.mappings_.end());
  }

  void AssertMainMapEquals(const std::vector<Entry>& entries) {
    AssertMapEquals(map_, entries);
  }

  bloaty::RangeMap map_;
  bloaty::RangeMap map2_;
  bloaty::RangeMap map3_;
};

TEST_F(RangeMapTest, AddRange) {
  CheckConsistency();
  AssertMainMapEquals({});

  map_.AddRange(4, 3, "foo");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(4, 7, UINT64_MAX, "foo")
  });

  map_.AddRange(30, 5, "bar");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(4, 7, UINT64_MAX, "foo"),
    std::make_tuple(30, 35, UINT64_MAX, "bar")
  });

  map_.AddRange(50, 0, "baz");  // No-op due to 0 size.
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(4, 7, UINT64_MAX, "foo"),
    std::make_tuple(30, 35, UINT64_MAX, "bar")
  });

  map_.AddRange(20, 5, "baz");
  map_.AddRange(25, 5, "baz2");
  map_.AddRange(40, 5, "quux");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(4, 7, UINT64_MAX, "foo"),
    std::make_tuple(20, 25, UINT64_MAX, "baz"),
    std::make_tuple(25, 30, UINT64_MAX, "baz2"),
    std::make_tuple(30, 35, UINT64_MAX, "bar"),
    std::make_tuple(40, 45, UINT64_MAX, "quux")
  });

  map_.AddRange(21, 25, "overlapping");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(4, 7, UINT64_MAX, "foo"),
    std::make_tuple(20, 25, UINT64_MAX, "baz"),
    std::make_tuple(25, 30, UINT64_MAX, "baz2"),
    std::make_tuple(30, 35, UINT64_MAX, "bar"),
    std::make_tuple(35, 40, UINT64_MAX, "overlapping"),
    std::make_tuple(40, 45, UINT64_MAX, "quux"),
    std::make_tuple(45, 46, UINT64_MAX, "overlapping")
  });

  map_.AddRange(21, 25, "overlapping no-op");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(4, 7, UINT64_MAX, "foo"),
    std::make_tuple(20, 25, UINT64_MAX, "baz"),
    std::make_tuple(25, 30, UINT64_MAX, "baz2"),
    std::make_tuple(30, 35, UINT64_MAX, "bar"),
    std::make_tuple(35, 40, UINT64_MAX, "overlapping"),
    std::make_tuple(40, 45, UINT64_MAX, "quux"),
    std::make_tuple(45, 46, UINT64_MAX, "overlapping")
  });

  map_.AddRange(0, 100, "overlap everything");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(0, 4, UINT64_MAX, "overlap everything"),
    std::make_tuple(4, 7, UINT64_MAX, "foo"),
    std::make_tuple(7, 20, UINT64_MAX, "overlap everything"),
    std::make_tuple(20, 25, UINT64_MAX, "baz"),
    std::make_tuple(25, 30, UINT64_MAX, "baz2"),
    std::make_tuple(30, 35, UINT64_MAX, "bar"),
    std::make_tuple(35, 40, UINT64_MAX, "overlapping"),
    std::make_tuple(40, 45, UINT64_MAX, "quux"),
    std::make_tuple(45, 46, UINT64_MAX, "overlapping"),
    std::make_tuple(46, 100, UINT64_MAX, "overlap everything"),
  });
}

TEST_F(RangeMapTest, Translation) {
  map_.AddDualRange(20, 5, 120, "foo");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(20, 25, 120, "foo")
  });

  map2_.AddRangeWithTranslation(15, 15, "translate me", map_, &map3_);
  CheckConsistency();
  AssertMapEquals(map2_, {
    std::make_tuple(15, 30, UINT64_MAX, "translate me")
  });
  AssertMapEquals(map3_, {
    std::make_tuple(120, 125, UINT64_MAX, "translate me")
  });

  map_.AddDualRange(1000, 30, 1100, "bar");
  map2_.AddRangeWithTranslation(1000, 5, "translate me2", map_, &map3_);
  AssertMapEquals(map2_, {
    std::make_tuple(15, 30, UINT64_MAX, "translate me"),
    std::make_tuple(1000, 1005, UINT64_MAX, "translate me2")
  });
  AssertMapEquals(map3_, {
    std::make_tuple(120, 125, UINT64_MAX, "translate me"),
    std::make_tuple(1100, 1105, UINT64_MAX, "translate me2")
  });
}

TEST_F(RangeMapTest, Translation2) {
  map_.AddRange(5, 5, "foo");
  map_.AddDualRange(20, 5, 120, "bar");
  map_.AddRange(25, 5, "baz");
  map_.AddDualRange(30, 5, 130, "quux");
  CheckConsistency();
  AssertMainMapEquals({
    std::make_tuple(5, 10, UINT64_MAX, "foo"),
    std::make_tuple(20, 25, 120, "bar"),
    std::make_tuple(25, 30, UINT64_MAX, "baz"),
    std::make_tuple(30, 35, 130, "quux")
  });

  map2_.AddRangeWithTranslation(0, 50, "translate me", map_, &map3_);
  CheckConsistency();
  AssertMapEquals(map2_, {
    std::make_tuple(0, 50, UINT64_MAX, "translate me")
  });
  AssertMapEquals(map3_, {
    std::make_tuple(120, 125, UINT64_MAX, "translate me"),
    std::make_tuple(130, 135, UINT64_MAX, "translate me")
  });
}

}  // namespace bloaty
