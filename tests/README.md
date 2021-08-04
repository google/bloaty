
# Bloaty Tests

Bloaty has two sets of tests:

- C++ tests in `tests/*.cc`
- `lit` tests in `tests/**/*.test`

We are in the process of migrating existing tests to `lit` where possible (see: https://github.com/google/bloaty/issues/221).

## `lit` tests

These tests use the `lit` and `yaml2obj` tools from LLVM:
- `yaml2obj` allows us to generate very specific ELF/Mach-O/PE files from a
  text-based YAML format.
- `lit` lets us intermix this YAML with commands to run Bloaty and make
  assertions about its output.

This is ideal for testing Bloaty's parsers, because
`yaml2obj` is a precise and readable way of constructing
input payloads.

To run these tests via CMake, a few additional parameters
must be specified currently:
- `-DLIT_EXECUTABLE=<PATH>`: specifies where to find the lit tool
- `-DFILECHECK_EXECUTABLE=<PATH>`: specifies where to find the FileCheck tool
- `-DYAML2OBJ_EXECUTABLE=<PATH>`: specifies where to find the yaml2obj tool

You can install lit via pip:
```sh
pip install --user lit
```

The FileCheck utility and yaml2obj currently need to be provided by the user.
These are part of the LLVM toolchain and require a very recent build
(development release from the main branch) to run the tests.

```sh
cmake -B build -G Ninja -S . -DLIT_EXECUTABLE=${HOME}/Library/Python/3.8/bin/lit -DFILECHECK_EXECUTABLE=${HOME}/BinaryCache/llvm.org/bin/FileCheck -DYAML2OBJ_EXECUTABLE=${HOME}/BinaryCache/llvm.org/bin/yaml2obj
cmake --build build --config Debug
cmake --build build --target check-bloaty
```


## C++ Tests

The C++ tests are conventional C++ unit tests that use https://github.com/google/googletest.

Going forward, C++ should only be used for tests that do not
parse binary input files.  For example, C++ is good for
testing Bloaty's data structures and aggregation/reporting
logic.

To run the C++ tests (Git only, these are not included in the release tarball), type:

```
$ cmake --build build --config Debug --target test
```
