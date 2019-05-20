
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

To build Bloaty from source, the protobuf compiler is needed.
On Ubuntu/Debian, you can install it with:
```
$ sudo apt-get install protobuf-compiler
```

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

```
     VM SIZE                         FILE SIZE
 --------------                   --------------
   0.0%       0 .debug_info        10.7Mi  37.1%
   0.0%       0 .debug_loc         5.39Mi  18.6%
   0.0%       0 .debug_str         4.48Mi  15.5%
  37.7%  1.86Mi .text              1.86Mi   6.4%
   0.0%       0 .debug_ranges      1.67Mi   5.8%
  32.6%  1.61Mi .rodata            1.61Mi   5.6%
   0.0%       0 .debug_line         856Ki   2.9%
   0.0%       0 .strtab             470Ki   1.6%
   7.2%   362Ki .dynstr             362Ki   1.2%
   6.4%   321Ki .rela.dyn           321Ki   1.1%
   6.1%   307Ki .data.rel.ro        307Ki   1.0%
   0.0%       0 .debug_abbrev       241Ki   0.8%
   4.6%   232Ki .eh_frame           232Ki   0.8%
   0.0%       0 .symtab             188Ki   0.6%
   2.5%   123Ki .dynsym             123Ki   0.4%
   1.0%  48.4Ki .gcc_except_table  48.4Ki   0.2%
   0.8%  39.8Ki .gnu.hash          39.8Ki   0.1%
   0.7%  36.6Ki .eh_frame_hdr      36.6Ki   0.1%
   0.0%       0 .debug_aranges     27.1Ki   0.1%
   0.4%  17.7Ki [23 Others]        14.5Ki   0.0%
   0.2%  10.3Ki .gnu.version       10.3Ki   0.0%
 100.0%  4.93Mi TOTAL              28.9Mi 100.0%
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

```
$ ./bloaty bloaty -d compileunits
     VM SIZE                                                                                   FILE SIZE
 --------------                                                                             --------------
  44.2%  2.18Mi [137 Others]                                                                 10.7Mi  36.9%
   5.4%   271Ki ../third_party/protobuf/src/google/protobuf/descriptor.cc                    3.92Mi  13.5%
   7.1%   360Ki ../third_party/protobuf/src/google/protobuf/descriptor.pb.cc                 2.39Mi   8.2%
   8.3%   416Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c                           1.57Mi   5.4%
   1.7%  87.4Ki ../third_party/protobuf/src/google/protobuf/text_format.cc                   1.00Mi   3.5%
   2.1%   106Ki ../third_party/protobuf/src/google/protobuf/generated_message_reflection.cc   959Ki   3.2%
   0.8%  38.1Ki ../third_party/protobuf/src/google/protobuf/descriptor_database.cc            771Ki   2.6%
   1.5%  73.4Ki ../third_party/protobuf/src/google/protobuf/message.cc                        754Ki   2.5%
   2.5%   126Ki ../src/bloaty.cc                                                              753Ki   2.5%
   0.9%  43.5Ki ../third_party/re2/re2/dfa.cc                                                 648Ki   2.2%
   1.2%  60.5Ki ../third_party/protobuf/src/google/protobuf/extension_set.cc                  610Ki   2.1%
   0.8%  42.0Ki ../third_party/re2/re2/re2.cc                                                 595Ki   2.0%
   0.6%  28.2Ki ../third_party/protobuf/src/google/protobuf/generated_message_util.cc         572Ki   1.9%
   1.1%  56.5Ki ../third_party/protobuf/src/google/protobuf/map_field.cc                      565Ki   1.9%
   0.8%  42.5Ki ../third_party/re2/re2/regexp.cc                                              543Ki   1.8%
   1.8%  91.3Ki ../third_party/capstone/arch/AArch64/AArch64Disassembler.c                    523Ki   1.8%
   1.0%  50.9Ki ../third_party/protobuf/src/google/protobuf/wire_format.cc                    520Ki   1.8%
   1.8%  88.8Ki ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp                490Ki   1.7%
   3.2%   163Ki ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c                     456Ki   1.5%
   6.5%   329Ki ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c                     427Ki   1.4%
   6.7%   337Ki ../third_party/capstone/arch/X86/X86Mapping.c                                 417Ki   1.4%
 100.0%  4.93Mi TOTAL                                                                        28.9Mi 100.0%
```


Run Bloaty with `--help` to see a list of available options:

```
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

