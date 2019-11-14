
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

Building Bloaty requires CMake and ``protoc``, the protobuf compiler. On Ubuntu, install them with:

```
$ sudo apt install cmake protobuf-compiler
```

Bloaty bundles ``libprotobuf``, ``re2``, ``capstone``, and ``pkg-config`` as Git submodules, but it will prefer the system's versions of those dependencies if available. All other dependencies are included as Git submodules. To build, run:

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
  32.3%  6.84Mi   0.0%       0    .debug_info
  19.6%  4.15Mi   0.0%       0    .debug_loc
  11.3%  2.39Mi  39.5%  2.39Mi    .rodata
   9.4%  1.98Mi   0.0%       0    .debug_str
   6.8%  1.43Mi   0.0%       0    .debug_ranges
   5.9%  1.24Mi  20.5%  1.24Mi    .text
   5.8%  1.24Mi   0.0%       0    .debug_line
   0.0%       0  16.6%  1.00Mi    .bss
   2.0%   440Ki   7.1%   440Ki    .data
   1.6%   352Ki   5.7%   352Ki    .rela.dyn
   1.5%   329Ki   5.3%   329Ki    .data.rel.ro
   1.0%   208Ki   0.0%       0    .strtab
   0.6%   138Ki   0.0%       0    .debug_abbrev
   0.6%   122Ki   0.0%       0    .symtab
   0.6%   120Ki   1.9%   120Ki    .eh_frame
   0.5%   100Ki   1.6%   100Ki    .dynstr
   0.2%  43.6Ki   0.7%  43.5Ki    .dynsym
   0.2%  35.2Ki   0.4%  27.9Ki    [24 Others]
   0.1%  20.3Ki   0.3%  20.2Ki    .eh_frame_hdr
   0.1%  19.8Ki   0.3%  19.8Ki    .gcc_except_table
   0.1%  13.3Ki   0.0%       0    .debug_aranges
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  34.5%  7.30Mi  34.5%  2.08Mi    [124 Others]
  10.5%  2.22Mi   6.7%   413Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.7%   366Ki  17.4%  1.05Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
   4.5%   979Ki  13.9%   863Ki    ../third_party/capstone/arch/X86/X86Mapping.c
   4.4%   957Ki   1.3%  79.0Ki    ../third_party/capstone/arch/SystemZ/SystemZDisassembler.c
   4.1%   898Ki   1.5%  91.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   3.9%   853Ki   0.7%  42.0Ki    ../third_party/re2/re2/re2.cc
   3.7%   802Ki   2.0%   126Ki    ../src/bloaty.cc
   3.6%   772Ki   0.6%  38.6Ki    ../third_party/re2/re2/dfa.cc
   3.3%   705Ki   0.6%  39.8Ki    ../third_party/re2/re2/regexp.cc
   3.1%   662Ki   1.1%  67.8Ki    ../third_party/capstone/arch/Mips/MipsDisassembler.c
   2.7%   577Ki   0.4%  23.4Ki    ../third_party/re2/re2/prog.cc
   2.5%   549Ki   7.0%   432Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   2.5%   544Ki   1.5%  92.6Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.5%   537Ki   0.6%  35.3Ki    ../third_party/re2/re2/parse.cc
   2.4%   524Ki   2.8%   172Ki    ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
   2.3%   503Ki   0.4%  26.4Ki    ../third_party/re2/re2/compile.cc
   2.1%   460Ki   0.6%  35.8Ki    ../third_party/capstone/arch/PowerPC/PPCDisassembler.c
   2.0%   427Ki   1.7%   108Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   1.9%   409Ki   4.5%   278Ki    ../third_party/capstone/arch/SystemZ/SystemZMapping.c
   1.8%   400Ki   0.2%  15.0Ki    ../third_party/re2/re2/nfa.cc
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  --source-filter=PATTERN
                     Only show keys with names matching this pattern.

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
  76.1%  16.1Mi   0.0%       0    [Unmapped]
    42.4%  6.84Mi   NAN%       0    .debug_info
    25.7%  4.15Mi   NAN%       0    .debug_loc
    12.3%  1.98Mi   NAN%       0    .debug_str
     8.9%  1.43Mi   NAN%       0    .debug_ranges
     7.7%  1.24Mi   NAN%       0    .debug_line
     1.3%   208Ki   NAN%       0    .strtab
     0.8%   138Ki   NAN%       0    .debug_abbrev
     0.7%   121Ki   NAN%       0    .symtab
     0.1%  13.2Ki   NAN%       0    .debug_aranges
     0.0%  5.65Ki   NAN%       0    [Unmapped]
     0.0%     383   NAN%       0    .shstrtab
     0.0%      28   NAN%       0    .comment
  12.0%  2.54Mi  42.1%  2.54Mi    LOAD #4 [R]
    93.8%  2.39Mi  93.8%  2.39Mi    .rodata
     4.6%   120Ki   4.6%   120Ki    .eh_frame
     0.8%  20.2Ki   0.8%  20.2Ki    .eh_frame_hdr
     0.8%  19.8Ki   0.8%  19.8Ki    .gcc_except_table
     0.0%       4   0.0%       4    [LOAD #4 [R]]
   3.6%   772Ki  29.0%  1.76Mi    LOAD #5 [RW]
     0.0%       0  57.1%  1.00Mi    .bss
    57.0%   440Ki  24.4%   440Ki    .data
    42.7%   329Ki  18.3%   329Ki    .data.rel.ro
     0.2%  1.63Ki   0.1%  1.63Ki    .got.plt
     0.1%     560   0.0%     560    .dynamic
     0.0%     200   0.0%     200    .got
     0.0%      96   0.0%      96    .init_array
     0.0%      24   0.0%      24    [LOAD #5 [RW]]
     0.0%       8   0.0%       8    .fini_array
   5.9%  1.24Mi  20.5%  1.24Mi    LOAD #3 [RX]
    99.7%  1.24Mi  99.7%  1.24Mi    .text
     0.3%  3.23Ki   0.3%  3.23Ki    .plt
     0.0%      96   0.0%      96    .plt.got
     0.0%      23   0.0%      23    .init
     0.0%      12   0.0%      12    [LOAD #3 [RX]]
     0.0%       9   0.0%       9    .fini
   2.4%   517Ki   8.4%   517Ki    LOAD #2 [R]
    68.0%   352Ki  68.0%   352Ki    .rela.dyn
    19.3%   100Ki  19.3%   100Ki    .dynstr
     8.4%  43.5Ki   8.4%  43.5Ki    .dynsym
     2.4%  12.4Ki   2.4%  12.4Ki    .gnu.hash
     0.9%  4.83Ki   0.9%  4.83Ki    .rela.plt
     0.7%  3.62Ki   0.7%  3.62Ki    .gnu.version
     0.1%     691   0.1%     691    [LOAD #2 [R]]
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
     2.6%      64   NAN%       0    .debug_aranges
     2.6%      64   NAN%       0    .debug_info
     2.6%      64   NAN%       0    .debug_line
     2.6%      64   NAN%       0    .debug_loc
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
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  76.1%  16.1Mi   0.0%       0    [Unmapped]
  12.0%  2.54Mi  42.1%  2.54Mi    LOAD #4 [R]
   3.6%   772Ki  29.0%  1.76Mi    LOAD #5 [RW]
   5.9%  1.24Mi  20.5%  1.24Mi    LOAD #3 [RX]
   2.4%   517Ki   8.4%   517Ki    LOAD #2 [R]
   0.0%  2.44Ki   0.0%       0    [ELF Headers]
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  90.8%  1.27Mi   0.0%       0    Section []
   5.7%  81.6Ki  76.7%  81.6Ki    Section [AX]
   1.7%  24.0Ki  22.6%  24.0Ki    Section [A]
   1.7%  24.0Ki   0.0%       0    [ELF Headers]
   0.1%     991   0.0%       0    [Unmapped]
   0.0%     656   0.7%     725    Section [AW]
 100.0%  1.40Mi 100.0%   106Ki    TOTAL
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
  32.3%  6.84Mi   0.0%       0    .debug_info
  19.6%  4.15Mi   0.0%       0    .debug_loc
  11.3%  2.39Mi  39.5%  2.39Mi    .rodata
   9.4%  1.98Mi   0.0%       0    .debug_str
   6.8%  1.43Mi   0.0%       0    .debug_ranges
   5.9%  1.24Mi  20.5%  1.24Mi    .text
   5.8%  1.24Mi   0.0%       0    .debug_line
   0.0%       0  16.6%  1.00Mi    .bss
   2.0%   440Ki   7.1%   440Ki    .data
   1.6%   352Ki   5.7%   352Ki    .rela.dyn
   1.5%   329Ki   5.3%   329Ki    .data.rel.ro
   1.0%   208Ki   0.0%       0    .strtab
   0.6%   138Ki   0.0%       0    .debug_abbrev
   0.6%   122Ki   0.0%       0    .symtab
   0.6%   120Ki   1.9%   120Ki    .eh_frame
   0.5%   100Ki   1.6%   100Ki    .dynstr
   0.2%  43.6Ki   0.7%  43.5Ki    .dynsym
   0.2%  35.2Ki   0.4%  27.9Ki    [24 Others]
   0.1%  20.3Ki   0.3%  20.2Ki    .eh_frame_hdr
   0.1%  19.8Ki   0.3%  19.8Ki    .gcc_except_table
   0.1%  13.3Ki   0.0%       0    .debug_aranges
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
```

## Symbols

Symbols come from the symbol table, and represent individual
functions or variables.

```cmdoutput
$ ./bloaty -d symbols bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  32.3%  6.84Mi   0.0%       0    [section .debug_info]
  19.6%  4.15Mi   0.0%       0    [section .debug_loc]
  12.3%  2.60Mi  37.3%  2.26Mi    [3789 Others]
   9.4%  1.98Mi   0.0%       0    [section .debug_str]
   6.8%  1.43Mi   0.0%       0    [section .debug_ranges]
   6.8%  1.43Mi  23.6%  1.43Mi    insns
   5.8%  1.24Mi   0.0%       0    [section .debug_line]
   0.0%      44  16.5%  1024Ki    g_instruction_table
   1.3%   279Ki   4.5%   279Ki    insn_name_maps
   1.0%   218Ki   3.5%   218Ki    ARMInsts
   0.8%   175Ki   2.8%   175Ki    insn_ops
   0.6%   140Ki   2.3%   140Ki    x86DisassemblerTwoByteOpcodes
   0.6%   138Ki   0.0%       0    [section .debug_abbrev]
   0.6%   119Ki   1.9%   119Ki    AArch64_printInst
   0.5%   101Ki   1.6%   101Ki    Sparc_printInst
   0.4%  81.0Ki   1.3%  81.0Ki    PPC_printInst
   0.3%  74.0Ki   1.2%  74.0Ki    x86DisassemblerThreeByte38Opcodes
   0.3%  61.1Ki   1.0%  60.9Ki    DecoderTable32
   0.2%  54.0Ki   0.9%  54.0Ki    x86DisassemblerThreeByte3AOpcodes
   0.2%  50.1Ki   0.8%  49.8Ki    reg_name_maps
   0.2%  42.6Ki   0.7%  42.5Ki    SystemZ_getInstruction
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  37.4%  1.40Mi  33.6%   106Ki    CMakeFiles/libbloaty.dir/src/bloaty.cc.o
  18.4%   702Ki  14.2%  45.0Ki    CMakeFiles/libbloaty.dir/src/dwarf.cc.o
  10.4%   395Ki  13.3%  42.1Ki    CMakeFiles/libbloaty.dir/src/bloaty.pb.cc.o
   9.8%   374Ki  12.7%  40.2Ki    CMakeFiles/libbloaty.dir/src/elf.cc.o
   7.8%   298Ki   8.5%  26.8Ki    CMakeFiles/libbloaty.dir/src/macho.cc.o
   5.9%   226Ki   4.8%  15.1Ki    CMakeFiles/libbloaty.dir/src/webassembly.cc.o
   3.8%   146Ki   4.0%  12.5Ki    CMakeFiles/libbloaty.dir/src/range_map.cc.o
   3.7%   142Ki   6.4%  20.4Ki    CMakeFiles/libbloaty.dir/src/demangle.cc.o
   2.7%   103Ki   2.4%  7.66Ki    CMakeFiles/libbloaty.dir/src/disassemble.cc.o
 100.0%  3.73Mi 100.0%   316Ki    TOTAL
```

## Archive Members

When you are running Bloaty on a `.a` file, the `armembers`
source will let you break it down by `.o` file inside the
archive.

```cmdoutput
$ ./bloaty -d armembers liblibbloaty.a
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  25.5%  1.40Mi  21.4%   106Ki    bloaty.cc.o
  20.1%  1.10Mi  19.1%  95.0Ki    cxa_demangle.cpp.o
  12.5%   702Ki   9.1%  45.0Ki    dwarf.cc.o
   7.1%   395Ki   8.5%  42.1Ki    bloaty.pb.cc.o
   6.7%   374Ki   8.1%  40.2Ki    elf.cc.o
   5.3%   298Ki   5.4%  26.8Ki    macho.cc.o
   4.0%   226Ki   3.0%  15.1Ki    webassembly.cc.o
   2.6%   146Ki   2.5%  12.5Ki    range_map.cc.o
   2.5%   142Ki   4.1%  20.4Ki    demangle.cc.o
   2.2%   122Ki   3.0%  14.8Ki    escaping.cc.o
   2.0%   114Ki   3.5%  17.4Ki    charconv_bigint.cc.o
   1.9%   103Ki   1.5%  7.66Ki    disassemble.cc.o
   1.5%  81.4Ki   2.2%  10.8Ki    [8 Others]
   1.2%  65.1Ki   0.0%       0    [AR Symbol Table]
   1.1%  60.4Ki   2.3%  11.4Ki    numbers.cc.o
   1.0%  56.5Ki   2.8%  13.8Ki    charconv.cc.o
   0.9%  47.9Ki   1.3%  6.22Ki    str_cat.cc.o
   0.6%  34.4Ki   0.8%  3.91Ki    throw_delegate.cc.o
   0.5%  28.0Ki   0.5%  2.36Ki    ascii.cc.o
   0.5%  26.3Ki   0.7%  3.26Ki    string_view.cc.o
   0.4%  25.1Ki   0.3%  1.50Ki    str_split.cc.o
 100.0%  5.48Mi 100.0%   496Ki    TOTAL
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
  34.5%  7.30Mi  34.5%  2.08Mi    [124 Others]
  10.5%  2.22Mi   6.7%   413Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.7%   366Ki  17.4%  1.05Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
   4.5%   979Ki  13.9%   863Ki    ../third_party/capstone/arch/X86/X86Mapping.c
   4.4%   957Ki   1.3%  79.0Ki    ../third_party/capstone/arch/SystemZ/SystemZDisassembler.c
   4.1%   898Ki   1.5%  91.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   3.9%   853Ki   0.7%  42.0Ki    ../third_party/re2/re2/re2.cc
   3.7%   802Ki   2.0%   126Ki    ../src/bloaty.cc
   3.6%   772Ki   0.6%  38.6Ki    ../third_party/re2/re2/dfa.cc
   3.3%   705Ki   0.6%  39.8Ki    ../third_party/re2/re2/regexp.cc
   3.1%   662Ki   1.1%  67.8Ki    ../third_party/capstone/arch/Mips/MipsDisassembler.c
   2.7%   577Ki   0.4%  23.4Ki    ../third_party/re2/re2/prog.cc
   2.5%   549Ki   7.0%   432Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   2.5%   544Ki   1.5%  92.6Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.5%   537Ki   0.6%  35.3Ki    ../third_party/re2/re2/parse.cc
   2.4%   524Ki   2.8%   172Ki    ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
   2.3%   503Ki   0.4%  26.4Ki    ../third_party/re2/re2/compile.cc
   2.1%   460Ki   0.6%  35.8Ki    ../third_party/capstone/arch/PowerPC/PPCDisassembler.c
   2.0%   427Ki   1.7%   108Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   1.9%   409Ki   4.5%   278Ki    ../third_party/capstone/arch/SystemZ/SystemZMapping.c
   1.8%   400Ki   0.2%  15.0Ki    ../third_party/re2/re2/nfa.cc
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  32.3%  6.84Mi   0.0%       0    [section .debug_info]
  19.6%  4.15Mi   0.0%       0    [section .debug_loc]
  11.3%  2.39Mi  39.5%  2.39Mi    [section .rodata]
   9.4%  1.98Mi   0.0%       0    [section .debug_str]
   6.8%  1.43Mi   0.0%       0    [section .debug_ranges]
   5.9%  1.25Mi  20.3%  1.23Mi    [35364 Others]
   5.8%  1.24Mi   0.0%       0    [section .debug_line]
   0.0%       0  16.6%  1.00Mi    [section .bss]
   2.0%   440Ki   7.1%   440Ki    [section .data]
   1.6%   352Ki   5.7%   352Ki    [section .rela.dyn]
   1.5%   329Ki   5.3%   329Ki    [section .data.rel.ro]
   1.0%   208Ki   0.0%       0    [section .strtab]
   0.6%   138Ki   0.0%       0    [section .debug_abbrev]
   0.6%   122Ki   0.0%       0    [section .symtab]
   0.6%   120Ki   1.9%   120Ki    [section .eh_frame]
   0.5%   100Ki   1.6%   100Ki    [section .dynstr]
   0.2%  43.6Ki   0.7%  43.5Ki    [section .dynsym]
   0.1%  20.3Ki   0.3%  20.2Ki    [section .eh_frame_hdr]
   0.1%  20.1Ki   0.3%  20.1Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c:115
   0.1%  19.8Ki   0.3%  19.8Ki    [section .gcc_except_table]
   0.1%  16.0Ki   0.3%  16.0Ki    /usr/include/c++/8/bits/basic_string.h:220
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  34.5%  7.30Mi  34.5%  2.08Mi    [124 Others]
  10.5%  2.22Mi   6.7%   413Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
   1.7%   366Ki  17.4%  1.05Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
   4.5%   979Ki  13.9%   863Ki    ../third_party/capstone/arch/X86/X86Mapping.c
   4.4%   957Ki   1.3%  79.0Ki    ../third_party/capstone/arch/SystemZ/SystemZDisassembler.c
   4.1%   898Ki   1.5%  91.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
   3.9%   853Ki   0.7%  42.0Ki    ../third_party/re2/re2/re2.cc
   3.7%   802Ki   2.0%   126Ki    ../src/bloaty.cc
   3.6%   772Ki   0.6%  38.6Ki    ../third_party/re2/re2/dfa.cc
   3.3%   705Ki   0.6%  39.8Ki    ../third_party/re2/re2/regexp.cc
   3.1%   662Ki   1.1%  67.8Ki    ../third_party/capstone/arch/Mips/MipsDisassembler.c
   2.7%   577Ki   0.4%  23.4Ki    ../third_party/re2/re2/prog.cc
   2.5%   549Ki   7.0%   432Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
   2.5%   544Ki   1.5%  92.6Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   2.5%   537Ki   0.6%  35.3Ki    ../third_party/re2/re2/parse.cc
   2.4%   524Ki   2.8%   172Ki    ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
   2.3%   503Ki   0.4%  26.4Ki    ../third_party/re2/re2/compile.cc
   2.1%   460Ki   0.6%  35.8Ki    ../third_party/capstone/arch/PowerPC/PPCDisassembler.c
   2.0%   427Ki   1.7%   108Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
   1.9%   409Ki   4.5%   278Ki    ../third_party/capstone/arch/SystemZ/SystemZMapping.c
   1.8%   400Ki   0.2%  15.0Ki    ../third_party/re2/re2/nfa.cc
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
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
  58.4%  12.4Mi  84.0%  5.08Mi    third_party/capstone
  25.7%  5.44Mi   4.8%   295Ki    third_party/re2
   9.8%  2.07Mi   6.3%   390Ki    src
   2.5%   544Ki   1.5%  92.6Ki    third_party/demumble
   1.3%   281Ki   0.0%       0    [section .debug_loc]
   1.3%   279Ki   1.6%   100Ki    third_party/abseil
   0.3%  57.9Ki   0.0%       0    [section .debug_str]
   0.2%  36.5Ki   0.6%  36.5Ki    [section .rodata]
   0.1%  19.8Ki   0.3%  19.8Ki    [section .gcc_except_table]
   0.1%  19.5Ki   0.0%       0    [section .strtab]
   0.1%  15.1Ki   0.0%       0    [section .symtab]
   0.1%  14.7Ki   0.2%  11.4Ki    [28 Others]
   0.1%  13.2Ki   0.0%       0    [section .debug_aranges]
   0.1%  12.4Ki   0.2%  12.4Ki    [section .gnu.hash]
   0.0%  9.50Ki   0.2%  9.50Ki    [section .dynstr]
   0.0%  6.54Ki   0.1%  6.54Ki    [section .data]
   0.0%  5.98Ki   0.1%  5.98Ki    [section .dynsym]
   0.0%  5.92Ki   0.0%       0    [section .debug_ranges]
   0.0%  5.65Ki   0.0%       0    [Unmapped]
   0.0%  4.25Ki   0.1%  4.25Ki    [section .text]
   0.0%  4.06Ki   0.1%  4.06Ki    [section .eh_frame]
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
```

We can get an even richer report by combining the
`bloaty_package` source with the original `compileunits`
source:

```cmdoutput
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  58.4%  12.4Mi  84.0%  5.08Mi    third_party/capstone
    17.9%  2.22Mi   8.0%   413Ki    ../third_party/capstone/arch/ARM/ARMDisassembler.c
    13.6%  1.68Mi   7.1%   369Ki    [38 Others]
     2.9%   366Ki  20.7%  1.05Mi    ../third_party/capstone/arch/M68K/M68KDisassembler.c
     7.7%   979Ki  16.6%   863Ki    ../third_party/capstone/arch/X86/X86Mapping.c
     7.6%   957Ki   1.5%  79.0Ki    ../third_party/capstone/arch/SystemZ/SystemZDisassembler.c
     7.1%   898Ki   1.8%  91.2Ki    ../third_party/capstone/arch/AArch64/AArch64Disassembler.c
     5.2%   662Ki   1.3%  67.8Ki    ../third_party/capstone/arch/Mips/MipsDisassembler.c
     4.3%   549Ki   8.3%   432Ki    ../third_party/capstone/arch/X86/X86DisassemblerDecoder.c
     4.1%   524Ki   3.3%   172Ki    ../third_party/capstone/arch/AArch64/AArch64InstPrinter.c
     3.6%   460Ki   0.7%  35.8Ki    ../third_party/capstone/arch/PowerPC/PPCDisassembler.c
     3.4%   427Ki   2.1%   108Ki    ../third_party/capstone/arch/X86/X86ATTInstPrinter.c
     3.2%   409Ki   5.4%   278Ki    ../third_party/capstone/arch/SystemZ/SystemZMapping.c
     3.0%   376Ki   1.9%   101Ki    ../third_party/capstone/arch/ARM/ARMInstPrinter.c
     2.6%   335Ki   2.1%   111Ki    ../third_party/capstone/arch/PowerPC/PPCInstPrinter.c
     2.6%   325Ki   2.2%   111Ki    ../third_party/capstone/arch/Sparc/SparcInstPrinter.c
     2.2%   284Ki   4.6%   237Ki    ../third_party/capstone/arch/AArch64/AArch64Mapping.c
     2.1%   262Ki   4.1%   213Ki    ../third_party/capstone/arch/ARM/ARMMapping.c
     2.0%   259Ki   1.9%   100Ki    ../third_party/capstone/arch/X86/X86IntelInstPrinter.c
     1.6%   202Ki   3.1%   160Ki    ../third_party/capstone/arch/PowerPC/PPCMapping.c
     1.6%   201Ki   2.9%   152Ki    ../third_party/capstone/arch/Mips/MipsMapping.c
     1.4%   181Ki   0.5%  28.2Ki    ../third_party/capstone/arch/X86/X86Disassembler.c
  25.7%  5.44Mi   4.8%   295Ki    third_party/re2
    15.3%   853Ki  14.2%  42.0Ki    ../third_party/re2/re2/re2.cc
    13.9%   772Ki  13.1%  38.6Ki    ../third_party/re2/re2/dfa.cc
    12.7%   705Ki  13.5%  39.8Ki    ../third_party/re2/re2/regexp.cc
    10.4%   577Ki   7.9%  23.4Ki    ../third_party/re2/re2/prog.cc
     9.6%   537Ki  12.0%  35.3Ki    ../third_party/re2/re2/parse.cc
     9.0%   503Ki   8.9%  26.4Ki    ../third_party/re2/re2/compile.cc
     7.2%   400Ki   5.1%  15.0Ki    ../third_party/re2/re2/nfa.cc
     6.8%   376Ki   7.4%  21.8Ki    ../third_party/re2/re2/simplify.cc
     4.4%   243Ki   2.2%  6.40Ki    ../third_party/re2/re2/onepass.cc
     4.0%   221Ki   1.8%  5.38Ki    ../third_party/re2/re2/bitstate.cc
     3.8%   213Ki   2.4%  7.20Ki    ../third_party/re2/re2/tostring.cc
     1.0%  55.5Ki   6.3%  18.5Ki    ../third_party/re2/re2/unicode_groups.cc
     0.9%  49.2Ki   0.8%  2.22Ki    ../third_party/re2/re2/stringpiece.cc
     0.8%  41.9Ki   0.6%  1.73Ki    ../third_party/re2/util/strutil.cc
     0.1%  7.53Ki   2.3%  6.71Ki    ../third_party/re2/re2/unicode_casefold.cc
     0.1%  5.68Ki   0.4%  1.17Ki    ../third_party/re2/util/rune.cc
     0.1%  4.92Ki   1.1%  3.38Ki    ../third_party/re2/re2/perl_groups.cc
   9.8%  2.07Mi   6.3%   390Ki    src
    37.9%   802Ki  32.4%   126Ki    ../src/bloaty.cc
    18.5%   392Ki  14.2%  55.4Ki    ../src/dwarf.cc
     9.1%   191Ki  14.3%  55.7Ki    src/bloaty.pb.cc
     7.8%   166Ki  10.5%  40.9Ki    ../src/elf.cc
     7.7%   163Ki   1.9%  7.30Ki    ../src/main.cc
     5.6%   118Ki   7.6%  29.8Ki    ../src/macho.cc
     4.6%  97.6Ki   4.4%  17.1Ki    ../src/webassembly.cc
     3.9%  82.9Ki   9.4%  36.6Ki    ../src/demangle.cc
     2.9%  62.1Ki   3.5%  13.7Ki    ../src/range_map.cc
     1.9%  40.9Ki   1.9%  7.32Ki    ../src/disassemble.cc
   2.5%   544Ki   1.5%  92.6Ki    third_party/demumble
   100.0%   544Ki 100.0%  92.6Ki    ../third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   1.3%   281Ki   0.0%       0    [section .debug_loc]
   1.3%   279Ki   1.6%   100Ki    third_party/abseil
    20.1%  56.2Ki  21.2%  21.4Ki    ../third_party/abseil-cpp/absl/strings/internal/charconv_bigint.cc
    19.9%  55.6Ki  15.8%  15.9Ki    ../third_party/abseil-cpp/absl/strings/escaping.cc
    11.4%  31.9Ki  14.2%  14.3Ki    ../third_party/abseil-cpp/absl/strings/charconv.cc
    10.3%  28.8Ki  12.5%  12.6Ki    ../third_party/abseil-cpp/absl/strings/numbers.cc
     7.6%  21.2Ki   8.3%  8.38Ki    ../third_party/abseil-cpp/absl/base/internal/throw_delegate.cc
     7.4%  20.8Ki   6.9%  6.98Ki    ../third_party/abseil-cpp/absl/strings/str_cat.cc
     4.8%  13.4Ki   4.1%  4.10Ki    ../third_party/abseil-cpp/absl/strings/string_view.cc
     3.9%  10.9Ki   1.9%  1.94Ki    ../third_party/abseil-cpp/absl/strings/ascii.cc
     3.2%  8.99Ki   3.3%  3.29Ki    ../third_party/abseil-cpp/absl/strings/substitute.cc
     3.2%  8.89Ki   3.2%  3.24Ki    ../third_party/abseil-cpp/absl/base/internal/raw_logging.cc
     3.2%  8.81Ki   4.5%  4.51Ki    ../third_party/abseil-cpp/absl/strings/internal/charconv_parse.cc
     2.6%  7.38Ki   1.8%  1.81Ki    ../third_party/abseil-cpp/absl/strings/str_split.cc
     1.3%  3.49Ki   1.6%  1.57Ki    ../third_party/abseil-cpp/absl/strings/internal/memutil.cc
     0.7%  2.09Ki   0.6%     635    ../third_party/abseil-cpp/absl/strings/match.cc
     0.2%     670   0.2%     230    ../third_party/abseil-cpp/absl/strings/internal/utf8.cc
   0.3%  57.9Ki   0.0%       0    [section .debug_str]
   0.2%  36.5Ki   0.6%  36.5Ki    [section .rodata]
   0.1%  19.8Ki   0.3%  19.8Ki    [section .gcc_except_table]
   0.1%  19.5Ki   0.0%       0    [section .strtab]
   0.1%  15.1Ki   0.0%       0    [section .symtab]
   0.1%  14.7Ki   0.2%  11.4Ki    [28 Others]
   0.1%  13.2Ki   0.0%       0    [section .debug_aranges]
   0.1%  12.4Ki   0.2%  12.4Ki    [section .gnu.hash]
   0.0%  9.50Ki   0.2%  9.50Ki    [section .dynstr]
   0.0%  6.54Ki   0.1%  6.54Ki    [section .data]
   0.0%  5.98Ki   0.1%  5.98Ki    [section .dynsym]
   0.0%  5.92Ki   0.0%       0    [section .debug_ranges]
   0.0%  5.65Ki   0.0%       0    [Unmapped]
   0.0%  4.25Ki   0.1%  4.25Ki    [section .text]
   0.0%  4.06Ki   0.1%  4.06Ki    [section .eh_frame]
 100.0%  21.2Mi 100.0%  6.05Mi    TOTAL
```

# Source filter

Sometimes, you are only interested in parts of the binary
instead of the whole package.  This is common in embedded
programming, where ELF files are used only as a container
format, and only a few sections are actually loaded onto
the device.

For this, Bloaty provides a `--source-filter` option which
allows filtering out irrelevant data.  It takes a regex
which is applied to each of the symbol names in a data
source.  Only symbols which match the regex are displayed
in the output.  This is especially powerful when combined
with custom data sources, as the rewriting occurs before
the filtering.

In the case of hierarchical data source profiles, the regex
is applied to all symbol names in the hierarchy.  If any
name matches, all of its parents will be displayed as well.

For example, given the above scenario, maybe we are only
interested in how large the first-party Bloaty code is.
This can be displayed using a source filter on the `src`
directory.

```cmdoutput
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits --source-filter src bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
 100.0%  2.07Mi 100.0%   390Ki    src
    37.9%   802Ki  32.4%   126Ki    ../src/bloaty.cc
    18.5%   392Ki  14.2%  55.4Ki    ../src/dwarf.cc
     9.1%   191Ki  14.3%  55.7Ki    src/bloaty.pb.cc
     7.8%   166Ki  10.5%  40.9Ki    ../src/elf.cc
     7.7%   163Ki   1.9%  7.30Ki    ../src/main.cc
     5.6%   118Ki   7.6%  29.8Ki    ../src/macho.cc
     4.6%  97.6Ki   4.4%  17.1Ki    ../src/webassembly.cc
     3.9%  82.9Ki   9.4%  36.6Ki    ../src/demangle.cc
     2.9%  62.1Ki   3.5%  13.7Ki    ../src/range_map.cc
     1.9%  40.9Ki   1.9%  7.32Ki    ../src/disassemble.cc
 100.0%  2.07Mi 100.0%   390Ki    TOTAL
Filtering enabled (source_filter); omitted file = 19.1Mi, vm = 5.67Mi of entries
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
