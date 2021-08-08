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

#ifndef BLOATY_UTIL_H_
#define BLOATY_UTIL_H_

#include <stdexcept>

#include "absl/numeric/int128.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"

namespace bloaty {

class Error : public std::runtime_error {
 public:
  Error(const char* msg, const char* file, int line)
      : std::runtime_error(msg), file_(file), line_(line) {}

  // TODO(haberman): add these to Bloaty's error message when verbose is
  // enabled.
  const char* file() const { return file_; }
  int line() const { return line_; }

 private:
  const char* file_;
  int line_;
};

// Throwing emits a lot of code, so we do it out-of-line.
ABSL_ATTRIBUTE_NORETURN
void Throw(const char *str, int line);

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)
#define WARN(...)                                                   \
  if (verbose_level > 0) {                                          \
    printf("WARNING: %s\n", absl::Substitute(__VA_ARGS__).c_str()); \
  }

#if !defined(_MSC_VER)
#define BLOATY_UNREACHABLE() do { \
  assert(false); \
  __builtin_unreachable(); \
} while (0)
#else
#define BLOATY_UNREACHABLE() do { \
  assert(false); \
  __assume(0); \
} while (0)
#endif

#ifdef NDEBUG
// Prevent "unused variable" warnings.
#define BLOATY_ASSERT(expr) do {} while (false && (expr))
#else
#define BLOATY_ASSERT(expr) assert(expr)
#endif

inline uint64_t CheckedAdd(uint64_t a, uint64_t b) {
  absl::uint128 a_128(a), b_128(b);
  absl::uint128 c_128 = a_128 + b_128;
  if (c_128 > UINT64_MAX) {
    THROW("integer overflow in addition");
  }
  return static_cast<uint64_t>(c_128);
}

inline uint64_t CheckedMul(uint64_t a, uint64_t b) {
  absl::uint128 a_128(a), b_128(b);
  absl::uint128 c = a_128 * b_128;
  if (c > UINT64_MAX) {
    THROW("integer overflow in multiply");
  }
  return static_cast<uint64_t>(c);
}

inline absl::string_view StrictSubstr(absl::string_view data, size_t off,
                                      size_t n) {
  uint64_t end = CheckedAdd(off, n);
  if (end > data.size()) {
    THROW("region out-of-bounds");
  }
  return data.substr(off, n);
}

inline absl::string_view StrictSubstr(absl::string_view data, size_t off) {
  if (off > data.size()) {
    THROW("region out-of-bounds");
  }
  return data.substr(off);
}

inline size_t AlignUp(size_t offset, size_t granularity) {
  // Granularity must be a power of two.
  BLOATY_ASSERT((granularity & (granularity - 1)) == 0);
  return (offset + granularity - 1) & ~(granularity - 1);
}

// Endianness utilities ////////////////////////////////////////////////////////

enum class Endian { kBig, kLittle };

inline Endian GetMachineEndian() {
  int x = 1;
  return *(char *)&x == 1 ? Endian::kLittle : Endian::kBig;
}

// Generic algorithm for byte-swapping an integer of arbitrary size.
//
// With modern GCC/Clang this optimizes to a "bswap" instruction.
template <size_t N, class T> constexpr T _BS(T val) {
  if constexpr (N == 1) {
    return val & 0xff;
  } else {
    size_t bits = 8 * (N / 2);
    return (_BS<N / 2>(val) << bits) | _BS<N / 2>(val >> bits);
  }
};

// Byte swaps the given integer, and returns the byte-swapped value.
template <class T> constexpr T ByteSwap(T val) {
    return _BS<sizeof(T)>(val);
}

template <class T, size_t N = sizeof(T)> T ReadFixed(absl::string_view *data) {
  static_assert(N <= sizeof(T), "N too big for this data type");
  T val = 0;
  if (data->size() < N) {
    THROW("premature EOF reading fixed-length data");
  }
  memcpy(&val, data->data(), N);
  data->remove_prefix(N);
  return val;
}

template <class T> T ReadEndian(absl::string_view *data, Endian endian) {
  T val = ReadFixed<T>(data);
  return endian == GetMachineEndian() ? val : ByteSwap(val);
}

template <class T> T ReadLittleEndian(absl::string_view *data) {
  return ReadEndian<T>(data, Endian::kLittle);
}

template <class T> T ReadBigEndian(absl::string_view *data) {
  return ReadEndian<T>(data, Endian::kBig);
}

// General data reading  ///////////////////////////////////////////////////////

absl::string_view ReadNullTerminated(absl::string_view* data);

inline absl::string_view ReadBytes(size_t bytes, absl::string_view* data) {
  if (data->size() < bytes) {
    THROW("premature EOF reading variable-length DWARF data");
  }
  absl::string_view ret = data->substr(0, bytes);
  data->remove_prefix(bytes);
  return ret;
}

inline void SkipBytes(size_t bytes, absl::string_view* data) {
  ReadBytes(bytes, data);  // Discard result.
}

}  // namespace bloaty

#endif  // BLOATY_UTIL_H_
