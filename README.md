
# Bloaty: a size profiler for binaries

[![build](https://github.com/google/bloaty/actions/workflows/build.yml/badge.svg)](https://github.com/google/bloaty/actions/workflows/build.yml)

Ever wondered what's making your binary big?  Bloaty will
show you a size profile of the binary so you can understand
what's taking up space inside.

Bloaty performs a deep analysis of the binary. Using custom
ELF, DWARF, and Mach-O parsers, Bloaty aims to accurately
attribute every byte of the binary to the symbol or
compileunit that produced it. It will even disassemble the
binary looking for references to anonymous data. For more
information about the analysis performed by Bloaty, please
see [doc/how-bloaty-works.md](doc/how-bloaty-works.md).

Bloaty works on binaries, shared objects, object files, and
static libraries (`.a` files).  The following file formats
are supported:

* ELF
* Mach-O
* PE/COFF (experimental)
* WebAssembly (experimental)

This is not an official Google product.

## Building Bloaty

To build, use `cmake`. For example:

```
$ cmake -G Ninja .
$ ninja
```

Bloaty bundles ``libprotobuf``, ``re2``, ``capstone``, and
``pkg-config`` as Git submodules, and uses ``protoc`` build
from libprotobuf, but it will prefer the system's versions
of those dependencies if available. All other dependencies
are included as Git submodules.

If the Git repository hasn't been cloned with the
`--recursive`, the submodules can be checked out with:

```
$ git submodule update --init --recursive
```

To run the tests, see the info in
[tests/README.md](tests/README.md).

# Future Work

Here are some tentative plans for future features.

## Understanding Symbol References

If we can analyze references between symbols, this would
enable a lot of features:

- Detect garbage symbols (ie. how much would the binary
  shrink if we compiled with `-ffunction-sections
  -fdata-sections -Wl,-gc-sections`).
- Understand why a particular symbol can't be
  garbage-collected (like `ld -why_live` on OS X).
- Visualize the dependency tree of symbols (probably as a
  dominator tree) so users can see the weight of their
  binary in this way.
