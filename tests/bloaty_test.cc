#include "bloaty.h"

#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <tuple>
#include <vector>
#include "testing/base/public/gunit.h"
#include "testing/base/public/gmock.h"

#define CHECK_SYSCALL(call) \
  if (call < 0) { \
    perror(#call " " __FILE__ ":" TOSTRING(__LINE__)); \
    exit(1); \
  }

namespace {

// For constructing arrays of strings in the slightly peculiar format
// required by execve().
class StrArr {
 public:
  explicit StrArr(const std::vector<std::string>& strings)
      : size_(strings.size()), array_(new char*[size_ + 1]) {
    array_[size_] = NULL;
    for (size_t i = 0; i < strings.size(); i++) {
      // Can't use c_str() directly because array_ is not const char*.
      array_[i] = strdup(strings[i].c_str());
    }
  }

  ~StrArr() {
    // unique_ptr frees the array of pointers but not the pointed-to strings.
    for (int i = 0; i < size_; i++) {
      free(array_[i]);
    }
  }

  char **get() { return array_.get(); }

 private:
  size_t size_;
  // Can't use vector<char*> because execve() takes ptr to non-const array.
  std::unique_ptr<char*[]> array_;
};

bool GetFileSize(const std::string& filename, uint64_t* size) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Couldn't get file size for: " << filename << "\n";
    return false;
  }
  fseek(file, 0L, SEEK_END);
  *size = ftell(file);
  fclose(file);
  return true;
}

#define NONE_STRING "[None]"

// Testing Bloaty requires a delicate balance.  Bloaty's output is by its
// nature very compiler and platform dependent.  So we want to verify correct
// operation without overspecifying how the platform should behave.

class BloatyTest : public ::testing::Test {
 protected:
  void CheckConsistencyForRow(const bloaty::RollupRow& row, bool is_toplevel) {
    // If any children exist, they should sum up to this row's values.
    // Also none of the children should have the same name.
    std::unordered_set<std::string> names;

    if (row.sorted_children.size() > 0) {
      uint64_t vmtotal = 0;
      uint64_t filetotal = 0;
      for (const auto& child : row.sorted_children) {
        vmtotal += child.vmsize;
        filetotal += child.filesize;
        CheckConsistencyForRow(child, false);
        ASSERT_TRUE(names.insert(child.name).second);
        ASSERT_FALSE(child.vmsize == 0 && child.filesize == 0);
      }

      if (!row.diff_mode) {
        ASSERT_EQ(vmtotal, row.vmsize);
        ASSERT_EQ(filetotal, row.filesize);
      }
    }

    if (!is_toplevel && row.sorted_children.size() == 1) {
      ASSERT_NE(NONE_STRING, row.sorted_children[0].name);
    }
  }

  void CheckConsistency() {
    CheckConsistencyForRow(*top_row_, true);
    ASSERT_EQ("TOTAL", top_row_->name);
  }

  std::string JoinStrings(const std::vector<std::string>& strings) {
    std::string ret = strings[0];
    for (size_t i = 1; i < strings.size(); i++) {
      ret += " " + strings[i];
    }
    return ret;
  }

  bool TryRunBloaty(const std::vector<std::string>& strings) {
    LOG(INFO) << "Running bloaty: " << JoinStrings(strings);
    output_.reset(new bloaty::RollupOutput);
    top_row_ = &output_->toplevel_row();
    if (bloaty::BloatyMain(strings.size(), StrArr(strings).get(),
                           output_.get())) {
      CheckConsistency();
      output_->Print(&std::cerr);
      return true;
    } else {
      LOG(INFO) << "Bloaty returned error.";
      return false;
    }
  }

  void RunBloaty(const std::vector<std::string>& strings) {
    ASSERT_TRUE(TryRunBloaty(strings));
  }

  void AssertBloatyFails(const std::vector<std::string>& strings,
                         const std::string& msg_regex) {
    // TODO(haberman): verify msg_regex by making all errors logged to a
    // standard place.
    ASSERT_FALSE(TryRunBloaty(strings));
  }

  // Special constants for asserting of children.
  static constexpr int kUnknown = -1;
  static constexpr int kSameAsVM = -2;  // Only for file size.

