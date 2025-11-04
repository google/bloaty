# DWARF5 Test Data

This directory contains test binaries for DWARF5 support.

## dwarf5_simple_exe

A simple ELF executable compiled with DWARF5 debug info to test line table parsing.

### Building the Binary

This binary should be built on Linux in a clean directory to avoid leaking
local paths in the DW_AT_producer string:

```bash
# Build in a temporary clean path
mkdir -p /tmp/src
cp dwarf5_simple.c /tmp/src/
cd /tmp/src
clang -gdwarf-5 dwarf5_simple.c -o dwarf5_simple_exe
cp dwarf5_simple_exe /path/to/bloaty/tests/testdata/dwarf5/
```

Building in `/tmp/src` ensures that the binary doesn't contain
user-specific paths, making the binary reproducible across different build environments.

### Verification

After building, verify the binary is correct:

```bash
# Should show version 5
llvm-dwarfdump --debug-line dwarf5_simple_exe | grep "version:"

# Should show clean paths (/tmp/src) in producer, not home directories
llvm-dwarfdump --debug-info dwarf5_simple_exe | grep "DW_AT_producer"
```

### Why Not YAML?

We use a pre-compiled binary instead of yaml2obj because yaml2obj doesn't
support DWARF5's structured line table format. DWARF5 uses content type
descriptors (DW_LNCT_*) and forms for directory/file entries, while yaml2obj
currently only supports DWARF4's simple string arrays.

See: https://github.com/llvm/llvm-project/issues/166441