```
$ bloaty -d segments,sections bloaty
     VM SIZE                              FILE SIZE
 --------------                        --------------
   0.0%       0 [Unmapped]              24.0Mi  83.0%
       NAN%       0 .debug_info             10.7Mi  44.7%
       NAN%       0 .debug_loc              5.39Mi  22.4%
       NAN%       0 .debug_str              4.48Mi  18.7%
       NAN%       0 .debug_ranges           1.67Mi   6.9%
       NAN%       0 .debug_line              856Ki   3.5%
       NAN%       0 .strtab                  470Ki   1.9%
       NAN%       0 .debug_abbrev            241Ki   1.0%
       NAN%       0 .symtab                  188Ki   0.8%
       NAN%       0 .debug_aranges          27.1Ki   0.1%
       NAN%       0 .shstrtab                  390   0.0%
       NAN%       0 [Unmapped]                 118   0.0%
       NAN%       0 .comment                    28   0.0%
  93.7%  4.62Mi LOAD [RX]               4.62Mi  16.0%
      40.2%  1.86Mi .text                   1.86Mi  40.2%
      34.8%  1.61Mi .rodata                 1.61Mi  34.8%
       7.7%   362Ki .dynstr                  362Ki   7.7%
       6.8%   321Ki .rela.dyn                321Ki   6.8%
       4.9%   232Ki .eh_frame                232Ki   4.9%
       2.6%   123Ki .dynsym                  123Ki   2.6%
       1.0%  48.4Ki .gcc_except_table       48.4Ki   1.0%
       0.8%  39.8Ki .gnu.hash               39.8Ki   0.8%
       0.8%  36.6Ki .eh_frame_hdr           36.6Ki   0.8%
       0.2%  10.3Ki .gnu.version            10.3Ki   0.2%
       0.1%  4.36Ki .rela.plt               4.36Ki   0.1%
       0.1%  2.92Ki .plt                    2.92Ki   0.1%
       0.0%     624 [ELF Headers]              624   0.0%
       0.0%     384 .gnu.version_r             384   0.0%
       0.0%     104 .plt.got                   104   0.0%
       0.0%      39 [LOAD [RX]]                 39   0.0%
       0.0%      36 .note.gnu.build-id          36   0.0%
       0.0%      32 .note.ABI-tag               32   0.0%
       0.0%      28 .interp                     28   0.0%
       0.0%      23 .init                       23   0.0%
       0.0%       9 [1 Others]                   9   0.0%
   6.3%   316Ki LOAD [RW]                310Ki   1.0%
      97.1%   307Ki .data.rel.ro             307Ki  99.1%
       2.0%  6.20Ki .bss                         0   0.0%
       0.5%  1.48Ki .got.plt                1.48Ki   0.5%
       0.2%     560 .dynamic                   560   0.2%
       0.1%     352 .init_array                352   0.1%
       0.1%     328 .data                      328   0.1%
       0.1%     192 .got                       192   0.1%
       0.0%      56 [LOAD [RW]]                 32   0.0%
       0.0%      16 .tdata                      16   0.0%
       0.0%       8 .fini_array                  8   0.0%
   0.0%       0 [ELF Headers]           2.50Ki   0.0%
 100.0%  4.93Mi TOTAL                   28.9Mi 100.0%
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

```
$ bloaty -d segments bloaty
      VM SIZE                     FILE SIZE
 --------------               --------------
   0.0%       0 [Unmapped]     7.31Mi  94.2%
  99.2%   452Ki LOAD [RX]       452Ki   5.7%
   0.8%  3.46Ki LOAD [RW]      1.88Ki   0.0%
   0.0%       0 [ELF Headers]  2.38Ki   0.0%
 100.0%   456Ki TOTAL          7.75Mi 100.0%
```

Here we see one segment mapped `[R E]` (read/execute) and
one segment mapped `[RW ]` (read/write).  A large part of
the binary is not loaded into memory, which we see as
`[Unmapped]`.

Object files and static libraries don't have segments.
However we fake it by grouping sections by their flags.
This gives us a break-down sort of like real segments.

```
 $ ./bloaty bloaty -d segments src/bloaty.o
     VM SIZE                     FILE SIZE
 --------------               --------------
   0.0%       0 [Unmapped]     7.31Mi  67.6%
   0.0%       0 Section []     2.95Mi  27.3%
  85.2%   452Ki LOAD [RX]       452Ki   4.1%
  11.3%  59.8Ki Section [AX]   59.8Ki   0.5%
   0.0%       0 [ELF Headers]  28.3Ki   0.3%
   2.9%  15.4Ki Section [A]    15.4Ki   0.1%
   0.7%  3.46Ki LOAD [RW]      1.88Ki   0.0%
   0.0%      41 Section [AW]       20   0.0%
 100.0%   531Ki TOTAL          10.8Mi 100.0%