  void AssertChildren(
      const bloaty::RollupRow& row,
      const std::vector<std::tuple<std::string, int, int>>& children) {
    size_t i = 0;
    for (const auto& child : row.sorted_children) {
      std::string expected_name;
      int expected_vm, expected_file;
      std::tie(expected_name, expected_vm, expected_file) = children[i];

      // Excluding leading '_' is kind of a hack to exclude symbols
      // automatically inserted by the compiler, like __x86.get_pc_thunk.bx
      // for 32-bit x86 builds or _IO_stdin_used in binaries.
      //
      // Excluding leading '[' is for things like this:
      //
      //   [None]
      //   [ELF Headers]
      //   [AR Headers]
      //   etc.
      if (child.name[0] == '[' || child.name[0] == '_') {
        continue;
      }
      EXPECT_EQ(expected_name, child.name);

      // <0 indicates that we don't know what the exact size should be (for
      // example for functions).
      if (expected_vm == kUnknown) {
        // Always pass.
      } else if (expected_vm > 0) {
        EXPECT_EQ(expected_vm, child.vmsize);
      } else {
        ASSERT_TRUE(false);
      }

      if (expected_file == kUnknown) {
        // Always pass.
      } else if (expected_file == kSameAsVM) {
        EXPECT_EQ(child.vmsize, child.filesize);
      } else {
        EXPECT_EQ(expected_file, child.filesize);
      }

      if (++i == children.size()) {
        // We allow the actual data to have excess elements.
        break;
      }
    }

    // All expected elements must be present.
    ASSERT_EQ(i, children.size());
  }

  const bloaty::RollupRow* FindRow(const std::string& name) {
    for (const auto& child : top_row_->sorted_children) {
      if (child.name == name) {
        return &child;
      }
    }
    EXPECT_TRUE(false) << name;
    return nullptr;
  }

  std::unique_ptr<bloaty::RollupOutput> output_;
  const bloaty::RollupRow* top_row_;
};

constexpr int BloatyTest::kUnknown;
constexpr int BloatyTest::kSameAsVM;

TEST_F(BloatyTest, EmptyObjectFile) {
  std::string file = "01-empty.o";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  // Empty .c file should result in a .o file with no vmsize.
  RunBloaty({"bloaty", file});
  EXPECT_EQ(top_row_->vmsize, 0);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // Same with segments (we fake segments on .o files).
  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_EQ(top_row_->vmsize, 0);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // For symbols we should get a row for headers and a row for [Unmapped].
  std::string unmapped = "[Unmapped]";
  RunBloaty({"bloaty", "-d", "symbols", file});
  EXPECT_EQ(top_row_->vmsize, 0);
  EXPECT_EQ(top_row_->filesize, size);
  ASSERT_EQ(top_row_->sorted_children.size(), 2);
  EXPECT_TRUE(top_row_->sorted_children[0].name == unmapped ||
              top_row_->sorted_children[1].name == unmapped);

  // We can't run any of these targets against object files.
  std::string errmsg = "can't use data source";
  AssertBloatyFails({"bloaty", "-d", "compileunits", file}, errmsg);
  AssertBloatyFails({"bloaty", "-d", "inlines", file}, errmsg);
}

TEST_F(BloatyTest, SimpleObjectFile) {
  std::string file = "02-simple.o";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 64);
  EXPECT_LT(top_row_->vmsize, 300);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // Same with segments (we fake segments on .o files).
  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 64);
  EXPECT_LT(top_row_->vmsize, 300);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 1);

  // For inputfiles we should get everything attributed to the input file.
  RunBloaty({"bloaty", "-d", "inputfiles", file});
  AssertChildren(*top_row_, {
    std::make_tuple("02-simple.o", kUnknown, kUnknown)
  });

  // For symbols we should get entries for all our expected symbols.
  RunBloaty({"bloaty", "-d", "symbols", file});
  AssertChildren(*top_row_, {
    std::make_tuple("func1", kUnknown, kSameAsVM),
    std::make_tuple("func2", kUnknown, kSameAsVM),
    std::make_tuple("bss_a", 8, 0),
    std::make_tuple("data_a", 8, 8),
    std::make_tuple("rodata_a", 8, 8),
    std::make_tuple("bss_b", 4, 0),
    std::make_tuple("data_b", 4, 4),
    std::make_tuple("rodata_b", 4, 4),
  });

  RunBloaty({"bloaty", "-d", "sections,symbols", "-n", "50", file});

  auto row = FindRow(".bss");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("bss_a", 8, 0),
    std::make_tuple("bss_b", 4, 0),
  });

  row = FindRow(".data");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("data_a", 8, 8),
    std::make_tuple("data_b", 4, 4),
  });

  row = FindRow(".rodata");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("rodata_a", 8, 8),
    std::make_tuple("rodata_b", 4, 4),
  });
}

