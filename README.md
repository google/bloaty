
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
  34.6%  5.75Mi   0.0%       0 .debug_info
  14.3%  2.37Mi   0.0%       0 .debug_loc
  13.2%  2.20Mi  38.1%  2.20Mi .rodata
  11.1%  1.84Mi   0.0%       0 .debug_str
   7.4%  1.23Mi  21.4%  1.23Mi .text
   0.0%       0  17.4%  1.00Mi .bss
   5.8%   993Ki   0.0%       0 .debug_ranges
   3.0%   504Ki   0.0%       0 .debug_line
   2.6%   442Ki   7.5%   442Ki .data
   1.8%   304Ki   5.2%   304Ki .rela.dyn
   1.7%   297Ki   5.0%   297Ki .data.rel.ro
   1.1%   187Ki   0.0%       0 .strtab
   0.7%   123Ki   0.0%       0 .debug_abbrev
   0.7%   114Ki   0.0%       0 .symtab
   0.6%   109Ki   1.9%   109Ki .eh_frame
   0.6%  97.7Ki   1.7%  97.7Ki .dynstr
   0.3%  43.4Ki   0.7%  43.4Ki .dynsym
   0.2%  34.2Ki   0.5%  27.9Ki [22 Others]
   0.1%  17.9Ki   0.3%  17.9Ki .eh_frame_hdr
   0.1%  17.8Ki   0.3%  17.8Ki .gcc_except_table
   0.1%  13.0Ki   0.0%       0 .debug_aranges
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  36.3%  6.03Mi  31.8%  1.83Mi [119 Others]
   9.5%  1.59Mi   7.1%   416Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.8%   310Ki  18.3%  1.05Mi ../third_party/capstone/arch/M68K/M68KDisassembler.c
   5.7%   974Ki  14.6%   863Ki ../third_party/capstone/arch/X86/X86Mapping.c
   4.4%   756Ki   2.2%   127Ki ../src/bloaty.cc
   3.5%   590Ki   0.7%  41.6Ki ../third_party/re2/re2/dfa.cc
   3.5%   589Ki   0.7%  41.7Ki ../third_party/re2/re2/re2.cc
   3.2%   543Ki   0.7%  42.5Ki ../third_party/re2/re2/regexp.cc
   3.2%   538Ki   7.3%   431Ki ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   3.1%   529Ki   1.6%  91.5Ki ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   2.9%   490Ki   1.5%  88.8Ki ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.8%   484Ki   2.9%   170Ki ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
   2.8%   483Ki   1.8%   104Ki ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   2.6%   442Ki   1.1%  67.4Ki ../third_party/capstone/arch/Mips/MipsDisassembler.c
   2.4%   402Ki   0.6%  38.1Ki ../third_party/re2/re2/parse.cc
   2.2%   374Ki   0.9%  53.9Ki ../src/dwarf.cc
   2.1%   362Ki   0.5%  28.9Ki ../third_party/re2/re2/compile.cc
   2.1%   357Ki   0.4%  23.1Ki ../third_party/re2/re2/prog.cc
   2.0%   334Ki   1.7%   100Ki ../third_party/capstone/arch/ARM/ARMInstPrinter.c
   2.0%   334Ki   1.9%   110Ki ../third_party/capstone/arch/PowerPC/PPCInstPrinter.c
   1.9%   323Ki   1.7%   101Ki ../third_party/capstone/arch/X86/X86IntelInstPrinter.c
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
```


Run Bloaty with `--help` to see a list of available options:

```
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
  71.4%  11.9Mi   0.0%       0 [Unmapped]
      48.5%  5.75Mi   NAN%       0 .debug_info
      20.0%  2.37Mi   NAN%       0 .debug_loc
      15.5%  1.84Mi   NAN%       0 .debug_str
       8.2%   993Ki   NAN%       0 .debug_ranges
       4.1%   504Ki   NAN%       0 .debug_line
       1.5%   187Ki   NAN%       0 .strtab
       1.0%   123Ki   NAN%       0 .debug_abbrev
       0.9%   114Ki   NAN%       0 .symtab
       0.1%  13.0Ki   NAN%       0 .debug_aranges
       0.0%  3.46Ki   NAN%       0 [Unmapped]
       0.0%     383   NAN%       0 .shstrtab
       0.0%      29   NAN%       0 .comment
  24.3%  4.03Mi  70.0%  4.03Mi LOAD #2 [RX]
      54.5%  2.20Mi  54.5%  2.20Mi .rodata
      30.6%  1.23Mi  30.6%  1.23Mi .text
       7.4%   304Ki   7.4%   304Ki .rela.dyn
       2.7%   109Ki   2.7%   109Ki .eh_frame
       2.4%  97.7Ki   2.4%  97.7Ki .dynstr
       1.1%  43.4Ki   1.1%  43.4Ki .dynsym
       0.4%  17.9Ki   0.4%  17.9Ki .eh_frame_hdr
       0.4%  17.8Ki   0.4%  17.8Ki .gcc_except_table
       0.3%  12.3Ki   0.3%  12.3Ki .gnu.hash
       0.1%  4.99Ki   0.1%  4.99Ki .rela.plt
       0.1%  3.62Ki   0.1%  3.62Ki .gnu.version
       0.1%  3.34Ki   0.1%  3.34Ki .plt
       0.0%     568   0.0%     568 [ELF Headers]
       0.0%     368   0.0%     368 .gnu.version_r
       0.0%      96   0.0%      96 .plt.got
       0.0%      39   0.0%      39 [LOAD #2 [RX]]
       0.0%      36   0.0%      36 .note.gnu.build-id
       0.0%      32   0.0%      32 .note.ABI-tag
       0.0%      28   0.0%      28 .interp
       0.0%      23   0.0%      23 .init
       0.0%       9   0.0%       9 [1 Others]
   4.4%   742Ki  30.0%  1.73Mi LOAD #3 [RW]
       0.0%       0  58.0%  1.00Mi .bss
      59.6%   442Ki  25.0%   442Ki .data
      40.1%   297Ki  16.8%   297Ki .data.rel.ro
       0.2%  1.69Ki   0.1%  1.69Ki .got.plt
       0.1%     560   0.0%     560 .dynamic
       0.0%     200   0.0%     200 .got
       0.0%      72   0.0%      72 .init_array
       0.0%       8   0.0%       8 .fini_array
       0.0%       8   0.0%       8 [LOAD #3 [RW]]
   0.0%  2.44Ki   0.0%       0 [ELF Headers]
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  71.4%  11.9Mi   0.0%       0 [Unmapped]
  24.3%  4.03Mi  70.0%  4.03Mi LOAD #2 [RX]
   4.4%   742Ki  30.0%  1.73Mi LOAD #3 [RW]
   0.0%  2.44Ki   0.0%       0 [ELF Headers]
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  90.6%  1.19Mi   0.0%       0 Section []
   6.0%  80.9Ki  78.9%  80.9Ki Section [AX]
   1.7%  22.8Ki   0.0%       0 [ELF Headers]
   1.6%  20.9Ki  20.4%  20.9Ki Section [A]
   0.1%     812   0.0%       0 [Unmapped]
   0.0%     656   0.7%     725 Section [AW]
 100.0%  1.31Mi 100.0%   102Ki TOTAL
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
  34.6%  5.75Mi   0.0%       0 .debug_info
  14.3%  2.37Mi   0.0%       0 .debug_loc
  13.2%  2.20Mi  38.1%  2.20Mi .rodata
  11.1%  1.84Mi   0.0%       0 .debug_str
   7.4%  1.23Mi  21.4%  1.23Mi .text
   0.0%       0  17.4%  1.00Mi .bss
   5.8%   993Ki   0.0%       0 .debug_ranges
   3.0%   504Ki   0.0%       0 .debug_line
   2.6%   442Ki   7.5%   442Ki .data
   1.8%   304Ki   5.2%   304Ki .rela.dyn
   1.7%   297Ki   5.0%   297Ki .data.rel.ro
   1.1%   187Ki   0.0%       0 .strtab
   0.7%   123Ki   0.0%       0 .debug_abbrev
   0.7%   114Ki   0.0%       0 .symtab
   0.6%   109Ki   1.9%   109Ki .eh_frame
   0.6%  97.7Ki   1.7%  97.7Ki .dynstr
   0.3%  43.4Ki   0.7%  43.4Ki .dynsym
   0.2%  34.2Ki   0.5%  27.9Ki [22 Others]
   0.1%  17.9Ki   0.3%  17.9Ki .eh_frame_hdr
   0.1%  17.8Ki   0.3%  17.8Ki .gcc_except_table
   0.1%  13.0Ki   0.0%       0 .debug_aranges
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
```

## Symbols

Symbols come from the symbol table, and represent individual
functions or variables.

```cmdoutput
$ ./bloaty -d symbols bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  34.6%  5.75Mi   0.0%       0 [section .debug_info]
  14.9%  2.48Mi  37.7%  2.17Mi [3725 Others]
  14.3%  2.37Mi   0.0%       0 [section .debug_loc]
  11.1%  1.84Mi   0.0%       0 [section .debug_str]
   7.9%  1.31Mi  22.7%  1.31Mi insns
   0.0%      44  17.4%  1024Ki g_instruction_table
   5.8%   993Ki   0.0%       0 [section .debug_ranges]
   3.0%   504Ki   0.0%       0 [section .debug_line]
   1.3%   218Ki   3.7%   218Ki ARMInsts
   1.2%   209Ki   3.5%   208Ki insn_name_maps
   1.0%   175Ki   3.0%   175Ki insn_ops
   0.8%   140Ki   2.4%   140Ki x86DisassemblerTwoByteOpcodes
   0.7%   123Ki   0.0%       0 [section .debug_abbrev]
   0.7%   119Ki   2.0%   118Ki AArch64_printInst
   0.6%   101Ki   1.7%   101Ki Sparc_printInst
   0.5%  79.9Ki   1.4%  79.8Ki PPC_printInst
   0.4%  74.0Ki   1.3%  74.0Ki x86DisassemblerThreeByte38Opcodes
   0.3%  55.9Ki   0.9%  55.8Ki DecoderTable32
   0.3%  54.0Ki   0.9%  54.0Ki x86DisassemblerThreeByte3AOpcodes
   0.3%  46.8Ki   0.8%  46.6Ki reg_name_maps
   0.2%  40.0Ki   0.7%  39.9Ki printInstruction
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  37.6%  1.31Mi  34.1%   102Ki CMakeFiles/libbloaty.dir/src/bloaty.cc.o
  18.9%   672Ki  14.2%  42.7Ki CMakeFiles/libbloaty.dir/src/dwarf.cc.o
   9.7%   344Ki  12.7%  38.2Ki CMakeFiles/libbloaty.dir/src/elf.cc.o
   9.3%   331Ki  12.8%  38.5Ki CMakeFiles/libbloaty.dir/src/bloaty.pb.cc.o
   8.1%   289Ki   8.9%  26.8Ki CMakeFiles/libbloaty.dir/src/macho.cc.o
   5.8%   206Ki   4.6%  13.9Ki CMakeFiles/libbloaty.dir/src/webassembly.cc.o
   4.2%   149Ki   6.2%  18.7Ki CMakeFiles/libbloaty.dir/src/demangle.cc.o
   3.8%   137Ki   4.0%  11.9Ki CMakeFiles/libbloaty.dir/src/range_map.cc.o
   2.6%  93.5Ki   2.4%  7.06Ki CMakeFiles/libbloaty.dir/src/disassemble.cc.o
 100.0%  3.48Mi 100.0%   300Ki TOTAL
```

## Archive Members

When you are running Bloaty on a `.a` file, the `armembers`
source will let you break it down by `.o` file inside the
archive.

```cmdoutput
$ ./bloaty -d armembers liblibbloaty.a
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  25.9%  1.31Mi  21.6%   102Ki bloaty.cc.o
  19.7%  1019Ki  19.2%  90.8Ki cxa_demangle.cpp.o
  13.0%   672Ki   9.0%  42.7Ki dwarf.cc.o
   6.6%   344Ki   8.1%  38.2Ki elf.cc.o
   6.4%   331Ki   8.1%  38.5Ki bloaty.pb.cc.o
   5.6%   289Ki   5.7%  26.8Ki macho.cc.o
   4.0%   206Ki   2.9%  13.9Ki webassembly.cc.o
   2.9%   149Ki   3.9%  18.7Ki demangle.cc.o
   2.6%   137Ki   2.5%  11.9Ki range_map.cc.o
   2.1%   110Ki   3.7%  17.7Ki charconv_bigint.cc.o
   1.8%  93.5Ki   1.5%  7.06Ki disassemble.cc.o
   1.7%  88.1Ki   2.9%  13.6Ki escaping.cc.o
   1.4%  72.6Ki   2.1%  10.0Ki [8 Others]
   1.3%  66.7Ki   2.4%  11.5Ki numbers.cc.o
   1.2%  61.8Ki   0.0%       0 [AR Symbol Table]
   1.0%  52.9Ki   2.9%  13.8Ki charconv.cc.o
   0.8%  40.7Ki   1.2%  5.58Ki str_cat.cc.o
   0.6%  29.4Ki   0.8%  3.66Ki string_view.cc.o
   0.5%  26.2Ki   0.5%  2.25Ki ascii.cc.o
   0.5%  24.8Ki   0.6%  3.01Ki throw_delegate.cc.o
   0.5%  24.2Ki   0.3%  1.58Ki str_split.cc.o
 100.0%  5.06Mi 100.0%   473Ki TOTAL
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
  36.3%  6.03Mi  31.8%  1.83Mi [119 Others]
   9.5%  1.59Mi   7.1%   416Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.8%   310Ki  18.3%  1.05Mi ../third_party/capstone/arch/M68K/M68KDisassembler.c
   5.7%   974Ki  14.6%   863Ki ../third_party/capstone/arch/X86/X86Mapping.c
   4.4%   756Ki   2.2%   127Ki ../src/bloaty.cc
   3.5%   590Ki   0.7%  41.6Ki ../third_party/re2/re2/dfa.cc
   3.5%   589Ki   0.7%  41.7Ki ../third_party/re2/re2/re2.cc
   3.2%   543Ki   0.7%  42.5Ki ../third_party/re2/re2/regexp.cc
   3.2%   538Ki   7.3%   431Ki ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   3.1%   529Ki   1.6%  91.5Ki ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   2.9%   490Ki   1.5%  88.8Ki ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.8%   484Ki   2.9%   170Ki ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
   2.8%   483Ki   1.8%   104Ki ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   2.6%   442Ki   1.1%  67.4Ki ../third_party/capstone/arch/Mips/MipsDisassembler.c
   2.4%   402Ki   0.6%  38.1Ki ../third_party/re2/re2/parse.cc
   2.2%   374Ki   0.9%  53.9Ki ../src/dwarf.cc
   2.1%   362Ki   0.5%  28.9Ki ../third_party/re2/re2/compile.cc
   2.1%   357Ki   0.4%  23.1Ki ../third_party/re2/re2/prog.cc
   2.0%   334Ki   1.7%   100Ki ../third_party/capstone/arch/ARM/ARMInstPrinter.c
   2.0%   334Ki   1.9%   110Ki ../third_party/capstone/arch/PowerPC/PPCInstPrinter.c
   1.9%   323Ki   1.7%   101Ki ../third_party/capstone/arch/X86/X86IntelInstPrinter.c
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  34.6%  5.75Mi   0.0%       0 [section .debug_info]
  14.3%  2.37Mi   0.0%       0 [section .debug_loc]
  13.2%  2.20Mi  38.1%  2.20Mi [section .rodata]
  11.1%  1.84Mi   0.0%       0 [section .debug_str]
   7.5%  1.24Mi  21.2%  1.22Mi [34613 Others]
   0.0%       0  17.4%  1.00Mi [section .bss]
   5.8%   993Ki   0.0%       0 [section .debug_ranges]
   3.0%   504Ki   0.0%       0 [section .debug_line]
   2.6%   442Ki   7.5%   442Ki [section .data]
   1.8%   304Ki   5.2%   304Ki [section .rela.dyn]
   1.7%   297Ki   5.0%   297Ki [section .data.rel.ro]
   1.1%   187Ki   0.0%       0 [section .strtab]
   0.7%   123Ki   0.0%       0 [section .debug_abbrev]
   0.7%   114Ki   0.0%       0 [section .symtab]
   0.6%   109Ki   1.9%   109Ki [section .eh_frame]
   0.6%  97.7Ki   1.7%  97.7Ki [section .dynstr]
   0.3%  43.4Ki   0.7%  43.4Ki [section .dynsym]
   0.1%  21.6Ki   0.4%  21.6Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c:115
   0.1%  18.5Ki   0.3%  18.5Ki /usr/include/c++/7/bits/basic_string.h:220
   0.1%  17.9Ki   0.3%  17.9Ki [section .eh_frame_hdr]
   0.1%  17.8Ki   0.3%  17.8Ki [section .gcc_except_table]
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  36.3%  6.03Mi  31.8%  1.83Mi [119 Others]
   9.5%  1.59Mi   7.1%   416Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.8%   310Ki  18.3%  1.05Mi ../third_party/capstone/arch/M68K/M68KDisassembler.c
   5.7%   974Ki  14.6%   863Ki ../third_party/capstone/arch/X86/X86Mapping.c
   4.4%   756Ki   2.2%   127Ki ../src/bloaty.cc
   3.5%   590Ki   0.7%  41.6Ki ../third_party/re2/re2/dfa.cc
   3.5%   589Ki   0.7%  41.7Ki ../third_party/re2/re2/re2.cc
   3.2%   543Ki   0.7%  42.5Ki ../third_party/re2/re2/regexp.cc
   3.2%   538Ki   7.3%   431Ki ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   3.1%   529Ki   1.6%  91.5Ki ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   2.9%   490Ki   1.5%  88.8Ki ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.8%   484Ki   2.9%   170Ki ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
   2.8%   483Ki   1.8%   104Ki ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   2.6%   442Ki   1.1%  67.4Ki ../third_party/capstone/arch/Mips/MipsDisassembler.c
   2.4%   402Ki   0.6%  38.1Ki ../third_party/re2/re2/parse.cc
   2.2%   374Ki   0.9%  53.9Ki ../src/dwarf.cc
   2.1%   362Ki   0.5%  28.9Ki ../third_party/re2/re2/compile.cc
   2.1%   357Ki   0.4%  23.1Ki ../third_party/re2/re2/prog.cc
   2.0%   334Ki   1.7%   100Ki ../third_party/capstone/arch/ARM/ARMInstPrinter.c
   2.0%   334Ki   1.9%   110Ki ../third_party/capstone/arch/PowerPC/PPCInstPrinter.c
   1.9%   323Ki   1.7%   101Ki ../third_party/capstone/arch/X86/X86IntelInstPrinter.c
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
  58.5%  9.73Mi  83.3%  4.80Mi third_party/capstone
  24.2%  4.02Mi   5.4%   318Ki third_party/re2
  11.6%  1.92Mi   6.4%   377Ki src
   2.9%   490Ki   1.5%  88.8Ki third_party/demumble
   1.5%   254Ki   1.7%  98.4Ki third_party/abseil
   0.3%  52.7Ki   0.0%       0 [section .debug_str]
   0.2%  36.3Ki   0.6%  36.3Ki [section .rodata]
   0.1%  19.6Ki   0.0%       0 [section .strtab]
   0.1%  17.8Ki   0.3%  17.8Ki [section .gcc_except_table]
   0.1%  16.4Ki   0.0%       0 [section .debug_loc]
   0.1%  15.1Ki   0.0%       0 [section .symtab]
   0.1%  13.0Ki   0.0%       0 [section .debug_aranges]
   0.1%  12.3Ki   0.2%  12.3Ki [section .gnu.hash]
   0.1%  11.3Ki   0.1%  8.02Ki [26 Others]
   0.1%  9.80Ki   0.2%  9.80Ki [section .dynstr]
   0.0%  6.56Ki   0.1%  6.56Ki [section .data]
   0.0%  6.12Ki   0.1%  6.12Ki [section .dynsym]
   0.0%  5.08Ki   0.0%       0 [section .debug_ranges]
   0.0%  4.04Ki   0.1%  4.04Ki [section .text]
   0.0%  3.62Ki   0.1%  3.62Ki [section .gnu.version]
   0.0%  3.46Ki   0.0%       0 [Unmapped]
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
```

We can get an even richer report by combining the
`bloaty_package` source with the original `compileunits`
source:

```cmdoutput
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  58.5%  9.73Mi  83.3%  4.80Mi third_party/capstone
      16.3%  1.59Mi   8.5%   416Ki ../third_party/capstone/arch/ARM/ARMDisassembler.c
      14.8%  1.44Mi   6.6%   325Ki [36 Others]
       3.1%   310Ki  22.0%  1.05Mi ../third_party/capstone/arch/M68K/M68KDisassembler.c
       9.8%   974Ki  17.6%   863Ki ../third_party/capstone/arch/X86/X86Mapping.c
       5.4%   538Ki   8.8%   431Ki ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
       5.3%   529Ki   1.9%  91.5Ki ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
       4.9%   484Ki   3.5%   170Ki ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
       4.9%   483Ki   2.1%   104Ki ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
       4.4%   442Ki   1.4%  67.4Ki ../third_party/capstone/arch/Mips/MipsDisassembler.c
       3.4%   334Ki   2.0%   100Ki ../third_party/capstone/arch/ARM/ARMInstPrinter.c
       3.4%   334Ki   2.2%   110Ki ../third_party/capstone/arch/PowerPC/PPCInstPrinter.c
       3.2%   323Ki   2.1%   101Ki ../third_party/capstone/arch/X86/X86IntelInstPrinter.c
       3.1%   309Ki   2.3%   110Ki ../third_party/capstone/arch/Sparc/SparcInstPrinter.c
       3.0%   295Ki   0.7%  35.9Ki ../third_party/capstone/arch/PowerPC/PPCDisassembler.c
       2.8%   281Ki   4.8%   237Ki ../third_party/capstone/arch/AArch64/AArch64Mapping.c
       2.6%   260Ki   4.3%   213Ki ../third_party/capstone/arch/ARM/ARMMapping.c
       2.4%   237Ki   0.6%  27.2Ki ../third_party/capstone/arch/SystemZ/SystemZDisassembler.c
       2.0%   200Ki   3.3%   160Ki ../third_party/capstone/arch/PowerPC/PPCMapping.c
       2.0%   199Ki   3.1%   152Ki ../third_party/capstone/arch/Mips/MipsMapping.c
       1.8%   181Ki   0.6%  29.8Ki ../third_party/capstone/arch/X86/X86Disassembler.c
       1.4%   137Ki   1.7%  83.3Ki ../third_party/capstone/arch/SystemZ/SystemZMapping.c
  24.2%  4.02Mi   5.4%   318Ki third_party/re2
      14.4%   590Ki  13.0%  41.6Ki ../third_party/re2/re2/dfa.cc
      14.3%   589Ki  13.1%  41.7Ki ../third_party/re2/re2/re2.cc
      13.2%   543Ki  13.4%  42.5Ki ../third_party/re2/re2/regexp.cc
       9.8%   402Ki  12.0%  38.1Ki ../third_party/re2/re2/parse.cc
       8.8%   362Ki   9.1%  28.9Ki ../third_party/re2/re2/compile.cc
       8.7%   357Ki   7.3%  23.1Ki ../third_party/re2/re2/prog.cc
       7.1%   292Ki   8.2%  26.0Ki ../third_party/re2/re2/simplify.cc
       6.5%   267Ki   5.7%  18.3Ki ../third_party/re2/re2/nfa.cc
       4.1%   168Ki   2.5%  7.94Ki ../third_party/re2/re2/tostring.cc
       3.8%   157Ki   2.4%  7.54Ki ../third_party/re2/re2/onepass.cc
       3.4%   139Ki   2.8%  8.93Ki ../third_party/re2/re2/bitstate.cc
       1.9%  80.1Ki   5.7%  18.1Ki ../third_party/re2/re2/unicode_groups.cc
       1.1%  45.3Ki   1.1%  3.38Ki ../third_party/re2/re2/perl_groups.cc
       1.0%  41.9Ki   0.8%  2.40Ki ../third_party/re2/re2/stringpiece.cc
       0.9%  36.8Ki   0.7%  2.30Ki ../third_party/re2/util/strutil.cc
       0.8%  33.5Ki   2.1%  6.58Ki ../third_party/re2/re2/unicode_casefold.cc
       0.1%  4.88Ki   0.4%  1.18Ki ../third_party/re2/util/rune.cc
  11.6%  1.92Mi   6.4%   377Ki src
      38.4%   756Ki  33.8%   127Ki ../src/bloaty.cc
      19.0%   374Ki  14.3%  53.9Ki ../src/dwarf.cc
       8.4%   164Ki  13.7%  51.8Ki src/bloaty.pb.cc
       7.7%   151Ki  10.5%  39.5Ki ../src/elf.cc
       7.5%   147Ki   0.8%  3.17Ki ../src/main.cc
       5.7%   111Ki   7.9%  29.7Ki ../src/macho.cc
       4.5%  89.1Ki   4.5%  16.8Ki ../src/webassembly.cc
       4.0%  78.9Ki   9.0%  33.9Ki ../src/demangle.cc
       2.9%  58.0Ki   3.6%  13.7Ki ../src/range_map.cc
       1.9%  36.9Ki   1.9%  7.13Ki ../src/disassemble.cc
   2.9%   490Ki   1.5%  88.8Ki third_party/demumble
     100.0%   490Ki 100.0%  88.8Ki ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   1.5%   254Ki   1.7%  98.4Ki third_party/abseil
      21.3%  54.2Ki  23.0%  22.6Ki ../third_party/abseil-cpp/absl/strings/internal/charconv_bigint.cc
      18.4%  47.0Ki  14.9%  14.7Ki ../third_party/abseil-cpp/absl/strings/escaping.cc
      11.6%  29.6Ki  14.5%  14.2Ki ../third_party/abseil-cpp/absl/strings/charconv.cc
      11.6%  29.5Ki  12.5%  12.2Ki ../third_party/abseil-cpp/absl/strings/numbers.cc
       6.9%  17.7Ki   6.6%  6.46Ki ../third_party/abseil-cpp/absl/strings/str_cat.cc
       6.7%  17.2Ki   7.6%  7.46Ki ../third_party/abseil-cpp/absl/base/internal/throw_delegate.cc
       5.6%  14.4Ki   4.8%  4.76Ki ../third_party/abseil-cpp/absl/strings/string_view.cc
       4.0%  10.2Ki   1.9%  1.85Ki ../third_party/abseil-cpp/absl/strings/ascii.cc
       3.4%  8.60Ki   3.3%  3.26Ki ../third_party/abseil-cpp/absl/base/internal/raw_logging.cc
       3.1%  7.88Ki   4.4%  4.29Ki ../third_party/abseil-cpp/absl/strings/internal/charconv_parse.cc
       3.0%  7.54Ki   2.8%  2.78Ki ../third_party/abseil-cpp/absl/strings/substitute.cc
       2.9%  7.28Ki   2.0%  1.95Ki ../third_party/abseil-cpp/absl/strings/str_split.cc
       1.3%  3.29Ki   1.6%  1.53Ki ../third_party/abseil-cpp/absl/strings/internal/memutil.cc
       0.2%     628   0.2%     230 ../third_party/abseil-cpp/absl/strings/internal/utf8.cc
   0.3%  52.7Ki   0.0%       0 [section .debug_str]
   0.2%  36.3Ki   0.6%  36.3Ki [section .rodata]
   0.1%  19.6Ki   0.0%       0 [section .strtab]
   0.1%  17.8Ki   0.3%  17.8Ki [section .gcc_except_table]
   0.1%  16.4Ki   0.0%       0 [section .debug_loc]
   0.1%  15.1Ki   0.0%       0 [section .symtab]
   0.1%  13.0Ki   0.0%       0 [section .debug_aranges]
   0.1%  12.3Ki   0.2%  12.3Ki [section .gnu.hash]
   0.1%  11.3Ki   0.1%  8.02Ki [26 Others]
   0.1%  9.80Ki   0.2%  9.80Ki [section .dynstr]
   0.0%  6.56Ki   0.1%  6.56Ki [section .data]
   0.0%  6.12Ki   0.1%  6.12Ki [section .dynsym]
   0.0%  5.08Ki   0.0%       0 [section .debug_ranges]
   0.0%  4.04Ki   0.1%  4.04Ki [section .text]
   0.0%  3.62Ki   0.1%  3.62Ki [section .gnu.version]
   0.0%  3.46Ki   0.0%       0 [Unmapped]
 100.0%  16.6Mi 100.0%  5.76Mi TOTAL
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