```

## Sections

Sections give us a bit more granular look into the binary.
If we want to find the symbol table, the unwind information,
or the debug information, each kind of information lives in
its own section.  Bloaty's default output is sections.

```
$ bloaty -d sections bloaty
      VM SIZE                         FILE SIZE
 --------------                   --------------
   0.0%       0 .debug_info        2.97Mi  38.3%
   0.0%       0 .debug_loc         2.30Mi  29.7%
   0.0%       0 .debug_str         1.03Mi  13.3%
   0.0%       0 .debug_ranges       611Ki   7.7%
  72.8%   332Ki .text               332Ki   4.2%
   0.0%       0 .debug_line         218Ki   2.8%
   0.0%       0 .debug_abbrev      85.4Ki   1.1%
   0.0%       0 .strtab            62.8Ki   0.8%
  13.2%  60.0Ki .rodata            60.0Ki   0.8%
   7.0%  31.8Ki .eh_frame          31.8Ki   0.4%
   0.0%       0 .symtab            27.8Ki   0.3%
   0.0%       0 .debug_aranges     13.5Ki   0.2%
   2.3%  10.5Ki .gcc_except_table  10.5Ki   0.1%
   1.5%  6.77Ki [Other]            5.60Ki   0.1%
   0.9%  4.18Ki .eh_frame_hdr      4.18Ki   0.1%
   0.8%  3.54Ki .dynsym            3.54Ki   0.0%
   0.8%  3.52Ki .dynstr            3.52Ki   0.0%
   0.7%  2.98Ki .rela.plt          2.98Ki   0.0%
   0.1%     568 [ELF Headers]      2.93Ki   0.0%
   0.0%      34 [Unmapped]         2.85Ki   0.0%
   0.0%       4 [None]                  0   0.0%
 100.0%   456Ki TOTAL              7.75Mi 100.0%
```

## Symbols

Symbols come from the symbol table, and represent individual
functions or variables.

```
$ ./bloaty -d symbols bloaty
     VM SIZE                                         FILE SIZE
 --------------                                   --------------
   0.0%       0 [section .debug_info]              10.7Mi  37.1%
   0.0%       0 [section .debug_loc]               5.39Mi  18.6%
   0.0%       0 [section .debug_str]               4.48Mi  15.5%
  64.8%  3.20Mi [5661 Others]                      3.86Mi  13.3%
   0.0%       0 [section .debug_ranges]            1.67Mi   5.8%
   0.0%       0 [section .debug_line]               856Ki   2.9%
  12.9%   648Ki insns                               648Ki   2.2%
   0.0%       0 [section .debug_abbrev]             241Ki   0.8%
   4.3%   217Ki ARMInsts                            217Ki   0.7%
   3.7%   185Ki insn_name_maps                      185Ki   0.6%
   2.3%   117Ki AArch64_printInst                   117Ki   0.4%
   2.3%   117Ki x86DisassemblerTwoByteOpcodes       117Ki   0.4%
   2.0%   101Ki Sparc_printInst                     101Ki   0.3%
   1.5%  74.3Ki PPC_printInst                      74.4Ki   0.3%
   1.1%  54.0Ki x86DisassemblerThreeByte38Opcodes  54.0Ki   0.2%
   1.1%  53.0Ki DecoderTable32                     53.1Ki   0.2%
   1.0%  48.4Ki [section .gcc_except_table]        48.4Ki   0.2%
   0.8%  41.5Ki reg_name_maps                      41.7Ki   0.1%
   0.8%  39.8Ki [section .gnu.hash]                39.8Ki   0.1%
   0.8%  38.7Ki decodeInstruction_4.constprop.128  38.8Ki   0.1%
   0.7%  37.8Ki printInstruction                   37.8Ki   0.1%
 100.0%  4.93Mi TOTAL                              28.9Mi 100.0%
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

```
$ ./bloaty -d inputfiles src/*.o
     VM SIZE                    FILE SIZE
 --------------              --------------
  51.8%  75.2Ki src/bloaty.o  3.05Mi  48.2%
  28.2%  40.9Ki src/dwarf.o   2.04Mi  32.2%
  12.1%  17.5Ki src/elf.o      579Ki   8.9%
   5.5%  7.99Ki src/macho.o    415Ki   6.4%
   2.5%  3.57Ki src/main.o     279Ki   4.3%
 100.0%   145Ki TOTAL         6.34Mi 100.0%
```

## Archive Members