TEST_F(BloatyTest, SimpleArchiveFile) {
  std::string file = "03-simple.a";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  //EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 3);

  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  //EXPECT_EQ(top_row_->filesize, size);

  RunBloaty({"bloaty", "-d", "symbols", file});
  AssertChildren(*top_row_, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("long_filename_x", 12, 12),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
    std::make_tuple("foo_y", 4, 0),
    std::make_tuple("long_filename_y", 4, 4),
  });

  RunBloaty({"bloaty", "-d", "armembers,symbols", file});
  AssertChildren(*top_row_,
                 {
                     std::make_tuple("bar.o", kUnknown, kUnknown),
                     std::make_tuple("foo.o", kUnknown, kUnknown),
                     std::make_tuple("a_filename_longer_than_sixteen_chars.o",
                                     kUnknown, kUnknown),
                 });

  auto row = FindRow("bar.o");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
  });

  row = FindRow("foo.o");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_y", 4, 0),
  });

  row = FindRow("a_filename_longer_than_sixteen_chars.o");
  ASSERT_TRUE(row != nullptr);
  AssertChildren(*row, {
    std::make_tuple("long_filename_x", 12, 12),
    std::make_tuple("long_filename_y", 4, 4),
  });
}

TEST_F(BloatyTest, SimpleSharedObjectFile) {
  std::string file = "04-simple.so";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 3);

  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);

  RunBloaty({"bloaty", "-d", "symbols", file});
  AssertChildren(*top_row_, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
    std::make_tuple("foo_y", 4, 0)
  });
}

TEST_F(BloatyTest, SimpleBinary) {
  std::string file = "05-binary.bin";
  uint64_t size;
  ASSERT_TRUE(GetFileSize(file, &size));

  RunBloaty({"bloaty", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);
  EXPECT_GT(top_row_->sorted_children.size(), 3);

  RunBloaty({"bloaty", "-d", "segments", file});
  EXPECT_GT(top_row_->vmsize, 8000);
  EXPECT_LT(top_row_->vmsize, 12000);
  EXPECT_EQ(top_row_->filesize, size);

  RunBloaty({"bloaty", "-d", "symbols", file});
  AssertChildren(*top_row_, {
    std::make_tuple("bar_x", 4000, 4000),
    std::make_tuple("foo_x", 4000, 0),
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("main", kUnknown, kSameAsVM),
    std::make_tuple("bar_y", 4, 4),
    std::make_tuple("bar_z", 4, 0),
    std::make_tuple("foo_y", 4, 0)
  });

  RunBloaty({"bloaty", "-d", "compileunits,symbols", file});
  auto row = FindRow("bar.o.c");
  ASSERT_TRUE(row != nullptr);

  // This only includes functions (not data) for now.
  AssertChildren(*row, {
    std::make_tuple("bar_func", kUnknown, kSameAsVM),
  });

  row = FindRow("foo.o.c");
  ASSERT_TRUE(row != nullptr);

  // This only includes functions (not data) for now.
  AssertChildren(*row, {
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
  });

  RunBloaty({"bloaty", "-d", "sections,inlines", file});
}

TEST_F(BloatyTest, DiffMode) {
  RunBloaty({"bloaty", "06-diff.a", "--", "03-simple.a", "-d", "symbols"});
  AssertChildren(*top_row_, {
    std::make_tuple("foo_func", kUnknown, kSameAsVM),
    std::make_tuple("foo_y", 4, 0)
  });
}

}  // namespace
