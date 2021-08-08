
# Bloaty: a size profiler for binaries

[![build](https://github.com/google/bloaty/actions/workflows/build.yml/badge.svg)](https://github.com/google/bloaty/actions/workflows/build.yml)

Ever wondered what's making your binary big?  Bloaty will
show you a size profile of the binary so you can understand
what's taking up space inside.

```cmdoutput
$ ./bloaty bloaty -d compileunits
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  34.8%  10.2Mi  43.4%  2.91Mi    [163 Others]
  17.2%  5.08Mi   4.3%   295Ki    third_party/protobuf/src/google/protobuf/descriptor.cc
   7.3%  2.14Mi   2.6%   179Ki    third_party/protobuf/src/google/protobuf/descriptor.pb.cc
   4.6%  1.36Mi   1.1%  78.4Ki    third_party/protobuf/src/google/protobuf/text_format.cc
   3.7%  1.10Mi   4.5%   311Ki    third_party/capstone/arch/ARM/ARMDisassembler.c
   1.3%   399Ki  15.9%  1.07Mi    third_party/capstone/arch/M68K/M68KDisassembler.c
   3.2%   980Ki   1.1%  75.3Ki    third_party/protobuf/src/google/protobuf/generated_message_reflection.cc
   3.2%   965Ki   0.6%  40.7Ki    third_party/protobuf/src/google/protobuf/descriptor_database.cc
   2.8%   854Ki  12.0%   819Ki    third_party/capstone/arch/X86/X86Mapping.c
   2.8%   846Ki   1.0%  66.4Ki    third_party/protobuf/src/google/protobuf/extension_set.cc
   2.7%   800Ki   0.6%  41.2Ki    third_party/protobuf/src/google/protobuf/generated_message_util.cc
   2.3%   709Ki   0.7%  50.7Ki    third_party/protobuf/src/google/protobuf/wire_format.cc
   2.1%   637Ki   1.7%   117Ki    third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   1.8%   549Ki   1.7%   114Ki    src/bloaty.cc
   1.7%   503Ki   0.7%  48.1Ki    third_party/protobuf/src/google/protobuf/repeated_field.cc
   1.6%   469Ki   6.2%   427Ki    third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   1.4%   434Ki   0.2%  15.9Ki    third_party/protobuf/src/google/protobuf/message.cc
   1.4%   422Ki   0.3%  23.4Ki    third_party/re2/re2/dfa.cc
   1.3%   407Ki   0.4%  24.9Ki    third_party/re2/re2/regexp.cc
   1.3%   407Ki   0.4%  29.9Ki    third_party/protobuf/src/google/protobuf/map_field.cc
   1.3%   397Ki   0.4%  24.8Ki    third_party/re2/re2/re2.cc
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
```

Bloaty performs a deep analysis of the binary. Using custom
ELF, DWARF, and Mach-O parsers, Bloaty aims to accurately
attribute every byte of the binary to the symbol or
compileunit that produced it. It will even disassemble the
binary looking for references to anonymous data.

Bloaty supports many features:

- **file formats:** ELF, Mach-O, PE/COFF (experimental), WebAssembly (experimental)
- **data sources:** compileunit (shown above), symbol, section, segment, etc.
- **hierarchical profiles:** combine multiple data sources into a single report
- **size diffs:** see where the binary grew, perfect for CI tests
- **separate debug files:** strip the binary under test, while making debug data available for analysis
- **flexible demangling:** demangle C++ symbols, optionally discarding function/template parameters
- **custom data sources:** regex rewrites of built-in data sources, for custom munging/bucketing
- **regex filtering:** filter out parts of the binary that do or don't match a given regex
- **easy to deploy:** statically-linked C++ binary, easy to copy around

For detailed info on all of Bloaty's features, see the [User
Documentation](doc/using.md).

For more information about the analysis performed by Bloaty,
please see [How Bloaty Works](doc/how-bloaty-works.md).


## Install

To build, use `cmake`. For example:

```
$ cmake -B build -G Ninja -S .
$ cmake --build build
$ cmake --build build --target install
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

## Support

GitHub issues and PRs welcome.  Please include tests when possible, see: 
[tests/README.md](tests/README.md).

This is not an official Google product.