When you are running Bloaty on a `.a` file, the `armembers`
source will let you break it down by `.o` file inside the
archive.

```
./bloaty -d armembers src/libbloaty.a 
     VM SIZE                         FILE SIZE
 --------------                   --------------
  53.1%  75.2Ki bloaty.o           3.05Mi  50.1%
  28.9%  40.9Ki dwarf.o            2.04Mi  33.5%
  12.4%  17.5Ki elf.o               579Ki   9.3%
   5.6%  7.99Ki macho.o             415Ki   6.7%
   0.0%       0 [AR Symbol Table]  27.3Ki   0.4%
   0.0%       0 [AR Headers]          308   0.0%
 100.0%   141Ki TOTAL              6.10Mi 100.0%
```

You are free to use this data source even for non-`.a`
files, but it won't be very useful since it will always just
resolve to the input file (the `.a` file).

## Compile Units

Using debug information, we can tell what compile unit (and
corresponding source file) each bit of the binary came from.
There are a couple different places in DWARF we can look for
this information; currently we mainly use the
`.debug_aranges` section.  It's not perfect and sometimes
you'll see some of the binary show up as `[None]` if it's
not mentioned in aranges (improving this is a TODO).  But it
can tell us a lot.

```
 $ ./bloaty -d compileunits bloaty
     VM SIZE                                                                                   FILE SIZE
 --------------                                                                             --------------
  44.2%  2.18Mi [137 Others]                                                                 10.7Mi  36.9%
   5.4%   271Ki ../third_party/protobuf/src/google/protobuf/descriptor.cc                    3.92Mi  13.5%
   7.1%   360Ki ../third_party/protobuf/src/google/protobuf/descriptor.pb.cc                 2.39Mi   8.2%
   8.3%   416Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c                           1.57Mi   5.4%
   1.7%  87.4Ki ../third_party/protobuf/src/google/protobuf/text_format.cc                   1.00Mi   3.5%
   2.1%   106Ki ../third_party/protobuf/src/google/protobuf/generated_message_reflection.cc   959Ki   3.2%
   0.8%  38.1Ki ../third_party/protobuf/src/google/protobuf/descriptor_database.cc            771Ki   2.6%
   1.5%  73.4Ki ../third_party/protobuf/src/google/protobuf/message.cc                        754Ki   2.5%
   2.5%   126Ki ../src/bloaty.cc                                                              753Ki   2.5%
   0.9%  43.5Ki ../third_party/re2/re2/dfa.cc                                                 648Ki   2.2%
   1.2%  60.5Ki ../third_party/protobuf/src/google/protobuf/extension_set.cc                  610Ki   2.1%
   0.8%  42.0Ki ../third_party/re2/re2/re2.cc                                                 595Ki   2.0%
   0.6%  28.2Ki ../third_party/protobuf/src/google/protobuf/generated_message_util.cc         572Ki   1.9%
   1.1%  56.5Ki ../third_party/protobuf/src/google/protobuf/map_field.cc                      565Ki   1.9%
   0.8%  42.5Ki ../third_party/re2/re2/regexp.cc                                              543Ki   1.8%
   1.8%  91.3Ki ../third_party/capstone/arch/AArch64/AArch64Disassembler.c                    523Ki   1.8%
   1.0%  50.9Ki ../third_party/protobuf/src/google/protobuf/wire_format.cc                    520Ki   1.8%
   1.8%  88.8Ki ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp                490Ki   1.7%
   3.2%   163Ki ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c                     456Ki   1.5%
   6.5%   329Ki ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c                     427Ki   1.4%
   6.7%   337Ki ../third_party/capstone/arch/X86/X86Mapping.c                                 417Ki   1.4%
 100.0%  4.93Mi TOTAL                                                                        28.9Mi 100.0%
```

## Inlines

The DWARF debugging information also contains "line info"
information that understands inlining.  So within a
function, it will know which instructions came from an
inlined function from a header file.  This is the
information the debugger uses to point at a specific source
line as you're tracing through a program.

