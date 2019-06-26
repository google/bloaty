
# Bloaty McBloatface: a size profiler for binaries

[![Build Status](https://travis-ci.org/google/bloaty.svg?branch=master)](https://travis-ci.org/google/bloaty)

Ever wondered what's making your binary big?  Bloaty
McBloatface will show you a size profile of the binary so
you can understand what's taking up space inside.

Bloaty works on binaries, shared objects, object files, and
static libraries (`.a` files).  The following file formats
are supported:

* ELF
* Mach-O
* WebAssembly (experimental)

These formats are NOT supported, but I am very interested
in adding support for them (I may implement these myself but
would also be happy to get contributions!)

* PE/COFF (not supported)
* Android APK (not supported, might be tricky due to compression)

This is not an official Google product.

## Building Bloaty

Bloaty uses CMake to build.  All dependencies are included as Git submodules.
To build, simply run:

```
$ cmake .
$ make -j6
```

To run tests (Git only, these are not included in the release tarball), type:

```
$ make test
```

All the normal CMake features are available, like out-of-source builds:

```
$ mkdir build
$ cd build
$ cmake ..
$ make -j6
```

## Running Bloaty

Run it directly on a binary target.  For example, run it on itself.

```
$ ./bloaty bloaty
```

On Linux you'll see output something like:

```cmdoutput
$ ./bloaty bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  30.7%  8.32Mi   0.0%       0    .debug_info
  22.0%  5.97Mi   0.0%       0    .debug_loc
  13.7%  3.71Mi   0.0%       0    .debug_str
   9.7%  2.64Mi  38.7%  2.64Mi    .rodata
   7.0%  1.89Mi  27.7%  1.89Mi    .text
   5.8%  1.57Mi   0.0%       0    .debug_line
   0.0%       0  14.8%  1.01Mi    .bss
   3.3%   928Ki   0.0%       0    .debug_ranges
   1.6%   442Ki   0.0%       0    .strtab
   1.6%   437Ki   6.3%   437Ki    .data
   1.3%   361Ki   5.2%   361Ki    .dynstr
   0.8%   235Ki   3.4%   235Ki    .eh_frame
   0.8%   219Ki   0.0%       0    .symtab
   0.5%   135Ki   0.0%       0    .debug_abbrev
   0.4%   123Ki   1.8%   123Ki    .dynsym
   0.2%  51.9Ki   0.7%  51.8Ki    .gcc_except_table
   0.1%  39.9Ki   0.6%  39.9Ki    .gnu.hash
   0.1%  37.8Ki   0.5%  37.8Ki    .eh_frame_hdr
   0.1%  15.9Ki   0.2%  14.0Ki    [24 Others]
   0.0%  10.3Ki   0.1%  10.3Ki    .gnu.version
   0.0%  5.50Ki   0.0%       0    [Unmapped]
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

The "VM SIZE" column tells you how much space the binary
will take when it is loaded into memory.  The "FILE SIZE"
column tells you about how much space the binary is taking
on disk.  These two can be very different from each other:

- Some data lives in the file but isn't loaded into memory,
  like debug information.
- Some data is mapped into memory but doesn't exist in the
  file.  This mainly applies to the `.bss` section
  (zero-initialized data).

The default breakdown in Bloaty is by sections, but many
other ways of slicing the binary are supported such as
symbols and segments.  If you compiled with debug info, you
can even break down by compile units and inlines!

```cmdoutput
$ ./bloaty bloaty -d compileunits
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  34.7%  9.38Mi  39.4%  2.68Mi    [153 Others]
  16.9%  4.58Mi   4.9%   341Ki    ../third_party/protobuf/src/google/protobuf/descriptor.cc
   8.9%  2.42Mi   4.3%   301Ki    ../third_party/protobuf/src/google/protobuf/descriptor.pb.cc
   4.1%  1.11Mi   4.5%   311Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.5%   415Ki  15.6%  1.07Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
   3.4%   944Ki   1.3%  92.9Ki    ../third_party/protobuf/src/google/protobuf/generated_message_reflection.cc
   3.3%   925Ki   1.3%  87.7Ki    ../third_party/protobuf/src/google/protobuf/text_format.cc
   3.3%   923Ki  11.8%   820Ki    ../third_party/capstone/arch/X86/X86Mapping.c
   2.6%   716Ki   0.6%  44.6Ki    ../third_party/protobuf/src/google/protobuf/descriptor_database.cc
   2.4%   676Ki   1.0%  73.1Ki    ../third_party/protobuf/src/google/protobuf/extension_set.cc
   2.2%   619Ki   0.6%  41.7Ki    ../third_party/protobuf/src/google/protobuf/generated_message_util.cc
   2.1%   584Ki   1.6%   113Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.1%   582Ki   0.7%  48.4Ki    ../third_party/protobuf/src/google/protobuf/message.cc
   1.9%   533Ki   1.9%   131Ki    ../src/bloaty.cc
   1.9%   529Ki   6.1%   427Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   1.6%   439Ki   0.5%  35.1Ki    ../third_party/protobuf/src/google/protobuf/wire_format.cc
   1.4%   394Ki   0.5%  31.5Ki    ../third_party/re2/re2/regexp.cc
   1.4%   392Ki   0.4%  28.6Ki    ../third_party/re2/re2/dfa.cc
   1.4%   383Ki   1.4%  99.4Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   1.3%   373Ki   1.0%  73.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   1.3%   370Ki   0.5%  34.6Ki    ../third_party/re2/re2/re2.cc
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```


Run Bloaty with `--help` to see a list of available options:

```cmdoutput
$ ./bloaty --help
Bloaty McBloatface: a size profiler for binaries.

USAGE: bloaty [OPTION]... FILE... [-- BASE_FILE...]

Options:

  --csv              Output in CSV format instead of human-readable.
  --tsv              Output in TSV format instead of human-readable.
  -c FILE            Load configuration from <file>.
  -d SOURCE,SOURCE   Comma-separated list of sources to scan.
  --debug-file=FILE  Use this file for debug symbols and/or symbol table.
  -C MODE            How to demangle symbols.  Possible values are:
  --demangle=MODE      --demangle=none   no demangling, print raw symbols
                       --demangle=short  demangle, but omit arg/return types
                       --demangle=full   print full demangled type
                     The default is --demangle=short.
  --disassemble=FUNCTION
                     Disassemble this function (EXPERIMENTAL)
  --domain=DOMAIN    Which domains to show.  Possible values are:
                       --domain=vm
                       --domain=file
                       --domain=both (the default)
  -n NUM             How many rows to show per level before collapsing
                     other keys into '[Other]'.  Set to '0' for unlimited.
                     Defaults to 20.
  -s SORTBY          Whether to sort by VM or File size.  Possible values
                     are:
                       -s vm
                       -s file
                       -s both (the default: sorts by max(vm, file)).
  -w                 Wide output; don't truncate long labels.
  --help             Display this message and exit.
  --list-sources     Show a list of available sources and exit.

Options for debugging Bloaty:

  --debug-vmaddr=ADDR
  --debug-fileoff=OFF
                     Print extended debugging information for the given
                     VM address and/or file offset.
  -v                 Verbose output.  Dumps warnings encountered during
                     processing and full VM/file maps at the end.
                     Add more v's (-vv, -vvv) for even more.

```

# Size Diffs

You can use Bloaty to see how the size of a binary changed.
On the command-line, pass `--` followed by the files you
want to use as the diff base.

For example, here is a size diff between a couple different versions
of Bloaty, showing how it grew when I added some features.

```
$ ./bloaty bloaty -- oldbloaty
     VM SIZE                     FILE SIZE
 --------------               --------------
  [ = ]       0 .debug_loc     +688Ki  +9.9%
   +19%  +349Ki .text          +349Ki   +19%
  [ = ]       0 .debug_ranges  +180Ki   +11%
  [ = ]       0 .debug_info    +120Ki  +0.9%
   +23% +73.5Ki .rela.dyn     +73.5Ki   +23%
  +3.5% +57.1Ki .rodata       +57.1Ki  +3.5%
 +28e3% +53.9Ki .data         +53.9Ki +28e3%
  [ = ]       0 .debug_line   +40.2Ki  +4.8%
  +2.3% +5.35Ki .eh_frame     +5.35Ki  +2.3%
  -6.0%      -5 [Unmapped]    +2.65Ki  +215%
  +0.5% +1.70Ki .dynstr       +1.70Ki  +0.5%
  [ = ]       0 .symtab       +1.59Ki  +0.9%
  [ = ]       0 .debug_abbrev +1.29Ki  +0.5%
  [ = ]       0 .strtab       +1.26Ki  +0.3%
   +16%    +992 .bss                0  [ = ]
  +0.2%    +642 [13 Others]      +849  +0.2%
  +0.6%    +792 .dynsym          +792  +0.6%
   +16%    +696 .rela.plt        +696   +16%
   +16%    +464 .plt             +464   +16%
  +0.8%    +312 .eh_frame_hdr    +312  +0.8%
  [ = ]       0 .debug_str    -19.6Ki  -0.4%
   +11%  +544Ki TOTAL         +1.52Mi  +4.6%
```

Each line shows the how much each part changed compared to
its previous size.  Most sections grew, but one section at
the bottom (`.debug_str`) shrank.  The "TOTAL" line shows
how much the size changed overall.

# Hierarchical Profiles

Bloaty supports breaking the binary down in lots of
different ways.  You can combine multiple data sources into
a single hierarchical profile.  For example, we can use the
`segments` and `sections` data sources in a single report:

```cmdoutput
$ ./bloaty -d segments,sections bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  78.5%  21.3Mi   0.0%       0    [Unmapped]
    39.1%  8.32Mi   NAN%       0    .debug_info
    28.1%  5.97Mi   NAN%       0    .debug_loc
    17.4%  3.71Mi   NAN%       0    .debug_str
     7.4%  1.57Mi   NAN%       0    .debug_line
     4.3%   928Ki   NAN%       0    .debug_ranges
     2.0%   442Ki   NAN%       0    .strtab
     1.0%   218Ki   NAN%       0    .symtab
     0.6%   135Ki   NAN%       0    .debug_abbrev
     0.0%  5.50Ki   NAN%       0    [Unmapped]
     0.0%     386   NAN%       0    .shstrtab
     0.0%     131   NAN%       0    .debug_macinfo
     0.0%      82   NAN%       0    .comment
  10.9%  2.95Mi  43.4%  2.95Mi    LOAD #4 [R]
    89.3%  2.64Mi  89.3%  2.64Mi    .rodata
     7.8%   235Ki   7.8%   235Ki    .eh_frame
     1.7%  51.8Ki   1.7%  51.8Ki    .gcc_except_table
     1.2%  37.8Ki   1.2%  37.8Ki    .eh_frame_hdr
     0.0%       7   0.0%       7    [LOAD #4 [R]]
   7.0%  1.89Mi  27.8%  1.89Mi    LOAD #3 [RX]
    99.8%  1.89Mi  99.8%  1.89Mi    .text
     0.2%  3.16Ki   0.2%  3.16Ki    .plt
     0.0%      23   0.0%      23    .init
     0.0%      12   0.0%      12    [LOAD #3 [RX]]
     0.0%       9   0.0%       9    .fini
   1.6%   441Ki  21.1%  1.44Mi    LOAD #5 [RW]
     0.0%       0  70.0%  1.01Mi    .bss
    99.1%   437Ki  29.7%   437Ki    .data
     0.4%  1.59Ki   0.1%  1.59Ki    .got.plt
     0.3%  1.34Ki   0.1%  1.34Ki    .data.rel.ro
     0.1%     544   0.0%     544    .dynamic
     0.1%     360   0.0%     360    .init_array
     0.0%      32   0.0%      32    .got
     0.0%      16   0.0%      16    .tdata
     0.0%       8   0.0%       8    .fini_array
     0.0%       8   0.0%       8    [LOAD #5 [RW]]
   2.0%   542Ki   7.8%   542Ki    LOAD #2 [R]
    66.7%   361Ki  66.7%   361Ki    .dynstr
    22.8%   123Ki  22.8%   123Ki    .dynsym
     7.4%  39.9Ki   7.4%  39.9Ki    .gnu.hash
     1.9%  10.3Ki   1.9%  10.3Ki    .gnu.version
     0.9%  4.71Ki   0.9%  4.71Ki    .rela.plt
     0.2%  1.01Ki   0.2%  1.01Ki    .rela.dyn
     0.1%     743   0.1%     743    [LOAD #2 [R]]
     0.1%     368   0.1%     368    .gnu.version_r
     0.0%      36   0.0%      36    .note.gnu.build-id
     0.0%      32   0.0%      32    .note.ABI-tag
     0.0%      28   0.0%      28    .interp
   0.0%  2.44Ki   0.0%       0    [ELF Headers]
    46.2%  1.12Ki   NAN%       0    [18 Others]
     5.1%     128   NAN%       0    [ELF Headers]
     2.6%      64   NAN%       0    .comment
     2.6%      64   NAN%       0    .data
     2.6%      64   NAN%       0    .data.rel.ro
     2.6%      64   NAN%       0    .debug_abbrev
     2.6%      64   NAN%       0    .debug_info
     2.6%      64   NAN%       0    .debug_line
     2.6%      64   NAN%       0    .debug_loc
     2.6%      64   NAN%       0    .debug_macinfo
     2.6%      64   NAN%       0    .debug_ranges
     2.6%      64   NAN%       0    .debug_str
     2.6%      64   NAN%       0    .dynamic
     2.6%      64   NAN%       0    .dynstr
     2.6%      64   NAN%       0    .dynsym
     2.6%      64   NAN%       0    .eh_frame
     2.6%      64   NAN%       0    .eh_frame_hdr
     2.6%      64   NAN%       0    .fini
     2.6%      64   NAN%       0    .fini_array
     2.6%      64   NAN%       0    .gcc_except_table
     2.6%      64   NAN%       0    .gnu.hash
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

Bloaty displays a maximum of 20 lines for each level; other
values are grouped into an `[Other]` bin.  Use `-n <num>`
to override this setting.  If you pass `-n 0`, all data
will be output without collapsing anything into `[Other]`.

# Debugging Stripped Binaries

Bloaty supports reading debuginfo/symbols from separate
binaries.  This lets you profile a stripped binary, even for
data sources like "compileunits" or "symbols" that require
this extra information.

Bloaty uses build IDs to verify that the binary and the
debug file match.  Otherwise the results would be nonsense
(this kind of mismatch might sound unlikely but it's a very
easy mistake to make, and one that I made several times even
as Bloaty's author!).

If your binary has a build ID, then using separate debug
files is as simple as:

```
$ cp bloaty bloaty.stripped
$ strip bloaty.stripped
$ ./bloaty -d symbols --debug-file=bloaty bloaty.stripped
```

Some format-specific notes follow.

## ELF

For ELF, make sure you are compiling with build IDs enabled.
With gcc this happens automatically, but [Clang decided not
to make this the default, since it makes the link
slower](http://releases.llvm.org/3.9.0/tools/clang/docs/ReleaseNotes.html#major-new-features).
For Clang add `-Wl,--build-id` to your link line.  (If you
want a slightly faster link and don't care about
reproducibility, you can use `-Wl,--build-id=uuid` instead).

Bloaty does not currently support the GNU debuglink or
looking up debug files by build ID, [which are the methods
GDB uses to find debug
files](https://sourceware.org/gdb/onlinedocs/gdb/Separate-Debug-Files.html).
If there are use cases where Bloaty's `--debug-file` option
won't work, we can reconsider implementing these.

## Mach-O

Mach-O files always have build IDs (as far as I can tell),
so no special configuration is needed to make sure you get
them.

Mach-O puts debug information in separate files which you
can create with `dsymutil`:

```
$ dsymutil bloaty
$ strip bloaty  (optional)
$ ./bloaty -d symbols --debug-file=bloaty.dSYM/Contents/Resources/DWARF/bloaty bloaty
```

# Configuration Files

Any options that you can specify on the command-line, you
can put into a configuration file instead.  Then use can use
`-c FILE` to load those options from the config file.  Also,
a few features are only available with configuration files
and cannot be specify on the command-line.

The configuration file is a in Protocol Buffers text format.
The schema is the `Options` message in
[src/bloaty.proto](src/bloaty.proto).

The two most useful cases for configuration files are:

1. You have too many input files to put on the command-line.
   At Google we sometimes run Bloaty over thousands of input
   files.  This can cause the overall command-line to exceed
   OS limits.  With a config file, we can avoid this:

   ```
   filename: "path/to/long_filename_a.o"
   filename: "path/to/long_filename_b.o"
   filename: "path/to/long_filename_c.o"
   # ...repeat for thousands of files.
   ```
2. For custom data sources, it can be very useful to put
   them in a config file, for greater reusability.  For
   example, see the custom data sources defined in
   [custom_sources.bloaty](custom_sources.bloaty).
   Also read more about custom data sources below.

# Data Sources

Bloaty has many data sources built in.  These all provide
different ways of looking at the binary.  You can also
create your own data sources by applying regexes to the
built-in data sources (see "Custom Data Sources" below).

While Bloaty works on binaries, shared objects, object
files, and static libraries (`.a` files), some of the data
sources don't work on object files.  This applies especially
to data sources that read debug info.

## Segments

Segments are what the run-time loader uses to determine what
parts of the binary need to be loaded/mapped into memory.
There are usually just a few segments: one for each set of
`mmap()` permissions required:

```cmdoutput
$ ./bloaty -d segments bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  78.5%  21.3Mi   0.0%       0    [Unmapped]
  10.9%  2.95Mi  43.4%  2.95Mi    LOAD #4 [R]
   7.0%  1.89Mi  27.8%  1.89Mi    LOAD #3 [RX]
   1.6%   441Ki  21.1%  1.44Mi    LOAD #5 [RW]
   2.0%   542Ki   7.8%   542Ki    LOAD #2 [R]
   0.0%  2.44Ki   0.0%       0    [ELF Headers]
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

Here we see one segment mapped `[RX]` (read/execute) and
one segment mapped `[RW]` (read/write).  A large part of
the binary is not loaded into memory, which we see as
`[Unmapped]`.

Object files and static libraries don't have segments.
However we fake it by grouping sections by their flags.
This gives us a break-down sort of like real segments.

```cmdoutput
$ ./bloaty -d segments CMakeFiles/libbloaty.dir/src/bloaty.cc.o
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  87.1%   906Ki   0.0%       0    Section []
   8.4%  87.3Ki  78.2%  87.3Ki    Section [AX]
   2.3%  24.3Ki  21.8%  24.3Ki    Section [A]
   2.1%  21.8Ki   0.0%       0    [ELF Headers]
   0.1%     785   0.0%       0    [Unmapped]
   0.0%      24   0.1%      72    Section [AW]
 100.0%  1.02Mi 100.0%   111Ki    TOTAL
```

## Sections

Sections give us a bit more granular look into the binary.
If we want to find the symbol table, the unwind information,
or the debug information, each kind of information lives in
its own section.  Bloaty's default output is sections.

```cmdoutput
$ ./bloaty -d sections bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  30.7%  8.32Mi   0.0%       0    .debug_info
  22.0%  5.97Mi   0.0%       0    .debug_loc
  13.7%  3.71Mi   0.0%       0    .debug_str
   9.7%  2.64Mi  38.7%  2.64Mi    .rodata
   7.0%  1.89Mi  27.7%  1.89Mi    .text
   5.8%  1.57Mi   0.0%       0    .debug_line
   0.0%       0  14.8%  1.01Mi    .bss
   3.3%   928Ki   0.0%       0    .debug_ranges
   1.6%   442Ki   0.0%       0    .strtab
   1.6%   437Ki   6.3%   437Ki    .data
   1.3%   361Ki   5.2%   361Ki    .dynstr
   0.8%   235Ki   3.4%   235Ki    .eh_frame
   0.8%   219Ki   0.0%       0    .symtab
   0.5%   135Ki   0.0%       0    .debug_abbrev
   0.4%   123Ki   1.8%   123Ki    .dynsym
   0.2%  51.9Ki   0.7%  51.8Ki    .gcc_except_table
   0.1%  39.9Ki   0.6%  39.9Ki    .gnu.hash
   0.1%  37.8Ki   0.5%  37.8Ki    .eh_frame_hdr
   0.1%  15.9Ki   0.2%  14.0Ki    [24 Others]
   0.0%  10.3Ki   0.1%  10.3Ki    .gnu.version
   0.0%  5.50Ki   0.0%       0    [Unmapped]
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

## Symbols

Symbols come from the symbol table, and represent individual
functions or variables.

```cmdoutput
$ ./bloaty -d symbols bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  30.7%  8.32Mi   0.0%       0    [section .debug_info]
  22.0%  5.97Mi   0.0%       0    [section .debug_loc]
  13.7%  3.71Mi   0.0%       0    [section .debug_str]
  13.7%  3.70Mi  44.9%  3.06Mi    [5966 Others]
   5.8%  1.57Mi   0.0%       0    [section .debug_line]
   4.8%  1.31Mi  19.2%  1.31Mi    insns
   0.0%      44  14.7%  1024Ki    g_instruction_table
   3.3%   928Ki   0.0%       0    [section .debug_ranges]
   0.9%   246Ki   3.5%   245Ki    printAliasInstr
   0.9%   237Ki   3.4%   237Ki    [section .rodata]
   0.6%   175Ki   2.5%   175Ki    insn_ops
   0.6%   153Ki   2.2%   153Ki    ARMInsts
   0.5%   140Ki   2.0%   140Ki    x86DisassemblerTwoByteOpcodes
   0.5%   135Ki   0.0%       0    [section .debug_abbrev]
   0.4%   110Ki   1.6%   109Ki    printInstruction.OpInfo
   0.3%  94.3Ki   1.3%  94.2Ki    printInstruction.OpInfo2
   0.3%  85.1Ki   1.2%  84.8Ki    insn_name_maps
   0.3%  74.0Ki   1.1%  74.0Ki    x86DisassemblerThreeByte38Opcodes
   0.2%  59.7Ki   0.9%  59.4Ki    printInstruction.AsmStrs
   0.2%  55.9Ki   0.8%  55.8Ki    DecoderTable32
   0.2%  54.0Ki   0.8%  54.0Ki    x86DisassemblerThreeByte3AOpcodes
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

You can control how symbols are demangled with the `-C MODE`
or `--demangle=MODE` flag.  You can also specify the
demangling mode explicitly in the `-d` switch.  We have
three different demangling modes:

* `-C none` or `-d rawsymbols`: no, demangling.
* `-C short` or `-d shortsymbols`: short demangling: return
  types, template parameters, and function parameter types
  are omitted.  For example:
  `bloaty::dwarf::FormReader<>::GetFunctionForForm<>()`.
  This is the default.
* `-C full` or `-d fullsymbols`: full demangling.

One very handy thing about `-C short` (the default) is that
it groups all template instantiations together, regardless
of their parameters.  You can use this to determine how much
code size you are paying by doing multiple instantiations of
templates.  Try `bloaty -d shortsymbols,fullsymbols`.

## Input Files

When you pass multiple files to Bloaty, the `inputfiles`
source will let you break it down by input file:

```cmdoutput
$ ./bloaty -d inputfiles CMakeFiles/libbloaty.dir/src/*.o
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  42.2%  1.02Mi  37.3%   111Ki    CMakeFiles/libbloaty.dir/src/bloaty.cc.o
  16.0%   394Ki  15.6%  46.7Ki    CMakeFiles/libbloaty.dir/src/dwarf.cc.o
  10.5%   257Ki  10.2%  30.6Ki    CMakeFiles/libbloaty.dir/src/bloaty.pb.cc.o
   8.8%   217Ki   9.9%  29.7Ki    CMakeFiles/libbloaty.dir/src/elf.cc.o
   8.0%   198Ki   8.9%  26.8Ki    CMakeFiles/libbloaty.dir/src/macho.cc.o
   4.4%   107Ki   4.4%  13.0Ki    CMakeFiles/libbloaty.dir/src/webassembly.cc.o
   4.2%   103Ki   7.6%  22.9Ki    CMakeFiles/libbloaty.dir/src/demangle.cc.o
   3.4%  83.8Ki   3.4%  10.2Ki    CMakeFiles/libbloaty.dir/src/range_map.cc.o
   2.5%  62.4Ki   2.6%  7.91Ki    CMakeFiles/libbloaty.dir/src/disassemble.cc.o
 100.0%  2.41Mi 100.0%   299Ki    TOTAL
```

## Archive Members

When you are running Bloaty on a `.a` file, the `armembers`
source will let you break it down by `.o` file inside the
archive.

```cmdoutput
$ ./bloaty -d armembers liblibbloaty.a
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  28.1%  1.11Mi  23.1%   112Ki    cxa_demangle.cpp.o
  25.7%  1.02Mi  22.8%   111Ki    bloaty.cc.o
   9.7%   394Ki   9.5%  46.7Ki    dwarf.cc.o
   6.4%   257Ki   6.3%  30.6Ki    bloaty.pb.cc.o
   5.4%   217Ki   6.1%  29.7Ki    elf.cc.o
   4.9%   198Ki   5.5%  26.8Ki    macho.cc.o
   2.6%   107Ki   2.7%  13.0Ki    webassembly.cc.o
   2.5%   103Ki   4.7%  22.9Ki    demangle.cc.o
   2.1%  83.8Ki   2.1%  10.2Ki    range_map.cc.o
   1.9%  77.8Ki   3.2%  15.7Ki    charconv_bigint.cc.o
   1.8%  72.7Ki   3.0%  14.6Ki    escaping.cc.o
   1.5%  62.4Ki   1.6%  7.91Ki    disassemble.cc.o
   1.4%  57.9Ki   0.0%       0    [AR Symbol Table]
   1.2%  46.9Ki   1.3%  6.60Ki    [8 Others]
   1.1%  43.9Ki   2.4%  11.7Ki    charconv.cc.o
   1.0%  38.7Ki   2.0%  9.74Ki    numbers.cc.o
   0.7%  29.9Ki   1.2%  5.71Ki    str_cat.cc.o
   0.6%  24.6Ki   0.9%  4.25Ki    string_view.cc.o
   0.5%  21.1Ki   0.7%  3.21Ki    throw_delegate.cc.o
   0.4%  17.7Ki   0.4%  2.17Ki    ascii.cc.o
   0.3%  13.7Ki   0.7%  3.52Ki    charconv_parse.cc.o
 100.0%  3.95Mi 100.0%   489Ki    TOTAL
```

You are free to use this data source even for non-`.a`
files, but it won't be very useful since it will always just
resolve to the input file (the `.a` file).

## Compile Units

Using debug information, we can tell what compile unit (and
corresponding source file) each bit of the binary came from.

```cmdoutput
$ ./bloaty -d compileunits bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  34.7%  9.38Mi  39.4%  2.68Mi    [153 Others]
  16.9%  4.58Mi   4.9%   341Ki    ../third_party/protobuf/src/google/protobuf/descriptor.cc
   8.9%  2.42Mi   4.3%   301Ki    ../third_party/protobuf/src/google/protobuf/descriptor.pb.cc
   4.1%  1.11Mi   4.5%   311Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.5%   415Ki  15.6%  1.07Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
   3.4%   944Ki   1.3%  92.9Ki    ../third_party/protobuf/src/google/protobuf/generated_message_reflection.cc
   3.3%   925Ki   1.3%  87.7Ki    ../third_party/protobuf/src/google/protobuf/text_format.cc
   3.3%   923Ki  11.8%   820Ki    ../third_party/capstone/arch/X86/X86Mapping.c
   2.6%   716Ki   0.6%  44.6Ki    ../third_party/protobuf/src/google/protobuf/descriptor_database.cc
   2.4%   676Ki   1.0%  73.1Ki    ../third_party/protobuf/src/google/protobuf/extension_set.cc
   2.2%   619Ki   0.6%  41.7Ki    ../third_party/protobuf/src/google/protobuf/generated_message_util.cc
   2.1%   584Ki   1.6%   113Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.1%   582Ki   0.7%  48.4Ki    ../third_party/protobuf/src/google/protobuf/message.cc
   1.9%   533Ki   1.9%   131Ki    ../src/bloaty.cc
   1.9%   529Ki   6.1%   427Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   1.6%   439Ki   0.5%  35.1Ki    ../third_party/protobuf/src/google/protobuf/wire_format.cc
   1.4%   394Ki   0.5%  31.5Ki    ../third_party/re2/re2/regexp.cc
   1.4%   392Ki   0.4%  28.6Ki    ../third_party/re2/re2/dfa.cc
   1.4%   383Ki   1.4%  99.4Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   1.3%   373Ki   1.0%  73.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   1.3%   370Ki   0.5%  34.6Ki    ../third_party/re2/re2/re2.cc
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

## Inlines

The DWARF debugging information also contains "line info"
information that understands inlining.  So within a
function, it will know which instructions came from an
inlined function from a header file.  This is the
information the debugger uses to point at a specific source
line as you're tracing through a program.

```cmdoutput
$ ./bloaty -d inlines bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  30.7%  8.32Mi   0.0%       0    [section .debug_info]
  22.0%  5.97Mi   0.0%       0    [section .debug_loc]
  13.7%  3.71Mi   0.0%       0    [section .debug_str]
   9.7%  2.64Mi  38.7%  2.64Mi    [section .rodata]
   6.9%  1.86Mi  27.3%  1.86Mi    [43370 Others]
   5.8%  1.57Mi   0.0%       0    [section .debug_line]
   0.0%       0  14.8%  1.01Mi    [section .bss]
   3.3%   928Ki   0.0%       0    [section .debug_ranges]
   1.6%   442Ki   0.0%       0    [section .strtab]
   1.6%   437Ki   6.3%   437Ki    [section .data]
   1.3%   361Ki   5.2%   361Ki    [section .dynstr]
   0.8%   235Ki   3.4%   235Ki    [section .eh_frame]
   0.8%   219Ki   0.0%       0    [section .symtab]
   0.5%   135Ki   0.0%       0    [section .debug_abbrev]
   0.4%   123Ki   1.8%   123Ki    [section .dynsym]
   0.2%  51.9Ki   0.7%  51.8Ki    [section .gcc_except_table]
   0.1%  39.9Ki   0.6%  39.9Ki    [section .gnu.hash]
   0.1%  37.8Ki   0.5%  37.8Ki    [section .eh_frame_hdr]
   0.1%  25.2Ki   0.4%  25.2Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/8/../../../../include/c++/8/bits/basic_string.h:176
   0.1%  16.8Ki   0.2%  16.8Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/8/../../../../include/c++/8/bits/basic_string.h:172
   0.1%  15.4Ki   0.2%  15.4Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/8/../../../../include/c++/8/ext/new_allocator.h:125
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

# Custom Data Sources

Sometimes you want to munge the labels from an existing data
source.  For example, when we use "compileunits" on Bloaty
itself, we see files from all our dependencies mixed
together:

```cmdoutput
$ ./bloaty -d compileunits bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  34.7%  9.38Mi  39.4%  2.68Mi    [153 Others]
  16.9%  4.58Mi   4.9%   341Ki    ../third_party/protobuf/src/google/protobuf/descriptor.cc
   8.9%  2.42Mi   4.3%   301Ki    ../third_party/protobuf/src/google/protobuf/descriptor.pb.cc
   4.1%  1.11Mi   4.5%   311Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.5%   415Ki  15.6%  1.07Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
   3.4%   944Ki   1.3%  92.9Ki    ../third_party/protobuf/src/google/protobuf/generated_message_reflection.cc
   3.3%   925Ki   1.3%  87.7Ki    ../third_party/protobuf/src/google/protobuf/text_format.cc
   3.3%   923Ki  11.8%   820Ki    ../third_party/capstone/arch/X86/X86Mapping.c
   2.6%   716Ki   0.6%  44.6Ki    ../third_party/protobuf/src/google/protobuf/descriptor_database.cc
   2.4%   676Ki   1.0%  73.1Ki    ../third_party/protobuf/src/google/protobuf/extension_set.cc
   2.2%   619Ki   0.6%  41.7Ki    ../third_party/protobuf/src/google/protobuf/generated_message_util.cc
   2.1%   584Ki   1.6%   113Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.1%   582Ki   0.7%  48.4Ki    ../third_party/protobuf/src/google/protobuf/message.cc
   1.9%   533Ki   1.9%   131Ki    ../src/bloaty.cc
   1.9%   529Ki   6.1%   427Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   1.6%   439Ki   0.5%  35.1Ki    ../third_party/protobuf/src/google/protobuf/wire_format.cc
   1.4%   394Ki   0.5%  31.5Ki    ../third_party/re2/re2/regexp.cc
   1.4%   392Ki   0.4%  28.6Ki    ../third_party/re2/re2/dfa.cc
   1.4%   383Ki   1.4%  99.4Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   1.3%   373Ki   1.0%  73.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   1.3%   370Ki   0.5%  34.6Ki    ../third_party/re2/re2/re2.cc
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

If we want to bucket all of these by which library they came
from, we can write a custom data source.  It specifies the
base data source and a set of regexes to apply to it.  The
regexes are tried in order, and the first matching regex
will cause the entire label to be rewritten to the
replacement text.  Regexes follow [RE2
syntax](https://github.com/google/re2/wiki/Syntax) and the
replacement can refer to capture groups.

```cmdoutput
$ cat bloaty_package.bloaty
custom_data_source: {
  name: "bloaty_package"
  base_data_source: "compileunits"

  rewrite: {
    pattern: "^(\\.\\./)?src"
    replacement: "src"
  }
  rewrite: {
    pattern: "^(\\.\\./)?(third_party/\\w+)"
    replacement: "\\2"
  }
}
```

Then use the data source like so:

```cmdoutput
$ ./bloaty -c bloaty_package.bloaty -d bloaty_package bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  53.0%  14.3Mi  19.3%  1.32Mi    third_party/protobuf
  28.1%  7.60Mi  64.2%  4.37Mi    third_party/capstone
   9.1%  2.47Mi   3.6%   253Ki    third_party/re2
   5.0%  1.34Mi   4.7%   326Ki    src
   2.1%   584Ki   1.6%   113Ki    third_party/demumble
   0.7%   199Ki   1.2%  84.0Ki    third_party/abseil
   0.7%   194Ki   2.8%   194Ki    [section .rodata]
   0.2%  68.4Ki   0.0%       0    [section .debug_str]
   0.2%  51.8Ki   0.7%  51.8Ki    [section .gcc_except_table]
   0.2%  43.5Ki   0.0%       0    [section .symtab]
   0.1%  39.9Ki   0.6%  39.9Ki    [section .gnu.hash]
   0.1%  37.0Ki   0.5%  37.0Ki    [section .text]
   0.1%  31.2Ki   0.0%       0    [section .debug_loc]
   0.1%  27.8Ki   0.0%       0    [section .strtab]
   0.1%  15.0Ki   0.2%  15.0Ki    [section .dynstr]
   0.1%  14.3Ki   0.2%  11.4Ki    [28 Others]
   0.0%  10.3Ki   0.1%  10.3Ki    [section .gnu.version]
   0.0%  8.34Ki   0.1%  8.34Ki    [section .dynsym]
   0.0%  5.92Ki   0.0%       0    [section .debug_ranges]
   0.0%  5.50Ki   0.0%       0    [Unmapped]
   0.0%  4.18Ki   0.1%  4.18Ki    [section .eh_frame]
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

We can get an even richer report by combining the
`bloaty_package` source with the original `compileunits`
source:

```cmdoutput
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  53.0%  14.3Mi  19.3%  1.32Mi    third_party/protobuf
    31.9%  4.58Mi  25.4%   341Ki    ../third_party/protobuf/src/google/protobuf/descriptor.cc
    16.9%  2.42Mi  22.4%   301Ki    ../third_party/protobuf/src/google/protobuf/descriptor.pb.cc
     6.4%   944Ki   6.9%  92.9Ki    ../third_party/protobuf/src/google/protobuf/generated_message_reflection.cc
     6.3%   925Ki   6.5%  87.7Ki    ../third_party/protobuf/src/google/protobuf/text_format.cc
     4.9%   716Ki   3.3%  44.6Ki    ../third_party/protobuf/src/google/protobuf/descriptor_database.cc
     4.6%   676Ki   5.4%  73.1Ki    ../third_party/protobuf/src/google/protobuf/extension_set.cc
     4.2%   619Ki   3.1%  41.7Ki    ../third_party/protobuf/src/google/protobuf/generated_message_util.cc
     4.0%   582Ki   3.6%  48.4Ki    ../third_party/protobuf/src/google/protobuf/message.cc
     3.7%   538Ki   4.1%  54.7Ki    [13 Others]
     3.0%   439Ki   2.6%  35.1Ki    ../third_party/protobuf/src/google/protobuf/wire_format.cc
     2.3%   331Ki   2.7%  35.9Ki    ../third_party/protobuf/src/google/protobuf/map_field.cc
     1.9%   281Ki   2.5%  33.4Ki    ../third_party/protobuf/src/google/protobuf/stubs/strutil.cc
     1.9%   277Ki   1.4%  18.7Ki    ../third_party/protobuf/src/google/protobuf/dynamic_message.cc
     1.8%   262Ki   1.5%  19.6Ki    ../third_party/protobuf/src/google/protobuf/extension_set_heavy.cc
     1.3%   194Ki   3.1%  41.7Ki    ../third_party/protobuf/src/google/protobuf/io/tokenizer.cc
     1.2%   175Ki   1.5%  20.3Ki    ../third_party/protobuf/src/google/protobuf/wire_format_lite.cc
     0.9%   129Ki   0.8%  10.6Ki    ../third_party/protobuf/src/google/protobuf/unknown_field_set.cc
     0.9%   128Ki   0.5%  7.41Ki    ../third_party/protobuf/src/google/protobuf/reflection_ops.cc
     0.8%   123Ki   0.9%  11.7Ki    ../third_party/protobuf/src/google/protobuf/stubs/common.cc
     0.6%  95.2Ki   1.0%  13.0Ki    ../third_party/protobuf/src/google/protobuf/message_lite.cc
     0.5%  77.2Ki   0.9%  12.8Ki    ../third_party/protobuf/src/google/protobuf/io/coded_stream.cc
  28.1%  7.60Mi  64.2%  4.37Mi    third_party/capstone
    15.6%  1.19Mi   7.1%   319Ki    [36 Others]
    14.6%  1.11Mi   7.0%   311Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
     5.3%   415Ki  24.4%  1.07Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
    11.9%   923Ki  18.3%   820Ki    ../third_party/capstone/arch/X86/X86Mapping.c
     6.8%   529Ki   9.6%   427Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
     4.9%   383Ki   2.2%  99.4Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
     4.8%   373Ki   1.6%  73.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
     4.3%   333Ki   1.2%  54.2Ki    ../third_party/capstone/arch/Mips/MipsDisassembler.c
     3.7%   290Ki   3.4%   150Ki    ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
     3.2%   249Ki   1.9%  83.6Ki    ../third_party/capstone/arch/ARM/ARMInstPrinter.c
     3.2%   245Ki   4.9%   220Ki    ../third_party/capstone/arch/AArch64/AArch64Mapping.c
     2.9%   222Ki   4.4%   196Ki    ../third_party/capstone/arch/ARM/ARMMapping.c
     2.8%   221Ki   2.2%  98.6Ki    ../third_party/capstone/arch/X86/X86IntelInstPrinter.c
     2.6%   201Ki   2.1%  95.9Ki    ../third_party/capstone/arch/PowerPC/PPCInstPrinter.c
     2.1%   166Ki   3.0%   133Ki    ../third_party/capstone/arch/Mips/MipsMapping.c
     2.0%   157Ki   0.4%  18.6Ki    ../third_party/capstone/arch/X86/X86Disassembler.c
     2.0%   156Ki   0.6%  28.8Ki    ../third_party/capstone/arch/PowerPC/PPCDisassembler.c
     2.0%   155Ki   2.8%   126Ki    ../third_party/capstone/arch/PowerPC/PPCMapping.c
     2.0%   154Ki   2.0%  90.9Ki    ../third_party/capstone/arch/Sparc/SparcInstPrinter.c
     1.7%   131Ki   0.4%  17.8Ki    ../third_party/capstone/arch/TMS320C64x/TMS320C64xDisassembler.c
     1.6%   121Ki   0.4%  19.8Ki    ../third_party/capstone/arch/SystemZ/SystemZDisassembler.c
   9.1%  2.47Mi   3.6%   253Ki    third_party/re2
    15.6%   394Ki  12.4%  31.5Ki    ../third_party/re2/re2/regexp.cc
    15.5%   392Ki  11.3%  28.6Ki    ../third_party/re2/re2/dfa.cc
    14.7%   370Ki  13.6%  34.6Ki    ../third_party/re2/re2/re2.cc
    12.0%   304Ki  29.9%  76.0Ki    ../third_party/re2/re2/parse.cc
    11.7%   295Ki   8.8%  22.4Ki    ../third_party/re2/re2/prog.cc
     9.3%   234Ki   8.6%  21.8Ki    ../third_party/re2/re2/compile.cc
     4.6%   114Ki   4.8%  12.1Ki    ../third_party/re2/re2/simplify.cc
     4.1%   103Ki   3.5%  8.91Ki    ../third_party/re2/re2/nfa.cc
     3.5%  89.1Ki   1.7%  4.42Ki    ../third_party/re2/re2/onepass.cc
     2.9%  74.3Ki   1.4%  3.46Ki    ../third_party/re2/re2/tostring.cc
     2.1%  52.5Ki   1.8%  4.61Ki    ../third_party/re2/re2/bitstate.cc
     1.2%  29.3Ki   0.7%  1.90Ki    ../third_party/re2/re2/stringpiece.cc
     1.0%  24.7Ki   0.8%  2.08Ki    ../third_party/re2/util/strutil.cc
     0.9%  23.2Ki   0.0%       0    ../third_party/re2/re2/unicode_groups.cc
     0.4%  9.36Ki   0.0%       0    ../third_party/re2/re2/perl_groups.cc
     0.3%  8.51Ki   0.0%       0    ../third_party/re2/re2/unicode_casefold.cc
     0.2%  5.13Ki   0.5%  1.38Ki    ../third_party/re2/util/rune.cc
   5.0%  1.34Mi   4.7%   326Ki    src
    38.8%   533Ki  40.2%   131Ki    ../src/bloaty.cc
    14.1%   193Ki  15.0%  49.0Ki    ../src/dwarf.cc
    10.4%   142Ki   0.5%  1.53Ki    ../src/main.cc
     9.0%   123Ki  11.4%  37.1Ki    src/bloaty.pb.cc
     7.4%   102Ki   8.0%  26.1Ki    ../src/elf.cc
     7.2%  99.6Ki  10.1%  33.0Ki    ../src/macho.cc
     4.8%  65.4Ki   6.0%  19.6Ki    ../src/demangle.cc
     3.5%  48.5Ki   4.0%  13.2Ki    ../src/webassembly.cc
     2.8%  38.6Ki   2.8%  9.19Ki    ../src/range_map.cc
     1.9%  25.6Ki   2.0%  6.61Ki    ../src/disassemble.cc
   2.1%   584Ki   1.6%   113Ki    third_party/demumble
   100.0%   584Ki 100.0%   113Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   0.7%   199Ki   1.2%  84.0Ki    third_party/abseil
    20.0%  40.0Ki  18.7%  15.7Ki    ../third_party/abseil-cpp/absl/strings/internal/charconv_bigint.cc
    19.6%  39.2Ki  14.5%  12.2Ki    ../third_party/abseil-cpp/absl/strings/escaping.cc
    19.5%  38.9Ki  30.8%  25.9Ki    ../third_party/abseil-cpp/absl/strings/charconv.cc
     9.8%  19.6Ki  10.6%  8.94Ki    ../third_party/abseil-cpp/absl/strings/numbers.cc
     7.4%  14.9Ki   6.1%  5.11Ki    ../third_party/abseil-cpp/absl/strings/str_cat.cc
     6.7%  13.3Ki   5.6%  4.74Ki    ../third_party/abseil-cpp/absl/strings/string_view.cc
     3.6%  7.18Ki   1.8%  1.54Ki    ../third_party/abseil-cpp/absl/strings/ascii.cc
     3.0%  6.07Ki   3.1%  2.58Ki    ../third_party/abseil-cpp/absl/strings/internal/charconv_parse.cc
     3.0%  5.99Ki   2.1%  1.73Ki    ../third_party/abseil-cpp/absl/strings/str_split.cc
     2.6%  5.17Ki   2.3%  1.94Ki    ../third_party/abseil-cpp/absl/strings/substitute.cc
     2.0%  4.00Ki   2.1%  1.77Ki    ../third_party/abseil-cpp/absl/base/internal/raw_logging.cc
     1.2%  2.42Ki   1.5%  1.23Ki    ../third_party/abseil-cpp/absl/strings/internal/memutil.cc
     1.2%  2.33Ki   0.5%     441    ../third_party/abseil-cpp/absl/base/internal/throw_delegate.cc
     0.3%     634   0.3%     233    ../third_party/abseil-cpp/absl/strings/internal/utf8.cc
   0.7%   194Ki   2.8%   194Ki    [section .rodata]
   0.2%  68.4Ki   0.0%       0    [section .debug_str]
   0.2%  51.8Ki   0.7%  51.8Ki    [section .gcc_except_table]
   0.2%  43.5Ki   0.0%       0    [section .symtab]
   0.1%  39.9Ki   0.6%  39.9Ki    [section .gnu.hash]
   0.1%  37.0Ki   0.5%  37.0Ki    [section .text]
   0.1%  31.2Ki   0.0%       0    [section .debug_loc]
   0.1%  27.8Ki   0.0%       0    [section .strtab]
   0.1%  15.0Ki   0.2%  15.0Ki    [section .dynstr]
   0.1%  14.3Ki   0.2%  11.4Ki    [28 Others]
   0.0%  10.3Ki   0.1%  10.3Ki    [section .gnu.version]
   0.0%  8.34Ki   0.1%  8.34Ki    [section .dynsym]
   0.0%  5.92Ki   0.0%       0    [section .debug_ranges]
   0.0%  5.50Ki   0.0%       0    [Unmapped]
   0.0%  4.18Ki   0.1%  4.18Ki    [section .eh_frame]
 100.0%  27.1Mi 100.0%  6.81Mi    TOTAL
```

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