```
$ ./bloaty -d inlines bloaty 
     VM SIZE                                                    FILE SIZE
 --------------                                              --------------
   2.4%   110Ki [None]                                        7.42Mi  95.6%
  90.3%  4.01Mi /usr/include/c++/4.8/bitsstl_vector.h:414     15.3Ki   0.2%
   5.5%   250Ki [Other]                                        250Ki   3.2%
   0.3%  11.4Ki /usr/include/c++/4.8/bitsbasic_string.h:539   11.4Ki   0.1%
   0.2%  8.81Ki /usr/include/c++/4.8ostream:535               8.81Ki   0.1%
   0.2%  7.59Ki /usr/include/c++/4.8/bitsbasic_ios.h:456      7.59Ki   0.1%
   0.1%  6.20Ki /usr/include/c++/4.8streambuf:466             6.20Ki   0.1%
   0.1%  6.06Ki /usr/include/c++/4.8/bitsbasic_string.h:249   6.06Ki   0.1%
   0.1%  4.24Ki /usr/include/c++/4.8/bitsbasic_string.h:240   4.24Ki   0.1%
   0.1%  3.61Ki /usr/include/c++/4.8/bitsbasic_ios.h:276      3.61Ki   0.0%
   0.1%  3.51Ki /usr/include/c++/4.8/extatomicity.h:81        3.51Ki   0.0%
   0.1%  3.19Ki /usr/include/c++/4.8/bitsbasic_string.h:583   3.19Ki   0.0%
   0.1%  3.06Ki /usr/include/c++/4.8/bitsbasic_string.h:293   3.06Ki   0.0%
   0.1%  2.94Ki /usr/include/c++/4.8/extnew_allocator.h:110   2.94Ki   0.0%
   0.1%  2.89Ki /usr/include/c++/4.8ostream:385               2.89Ki   0.0%
   0.1%  2.87Ki /usr/include/c++/4.8/bitsstl_construct.h:102  2.87Ki   0.0%
   0.1%  2.86Ki /usr/include/c++/4.8/extatomicity.h:84        2.86Ki   0.0%
   0.1%  2.76Ki /usr/include/c++/4.8/extatomicity.h:49        2.76Ki   0.0%
   0.1%  2.70Ki /usr/include/c++/4.8/bitschar_traits.h:271    2.70Ki   0.0%
   0.1%  2.62Ki /usr/include/c++/4.8/bitsbasic_string.h:275   2.62Ki   0.0%
   0.1%  2.58Ki /usr/include/c++/4.8ostream:93                2.58Ki   0.0%
 100.0%  4.45Mi TOTAL                                         7.75Mi 100.0%
```

# Custom Data Sources

Sometimes you want to munge the labels from an existing data
source.  For example, when we use "compileunits" on Bloaty
itself, we see files from all our dependencies mixed
together:

```
$ ./bloaty -d compileunits bloaty
     VM SIZE                                                                                FILE SIZE
 --------------                                                                          --------------
  65.5%  3.21Mi [130 Others]                                                              12.3Mi  37.0%
   4.6%   232Ki third_party/protobuf/src/google/protobuf/descriptor.cc                    3.74Mi  11.2%
   5.6%   281Ki third_party/protobuf/src/google/protobuf/descriptor.pb.cc                 2.34Mi   7.0%
   1.8%  90.4Ki src/bloaty.cc                                                             2.15Mi   6.5%
   6.7%   335Ki third_party/capstone/arch/ARM/ARMDisassembler.c                           1.64Mi   4.9%
   1.3%  63.9Ki src/dwarf.cc                                                              1.32Mi   4.0%
   1.6%  82.2Ki third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp               1.17Mi   3.5%
   1.5%  73.9Ki third_party/protobuf/src/google/protobuf/text_format.cc                    997Ki   2.9%
   1.7%  83.5Ki third_party/protobuf/src/google/protobuf/generated_message_reflection.cc   938Ki   2.7%
   0.6%  31.3Ki third_party/protobuf/src/google/protobuf/descriptor_database.cc            766Ki   2.2%
   1.0%  50.9Ki third_party/protobuf/src/google/protobuf/message.cc                        746Ki   2.2%
   0.7%  36.4Ki third_party/re2/re2/dfa.cc                                                 621Ki   1.8%
   0.8%  42.3Ki third_party/re2/re2/re2.cc                                                 618Ki   1.8%
   1.0%  48.3Ki third_party/protobuf/src/google/protobuf/extension_set.cc                  608Ki   1.8%
   0.9%  46.4Ki third_party/protobuf/src/google/protobuf/map_field.cc                      545Ki   1.6%
   0.7%  36.1Ki third_party/re2/re2/regexp.cc                                              538Ki   1.6%
   1.7%  86.9Ki third_party/capstone/arch/AArch64/AArch64Disassembler.c                    517Ki   1.5%
   0.8%  41.8Ki third_party/protobuf/src/google/protobuf/wire_format.cc                    513Ki   1.5%
   0.5%  25.4Ki third_party/protobuf/src/google/protobuf/generated_message_util.cc         511Ki   1.5%
   0.1%  4.33Ki src/main.cc                                                                483Ki   1.4%
   0.8%  41.3Ki src/bloaty.pb.cc                                                           465Ki   1.4%
 100.0%  4.91Mi TOTAL                                                                     33.4Mi 100.0%
```

If we want to bucket all of these by which library they came
from, we can write a custom data source.  It specifies the
base data source and a set of regexes to apply to it.  The
regexes are tried in order, and the first matching regex
will cause the entire label to be rewritten to the
replacement text.  Regexes follow [RE2
syntax](https://github.com/google/re2/wiki/Syntax) and the
replacement can refer to capture groups.

```
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

```
$ ./bloaty -c config.bloaty -d bloaty_package bloaty
     VM SIZE                                   FILE SIZE
 --------------                             --------------
  21.7%  1.06Mi third_party/protobuf         14.2Mi  42.6%
  42.4%  2.08Mi third_party/capstone         6.88Mi  20.6%
   5.1%   256Ki src                          5.30Mi  15.9%
   5.5%   274Ki third_party/re2              3.97Mi  11.9%
   1.6%  82.2Ki third_party/demumble         1.17Mi   3.5%
   0.8%  38.0Ki third_party/abseil            526Ki   1.5%
   7.8%   390Ki [section .rodata]             390Ki   1.1%
   6.4%   320Ki [section .rela.dyn]           320Ki   0.9%
   4.6%   231Ki [section .eh_frame]           231Ki   0.7%
   0.0%       0 [section .debug_str]         82.7Ki   0.2%
   0.9%  44.8Ki [section .gcc_except_table]  44.8Ki   0.1%
   0.0%       0 [section .strtab]            40.5Ki   0.1%
   0.6%  31.5Ki [23 Others]                  38.8Ki   0.1%
   0.8%  38.2Ki [section .gnu.hash]          38.2Ki   0.1%
   0.7%  36.4Ki [section .eh_frame_hdr]      36.4Ki   0.1%
   0.0%       0 [section .debug_aranges]     27.6Ki   0.1%
   0.5%  26.4Ki [section .dynstr]            26.4Ki   0.1%
   0.0%       0 [section .symtab]            24.9Ki   0.1%
   0.4%  20.0Ki [section .data.rel.ro]       20.0Ki   0.1%
   0.0%       0 [section .debug_loc]         19.6Ki   0.1%
   0.3%  15.4Ki [section .dynsym]            15.4Ki   0.0%
 100.0%  4.91Mi TOTAL                        33.4Mi 100.0%
```

We can get an even richer report by combining the
`bloaty_package` source with the original `compileunits`
source:

```
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits bloaty
     VM SIZE                                                                                    FILE SIZE
 --------------                                                                              --------------
  21.7%  1.06Mi third_party/protobuf                                                          14.2Mi  42.6%
      21.3%   232Ki third_party/protobuf/src/google/protobuf/descriptor.cc                        3.74Mi  26.3%
      25.9%   281Ki third_party/protobuf/src/google/protobuf/descriptor.pb.cc                     2.34Mi  16.5%
       6.8%  73.9Ki third_party/protobuf/src/google/protobuf/text_format.cc                        997Ki   6.9%
       7.7%  83.5Ki third_party/protobuf/src/google/protobuf/generated_message_reflection.cc       938Ki   6.4%
       2.9%  31.3Ki third_party/protobuf/src/google/protobuf/descriptor_database.cc                766Ki   5.3%
       4.7%  50.9Ki third_party/protobuf/src/google/protobuf/message.cc                            746Ki   5.1%
       4.4%  47.8Ki [14 Others]                                                                    686Ki   4.7%
       4.4%  48.3Ki third_party/protobuf/src/google/protobuf/extension_set.cc                      608Ki   4.2%
       4.3%  46.4Ki third_party/protobuf/src/google/protobuf/map_field.cc                          545Ki   3.7%
       3.8%  41.8Ki third_party/protobuf/src/google/protobuf/wire_format.cc                        513Ki   3.5%
       2.3%  25.4Ki third_party/protobuf/src/google/protobuf/generated_message_util.cc             511Ki   3.5%
       1.2%  12.9Ki third_party/protobuf/src/google/protobuf/dynamic_message.cc                    316Ki   2.2%
       1.6%  17.4Ki third_party/protobuf/src/google/protobuf/extension_set_heavy.cc                288Ki   2.0%
       2.3%  25.3Ki third_party/protobuf/src/google/protobuf/stubs/strutil.cc                      263Ki   1.8%
       1.2%  12.8Ki third_party/protobuf/src/google/protobuf/stubs/common.cc                       218Ki   1.5%
       1.5%  16.8Ki third_party/protobuf/src/google/protobuf/wire_format_lite.cc                   194Ki   1.3%
       0.8%  9.22Ki third_party/protobuf/src/google/protobuf/reflection_ops.cc                     183Ki   1.3%
       1.2%  12.9Ki third_party/protobuf/src/google/protobuf/io/tokenizer.cc                       162Ki   1.1%
       0.6%  6.90Ki third_party/protobuf/src/google/protobuf/unknown_field_set.cc                  150Ki   1.0%
       0.3%  3.00Ki third_party/protobuf/src/google/protobuf/any.cc                                117Ki   0.8%
       0.8%  9.15Ki third_party/protobuf/src/google/protobuf/message_lite.cc                       114Ki   0.8%
  42.4%  2.08Mi third_party/capstone                                                          6.88Mi  20.6%
      15.8%   335Ki third_party/capstone/arch/ARM/ARMDisassembler.c                               1.64Mi  23.8%
       4.7%   100Ki [22 Others]                                                                    579Ki   8.2%
       4.1%  86.9Ki third_party/capstone/arch/AArch64/AArch64Disassembler.c                        517Ki   7.3%
      15.4%   328Ki third_party/capstone/arch/X86/X86DisassemblerDecoder.c                         427Ki   6.1%
       6.5%   139Ki third_party/capstone/arch/AArch64/AArch64InstPrinter.c                         423Ki   6.0%
       2.6%  55.6Ki third_party/capstone/arch/Mips/MipsDisassembler.c                              408Ki   5.8%
      14.1%   299Ki third_party/capstone/arch/X86/X86Mapping.c                                     380Ki   5.4%
       3.5%  73.9Ki third_party/capstone/arch/ARM/ARMInstPrinter.c                                 293Ki   4.2%
       4.5%  96.6Ki third_party/capstone/arch/Sparc/SparcInstPrinter.c                             287Ki   4.1%
       0.7%  14.4Ki third_party/capstone/arch/X86/X86ATTInstPrinter.c                              276Ki   3.9%
       3.5%  74.8Ki third_party/capstone/arch/PowerPC/PPCInstPrinter.c                             273Ki   3.9%
       1.3%  27.8Ki third_party/capstone/arch/PowerPC/PPCDisassembler.c                            241Ki   3.4%
       1.2%  25.4Ki third_party/capstone/arch/SystemZ/SystemZDisassembler.c                        223Ki   3.2%
       0.6%  13.3Ki third_party/capstone/arch/X86/X86IntelInstPrinter.c                            187Ki   2.7%
       5.6%   118Ki third_party/capstone/arch/AArch64/AArch64Mapping.c                             154Ki   2.2%
       5.2%   111Ki third_party/capstone/arch/ARM/ARMMapping.c                                     148Ki   2.1%
       1.0%  20.3Ki third_party/capstone/arch/X86/X86Disassembler.c                                130Ki   1.9%
       3.8%  81.5Ki third_party/capstone/arch/Mips/MipsMapping.c                                   120Ki   1.7%
       0.5%  11.3Ki third_party/capstone/arch/XCore/XCoreDisassembler.c                            103Ki   1.5%
       3.3%  71.0Ki third_party/capstone/arch/PowerPC/PPCMapping.c                                 100Ki   1.4%
       2.1%  44.1Ki third_party/capstone/arch/SystemZ/SystemZMapping.c                            91.5Ki   1.3%
   5.1%   256Ki src                                                                           5.30Mi  15.9%
      35.3%  90.4Ki src/bloaty.cc                                                                 2.15Mi  40.7%
      24.9%  63.9Ki src/dwarf.cc                                                                  1.32Mi  25.0%
       1.7%  4.33Ki src/main.cc                                                                    483Ki   8.9%
      16.1%  41.3Ki src/bloaty.pb.cc                                                               465Ki   8.6%
      10.3%  26.3Ki src/elf.cc                                                                     397Ki   7.3%
       2.3%  5.81Ki src/disassemble.cc                                                             204Ki   3.8%
       3.2%  8.25Ki src/macho.cc                                                                   191Ki   3.5%
       6.3%  16.2Ki src/demangle.cc                                                                119Ki   2.2%
   5.5%   274Ki third_party/re2                                                               3.97Mi  11.9%
      13.3%  36.4Ki third_party/re2/re2/dfa.cc                                                     621Ki  15.3%
      15.4%  42.3Ki third_party/re2/re2/re2.cc                                                     618Ki  15.2%
      13.2%  36.1Ki third_party/re2/re2/regexp.cc                                                  538Ki  13.2%
       9.4%  25.7Ki third_party/re2/re2/compile.cc                                                 363Ki   9.0%
       7.0%  19.3Ki third_party/re2/re2/prog.cc                                                    341Ki   8.4%
      16.9%  46.2Ki third_party/re2/re2/parse.cc                                                   336Ki   8.3%
       8.6%  23.5Ki third_party/re2/re2/simplify.cc                                                298Ki   7.3%
       6.4%  17.5Ki third_party/re2/re2/nfa.cc                                                     267Ki   6.6%
       2.4%  6.52Ki third_party/re2/re2/tostring.cc                                                176Ki   4.3%
       2.5%  6.92Ki third_party/re2/re2/onepass.cc                                                 148Ki   3.7%
       3.2%  8.65Ki third_party/re2/re2/bitstate.cc                                                140Ki   3.4%
       0.0%       0 third_party/re2/re2/unicode_groups.cc                                         60.3Ki   1.5%
       0.0%       0 third_party/re2/re2/perl_groups.cc                                            41.1Ki   1.0%
       0.7%  1.94Ki third_party/re2/re2/stringpiece.cc                                            41.0Ki   1.0%
       0.8%  2.21Ki third_party/re2/util/strutil.cc                                               39.8Ki   1.0%
       0.0%       0 third_party/re2/re2/unicode_casefold.cc                                       26.3Ki   0.6%
       0.4%    1020 third_party/re2/util/rune.cc                                                  4.77Ki   0.1%
   1.6%  82.2Ki third_party/demumble                                                          1.17Mi   3.5%
     100.0%  82.2Ki third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp                   1.17Mi 100.0%
   0.8%  38.0Ki third_party/abseil                                                             526Ki   1.5%
      26.5%  10.1Ki third_party/abseil-cpp/absl/strings/escaping.cc                                134Ki  25.6%
      24.6%  9.35Ki third_party/abseil-cpp/absl/strings/numbers.cc                                80.8Ki  15.4%
      11.5%  4.35Ki third_party/abseil-cpp/absl/strings/str_cat.cc                                58.9Ki  11.2%
      10.1%  3.84Ki third_party/abseil-cpp/absl/strings/string_view.cc                            44.2Ki   8.4%
       4.1%  1.55Ki third_party/abseil-cpp/absl/strings/str_split.cc                              41.6Ki   7.9%
       4.0%  1.51Ki third_party/abseil-cpp/absl/strings/ascii.cc                                  40.0Ki   7.6%
       3.4%  1.27Ki third_party/abseil-cpp/absl/strings/substitute.cc                             38.6Ki   7.3%
       9.3%  3.55Ki third_party/abseil-cpp/absl/base/internal/throw_delegate.cc                   38.2Ki   7.3%
       3.5%  1.33Ki third_party/abseil-cpp/absl/base/internal/raw_logging.cc                      31.9Ki   6.1%
       2.5%     985 third_party/abseil-cpp/absl/strings/internal/memutil.cc                       15.1Ki   2.9%
       0.6%     230 third_party/abseil-cpp/absl/strings/internal/utf8.cc                          1.85Ki   0.4%
   7.8%   390Ki [section .rodata]                                                              390Ki   1.1%
   6.4%   320Ki [section .rela.dyn]                                                            320Ki   0.9%
   4.6%   231Ki [section .eh_frame]                                                            231Ki   0.7%
   0.0%       0 [section .debug_str]                                                          82.7Ki   0.2%
   0.9%  44.8Ki [section .gcc_except_table]                                                   44.8Ki   0.1%
   0.0%       0 [section .strtab]                                                             40.5Ki   0.1%
   0.6%  31.5Ki [23 Others]                                                                   38.8Ki   0.1%
   0.8%  38.2Ki [section .gnu.hash]                                                           38.2Ki   0.1%
   0.7%  36.4Ki [section .eh_frame_hdr]                                                       36.4Ki   0.1%
   0.0%       0 [section .debug_aranges]                                                      27.6Ki   0.1%
   0.5%  26.4Ki [section .dynstr]                                                             26.4Ki   0.1%
   0.0%       0 [section .symtab]                                                             24.9Ki   0.1%
   0.4%  20.0Ki [section .data.rel.ro]                                                        20.0Ki   0.1%
   0.0%       0 [section .debug_loc]                                                          19.6Ki   0.1%
   0.3%  15.4Ki [section .dynsym]                                                             15.4Ki   0.0%
 100.0%  4.91Mi TOTAL                                                                         33.4Mi 100.0%
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
