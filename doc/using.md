
# User Documentation

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
  30.0%  8.85Mi   0.0%       0    .debug_info
  24.7%  7.29Mi   0.0%       0    .debug_loc
  12.8%  3.79Mi   0.0%       0    .debug_str
   9.7%  2.86Mi  42.8%  2.86Mi    .rodata
   6.9%  2.03Mi  30.3%  2.03Mi    .text
   6.3%  1.85Mi   0.0%       0    .debug_line
   4.0%  1.19Mi   0.0%       0    .debug_ranges
   0.0%       0  15.0%  1.01Mi    .bss
   1.6%   473Ki   0.0%       0    .strtab
   1.4%   435Ki   6.3%   435Ki    .data
   0.8%   254Ki   3.7%   254Ki    .eh_frame
   0.8%   231Ki   0.0%       0    .symtab
   0.5%   142Ki   0.0%       0    .debug_abbrev
   0.2%  56.8Ki   0.8%  56.8Ki    .gcc_except_table
   0.1%  41.4Ki   0.6%  41.4Ki    .eh_frame_hdr
   0.0%  11.4Ki   0.1%  9.45Ki    [26 Others]
   0.0%  7.20Ki   0.1%  7.14Ki    .dynstr
   0.0%  6.09Ki   0.1%  6.02Ki    .dynsym
   0.0%  4.89Ki   0.1%  4.83Ki    .rela.plt
   0.0%  4.59Ki   0.0%       0    [Unmapped]
   0.0%  3.30Ki   0.0%  3.23Ki    .plt
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
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
  80.7%  23.8Mi   0.0%       0    [Unmapped]
    37.2%  8.85Mi   NAN%       0    .debug_info
    30.6%  7.29Mi   NAN%       0    .debug_loc
    15.9%  3.79Mi   NAN%       0    .debug_str
     7.8%  1.85Mi   NAN%       0    .debug_line
     5.0%  1.19Mi   NAN%       0    .debug_ranges
     1.9%   473Ki   NAN%       0    .strtab
     1.0%   231Ki   NAN%       0    .symtab
     0.6%   142Ki   NAN%       0    .debug_abbrev
     0.0%  4.59Ki   NAN%       0    [Unmapped]
     0.0%     392   NAN%       0    .shstrtab
     0.0%     139   NAN%       0    .debug_macinfo
     0.0%      68   NAN%       0    .comment
  10.9%  3.21Mi  47.9%  3.21Mi    LOAD #4 [R]
    89.3%  2.86Mi  89.3%  2.86Mi    .rodata
     7.7%   254Ki   7.7%   254Ki    .eh_frame
     1.7%  56.8Ki   1.7%  56.8Ki    .gcc_except_table
     1.3%  41.4Ki   1.3%  41.4Ki    .eh_frame_hdr
     0.0%       1   0.0%       1    [LOAD #4 [R]]
   6.9%  2.03Mi  30.3%  2.03Mi    LOAD #3 [RX]
    99.8%  2.03Mi  99.8%  2.03Mi    .text
     0.2%  3.23Ki   0.2%  3.23Ki    .plt
     0.0%      28   0.0%      28    [LOAD #3 [RX]]
     0.0%      23   0.0%      23    .init
     0.0%       9   0.0%       9    .fini
   1.5%   439Ki  21.4%  1.44Mi    LOAD #5 [RW]
     0.0%       0  70.1%  1.01Mi    .bss
    99.1%   435Ki  29.6%   435Ki    .data
     0.4%  1.63Ki   0.1%  1.63Ki    .got.plt
     0.3%  1.46Ki   0.1%  1.46Ki    .data.rel.ro
     0.1%     560   0.0%     560    .dynamic
     0.1%     384   0.0%     376    .init_array
     0.0%      32   0.0%      56    [LOAD #5 [RW]]
     0.0%      32   0.0%      32    .got
     0.0%      16   0.0%      16    .tdata
     0.0%       8   0.0%       8    .fini_array
     0.0%       0   0.0%       8    .tbss
   0.1%  23.3Ki   0.3%  23.3Ki    LOAD #2 [R]
    30.7%  7.14Ki  30.7%  7.14Ki    .dynstr
    25.9%  6.02Ki  25.9%  6.02Ki    .dynsym
    20.8%  4.83Ki  20.8%  4.83Ki    .rela.plt
     7.7%  1.78Ki   7.7%  1.78Ki    .hash
     5.0%  1.17Ki   5.0%  1.17Ki    .rela.dyn
     3.1%     741   3.1%     741    [LOAD #2 [R]]
     2.7%     632   2.7%     632    .gnu.hash
     2.2%     514   2.2%     514    .gnu.version
     1.6%     384   1.6%     384    .gnu.version_r
     0.2%      36   0.2%      36    .note.gnu.build-id
     0.1%      32   0.1%      32    .note.ABI-tag
     0.1%      28   0.1%      28    .interp
   0.0%  2.56Ki   0.0%       0    [ELF Headers]
    46.3%  1.19Ki   NAN%       0    [19 Others]
     7.3%     192   NAN%       0    [ELF Headers]
     2.4%      64   NAN%       0    .comment
     2.4%      64   NAN%       0    .data
     2.4%      64   NAN%       0    .data.rel.ro
     2.4%      64   NAN%       0    .debug_abbrev
     2.4%      64   NAN%       0    .debug_info
     2.4%      64   NAN%       0    .debug_line
     2.4%      64   NAN%       0    .debug_loc
     2.4%      64   NAN%       0    .debug_macinfo
     2.4%      64   NAN%       0    .debug_ranges
     2.4%      64   NAN%       0    .debug_str
     2.4%      64   NAN%       0    .dynamic
     2.4%      64   NAN%       0    .dynstr
     2.4%      64   NAN%       0    .dynsym
     2.4%      64   NAN%       0    .eh_frame
     2.4%      64   NAN%       0    .eh_frame_hdr
     2.4%      64   NAN%       0    .fini
     2.4%      64   NAN%       0    .fini_array
     2.4%      64   NAN%       0    .gcc_except_table
     2.4%      64   NAN%       0    .gnu.hash
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
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
  80.7%  23.8Mi   0.0%       0    [Unmapped]
  10.9%  3.21Mi  47.9%  3.21Mi    LOAD #4 [R]
   6.9%  2.03Mi  30.3%  2.03Mi    LOAD #3 [RX]
   1.5%   439Ki  21.4%  1.44Mi    LOAD #5 [RW]
   0.1%  23.3Ki   0.3%  23.3Ki    LOAD #2 [R]
   0.0%  2.56Ki   0.0%       0    [ELF Headers]
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
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
  87.5%   972Ki   0.0%       0    Section []
   8.2%  90.9Ki  78.3%  90.9Ki    Section [AX]
   2.3%  25.2Ki  21.7%  25.2Ki    Section [A]
   2.0%  22.6Ki   0.0%       0    [ELF Headers]
   0.1%     844   0.0%       0    [Unmapped]
   0.0%      24   0.1%      72    Section [AW]
 100.0%  1.09Mi 100.0%   116Ki    TOTAL
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
  30.0%  8.85Mi   0.0%       0    .debug_info
  24.7%  7.29Mi   0.0%       0    .debug_loc
  12.8%  3.79Mi   0.0%       0    .debug_str
   9.7%  2.86Mi  42.8%  2.86Mi    .rodata
   6.9%  2.03Mi  30.3%  2.03Mi    .text
   6.3%  1.85Mi   0.0%       0    .debug_line
   4.0%  1.19Mi   0.0%       0    .debug_ranges
   0.0%       0  15.0%  1.01Mi    .bss
   1.6%   473Ki   0.0%       0    .strtab
   1.4%   435Ki   6.3%   435Ki    .data
   0.8%   254Ki   3.7%   254Ki    .eh_frame
   0.8%   231Ki   0.0%       0    .symtab
   0.5%   142Ki   0.0%       0    .debug_abbrev
   0.2%  56.8Ki   0.8%  56.8Ki    .gcc_except_table
   0.1%  41.4Ki   0.6%  41.4Ki    .eh_frame_hdr
   0.0%  11.4Ki   0.1%  9.45Ki    [26 Others]
   0.0%  7.20Ki   0.1%  7.14Ki    .dynstr
   0.0%  6.09Ki   0.1%  6.02Ki    .dynsym
   0.0%  4.89Ki   0.1%  4.83Ki    .rela.plt
   0.0%  4.59Ki   0.0%       0    [Unmapped]
   0.0%  3.30Ki   0.0%  3.23Ki    .plt
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
```

Sections are regions of the binary that are the linker
treats as atomic when linking. The linker will never break
apart or rearrange the data within a section. This is why it
is necessary to compile with `-ffunction-sections` and
`-fdata-sections` if you want the linker to strip out
individual functions or variables that have no references.
However the linker will often combine many input sections
into a single output section.

## Symbols

Symbols come from the symbol table, and represent individual
functions or variables.

```cmdoutput
$ ./bloaty -d symbols bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  30.0%  8.85Mi   0.0%       0    [section .debug_info]
  24.7%  7.29Mi   0.0%       0    [section .debug_loc]
  12.8%  3.79Mi   0.0%       0    [section .debug_str]
  11.7%  3.44Mi  41.2%  2.76Mi    [5895 Others]
   6.3%  1.85Mi   0.0%       0    [section .debug_line]
   4.9%  1.43Mi  21.4%  1.43Mi    insns
   4.0%  1.19Mi   0.0%       0    [section .debug_ranges]
   0.0%      44  14.9%  1024Ki    g_instruction_table
   0.8%   255Ki   3.7%   255Ki    [section .rodata]
   0.8%   240Ki   3.5%   240Ki    printAliasInstr
   0.6%   175Ki   2.6%   175Ki    insn_ops
   0.5%   153Ki   2.2%   153Ki    ARMInsts
   0.5%   142Ki   0.0%       0    [section .debug_abbrev]
   0.5%   140Ki   2.0%   140Ki    x86DisassemblerTwoByteOpcodes
   0.4%   113Ki   1.6%   113Ki    insn_name_maps
   0.4%   106Ki   1.6%   106Ki    printInstruction.OpInfo
   0.3%  97.1Ki   1.4%  96.9Ki    printInstruction.OpInfo2
   0.2%  74.0Ki   1.1%  74.0Ki    x86DisassemblerThreeByte38Opcodes
   0.2%  71.1Ki   1.0%  70.8Ki    printInstruction.AsmStrs
   0.2%  61.1Ki   0.9%  60.9Ki    DecoderTable32
   0.2%  56.8Ki   0.8%  56.8Ki    [section .gcc_except_table]
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
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
  42.8%  1.09Mi  37.9%   116Ki    CMakeFiles/libbloaty.dir/src/bloaty.cc.o
  15.7%   407Ki  15.5%  47.6Ki    CMakeFiles/libbloaty.dir/src/dwarf.cc.o
  10.3%   266Ki  10.4%  31.8Ki    CMakeFiles/libbloaty.dir/src/bloaty.pb.cc.o
   9.0%   232Ki   9.7%  29.8Ki    CMakeFiles/libbloaty.dir/src/elf.cc.o
   8.0%   207Ki   8.7%  26.6Ki    CMakeFiles/libbloaty.dir/src/macho.cc.o
   4.4%   114Ki   4.3%  13.1Ki    CMakeFiles/libbloaty.dir/src/webassembly.cc.o
   4.0%   103Ki   7.5%  22.9Ki    CMakeFiles/libbloaty.dir/src/demangle.cc.o
   3.4%  87.0Ki   3.3%  10.2Ki    CMakeFiles/libbloaty.dir/src/range_map.cc.o
   2.5%  64.3Ki   2.6%  7.94Ki    CMakeFiles/libbloaty.dir/src/disassemble.cc.o
 100.0%  2.53Mi 100.0%   306Ki    TOTAL
```

## Archive Members

When you are running Bloaty on a `.a` file, the `armembers`
source will let you break it down by `.o` file inside the
archive.

```cmdoutput
$ ./bloaty -d armembers liblibbloaty.a
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  28.5%  1.21Mi  23.5%   120Ki    cxa_demangle.cpp.o
  25.6%  1.09Mi  22.6%   116Ki    bloaty.cc.o
   9.4%   407Ki   9.3%  47.6Ki    dwarf.cc.o
   6.2%   266Ki   6.2%  31.8Ki    bloaty.pb.cc.o
   5.4%   232Ki   5.8%  29.8Ki    elf.cc.o
   4.8%   207Ki   5.2%  26.6Ki    macho.cc.o
   2.6%   114Ki   2.6%  13.1Ki    webassembly.cc.o
   2.4%   103Ki   4.5%  22.9Ki    demangle.cc.o
   2.0%  87.0Ki   2.0%  10.2Ki    range_map.cc.o
   1.9%  80.4Ki   3.2%  16.7Ki    charconv_bigint.cc.o
   1.8%  79.3Ki   2.7%  14.0Ki    escaping.cc.o
   1.5%  65.0Ki   2.1%  10.9Ki    [9 Others]
   1.5%  64.3Ki   1.5%  7.94Ki    disassemble.cc.o
   1.4%  59.9Ki   0.0%       0    [AR Symbol Table]
   1.0%  45.2Ki   2.4%  12.4Ki    numbers.cc.o
   0.9%  40.9Ki   2.2%  11.4Ki    charconv.cc.o
   0.9%  38.8Ki   1.2%  6.10Ki    int128.cc.o
   0.7%  30.1Ki   1.1%  5.58Ki    str_cat.cc.o
   0.6%  24.1Ki   0.8%  3.92Ki    string_view.cc.o
   0.5%  21.2Ki   0.6%  3.21Ki    throw_delegate.cc.o
   0.4%  19.2Ki   0.4%  2.26Ki    ascii.cc.o
 100.0%  4.23Mi 100.0%   512Ki    TOTAL
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
  30.0%  8.85Mi   0.0%       0    [section .debug_info]
  24.7%  7.29Mi   0.0%       0    [section .debug_loc]
  12.8%  3.79Mi   0.0%       0    [section .debug_str]
   9.7%  2.86Mi  42.8%  2.86Mi    [section .rodata]
   6.6%  1.96Mi  29.1%  1.95Mi    [44060 Others]
   6.3%  1.85Mi   0.0%       0    [section .debug_line]
   4.0%  1.19Mi   0.0%       0    [section .debug_ranges]
   0.0%       0  15.0%  1.01Mi    [section .bss]
   1.6%   473Ki   0.0%       0    [section .strtab]
   1.4%   435Ki   6.3%   435Ki    [section .data]
   0.8%   254Ki   3.7%   254Ki    [section .eh_frame]
   0.8%   231Ki   0.0%       0    [section .symtab]
   0.5%   142Ki   0.0%       0    [section .debug_abbrev]
   0.2%  56.8Ki   0.8%  56.8Ki    [section .gcc_except_table]
   0.1%  41.4Ki   0.6%  41.4Ki    [section .eh_frame_hdr]
   0.1%  27.4Ki   0.4%  27.4Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9/bits/basic_string.h:187
   0.1%  19.1Ki   0.3%  19.1Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9/bits/basic_string.h:183
   0.1%  16.8Ki   0.2%  16.8Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9/ext/new_allocator.h:128
   0.1%  16.0Ki   0.2%  16.0Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9/bits/char_traits.h:300
   0.1%  15.8Ki   0.2%  15.8Ki    /usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9/bits/basic_string.h:222
   0.0%  14.7Ki   0.2%  14.7Ki    [section .text]
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
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
  56.6%  16.7Mi  16.6%  1.11Mi    third_party/protobuf
  24.9%  7.35Mi  68.5%  4.58Mi    third_party/capstone
   9.4%  2.77Mi   3.2%   221Ki    third_party/re2
   4.6%  1.36Mi   4.1%   280Ki    src
   2.1%   637Ki   1.7%   117Ki    third_party/demumble
   0.7%   209Ki   1.1%  73.8Ki    third_party/abseil
   0.7%   204Ki   3.0%   204Ki    [section .rodata]
   0.2%  56.8Ki   0.8%  56.8Ki    [section .gcc_except_table]
   0.2%  47.7Ki   0.0%       0    [section .debug_str]
   0.2%  46.3Ki   0.0%       0    [section .symtab]
   0.1%  42.0Ki   0.6%  42.0Ki    [section .text]
   0.1%  41.4Ki   0.0%       0    [section .debug_loc]
   0.1%  29.3Ki   0.0%       0    [section .strtab]
   0.0%  12.0Ki   0.2%  11.5Ki    [30 Others]
   0.0%  7.36Ki   0.0%       0    [section .debug_ranges]
   0.0%  6.10Ki   0.1%  6.10Ki    [section .dynstr]
   0.0%  4.99Ki   0.1%  4.99Ki    [section .dynsym]
   0.0%  4.77Ki   0.1%  4.77Ki    [section .eh_frame]
   0.0%  4.59Ki   0.0%       0    [Unmapped]
   0.0%  3.23Ki   0.0%  3.23Ki    [section .plt]
   0.0%  2.50Ki   0.0%       0    [ELF Headers]
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
```

We can get an even richer report by combining the
`bloaty_package` source with the original `compileunits`
source:

```cmdoutput
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  56.6%  16.7Mi  16.6%  1.11Mi    third_party/protobuf
    30.5%  5.08Mi  26.0%   295Ki    third_party/protobuf/src/google/protobuf/descriptor.cc
    12.8%  2.14Mi  15.8%   179Ki    third_party/protobuf/src/google/protobuf/descriptor.pb.cc
     8.2%  1.36Mi   6.9%  78.4Ki    third_party/protobuf/src/google/protobuf/text_format.cc
     5.7%   980Ki   6.6%  75.3Ki    third_party/protobuf/src/google/protobuf/generated_message_reflection.cc
     5.7%   965Ki   3.6%  40.7Ki    third_party/protobuf/src/google/protobuf/descriptor_database.cc
     5.0%   846Ki   5.8%  66.4Ki    third_party/protobuf/src/google/protobuf/extension_set.cc
     4.7%   800Ki   3.6%  41.2Ki    third_party/protobuf/src/google/protobuf/generated_message_util.cc
     4.7%   798Ki   6.1%  69.3Ki    [16 Others]
     4.2%   709Ki   4.5%  50.7Ki    third_party/protobuf/src/google/protobuf/wire_format.cc
     2.9%   503Ki   4.2%  48.1Ki    third_party/protobuf/src/google/protobuf/repeated_field.cc
     2.5%   434Ki   1.4%  15.9Ki    third_party/protobuf/src/google/protobuf/message.cc
     2.4%   407Ki   2.6%  29.9Ki    third_party/protobuf/src/google/protobuf/map_field.cc
     1.8%   309Ki   2.4%  27.5Ki    third_party/protobuf/src/google/protobuf/stubs/strutil.cc
     1.5%   256Ki   0.8%  9.19Ki    third_party/protobuf/src/google/protobuf/dynamic_message.cc
     1.2%   208Ki   1.2%  13.2Ki    third_party/protobuf/src/google/protobuf/extension_set_heavy.cc
     1.2%   206Ki   2.4%  27.4Ki    third_party/protobuf/src/google/protobuf/wire_format_lite.cc
     1.1%   192Ki   1.6%  17.7Ki    third_party/protobuf/src/google/protobuf/parse_context.cc
     1.1%   187Ki   0.8%  9.33Ki    third_party/protobuf/src/google/protobuf/reflection_ops.cc
     1.0%   167Ki   1.2%  13.9Ki    third_party/protobuf/src/google/protobuf/message_lite.cc
     1.0%   165Ki   1.6%  18.7Ki    third_party/protobuf/src/google/protobuf/io/tokenizer.cc
     0.9%   152Ki   0.7%  7.57Ki    third_party/protobuf/src/google/protobuf/unknown_field_set.cc
  24.9%  7.35Mi  68.5%  4.58Mi    third_party/capstone
    17.4%  1.28Mi   6.5%   303Ki    [38 Others]
    14.9%  1.10Mi   6.6%   311Ki    third_party/capstone/arch/ARM/ARMDisassembler.c
     5.3%   399Ki  23.3%  1.07Mi    third_party/capstone/arch/M68K/M68KDisassembler.c
    11.4%   854Ki  17.5%   819Ki    third_party/capstone/arch/X86/X86Mapping.c
     6.2%   469Ki   9.1%   427Ki    third_party/capstone/arch/X86/X86DisassemblerDecoder.c
     4.8%   363Ki   1.3%  59.1Ki    third_party/capstone/arch/SystemZ/SystemZDisassembler.c
     4.4%   329Ki   1.2%  54.2Ki    third_party/capstone/arch/Mips/MipsDisassembler.c
     4.2%   314Ki   1.6%  73.0Ki    third_party/capstone/arch/AArch64/AArch64Disassembler.c
     3.4%   256Ki   3.1%   145Ki    third_party/capstone/arch/AArch64/AArch64InstPrinter.c
     3.2%   243Ki   4.7%   219Ki    third_party/capstone/arch/AArch64/AArch64Mapping.c
     3.2%   241Ki   4.7%   220Ki    third_party/capstone/arch/SystemZ/SystemZMapping.c
     2.9%   219Ki   4.2%   196Ki    third_party/capstone/arch/ARM/ARMMapping.c
     2.7%   205Ki   1.8%  83.3Ki    third_party/capstone/arch/ARM/ARMInstPrinter.c
     2.2%   166Ki   2.0%  95.4Ki    third_party/capstone/arch/PowerPC/PPCInstPrinter.c
     2.0%   153Ki   2.8%   132Ki    third_party/capstone/arch/Mips/MipsMapping.c
     2.0%   153Ki   0.4%  17.7Ki    third_party/capstone/arch/TMS320C64x/TMS320C64xDisassembler.c
     2.0%   151Ki   2.1%  99.0Ki    third_party/capstone/arch/X86/X86ATTInstPrinter.c
     2.0%   149Ki   1.9%  90.6Ki    third_party/capstone/arch/Sparc/SparcInstPrinter.c
     2.0%   148Ki   2.7%   126Ki    third_party/capstone/arch/PowerPC/PPCMapping.c
     1.9%   146Ki   2.1%  96.2Ki    third_party/capstone/arch/X86/X86IntelInstPrinter.c
     1.7%   124Ki   0.6%  28.7Ki    third_party/capstone/arch/PowerPC/PPCDisassembler.c
   9.4%  2.77Mi   3.2%   221Ki    third_party/re2
    14.9%   422Ki  10.6%  23.4Ki    third_party/re2/re2/dfa.cc
    14.4%   407Ki  11.3%  24.9Ki    third_party/re2/re2/regexp.cc
    14.0%   397Ki  11.2%  24.8Ki    third_party/re2/re2/re2.cc
    12.2%   345Ki  10.0%  22.1Ki    third_party/re2/re2/prog.cc
    11.4%   322Ki  33.1%  73.2Ki    third_party/re2/re2/parse.cc
    10.3%   292Ki   8.9%  19.6Ki    third_party/re2/re2/compile.cc
     5.6%   159Ki   3.7%  8.08Ki    third_party/re2/re2/nfa.cc
     4.6%   130Ki   4.7%  10.5Ki    third_party/re2/re2/simplify.cc
     3.7%   106Ki   1.9%  4.19Ki    third_party/re2/re2/onepass.cc
     3.1%  88.7Ki   1.4%  3.08Ki    third_party/re2/re2/bitstate.cc
     2.9%  83.6Ki   1.6%  3.50Ki    third_party/re2/re2/tostring.cc
     1.1%  31.3Ki   0.6%  1.41Ki    third_party/re2/re2/stringpiece.cc
     0.9%  24.3Ki   0.6%  1.22Ki    third_party/re2/util/strutil.cc
     0.6%  16.2Ki   0.0%       0    third_party/re2/re2/unicode_groups.cc
     0.2%  5.36Ki   0.5%  1.09Ki    third_party/re2/util/rune.cc
     0.1%  1.50Ki   0.0%       0    third_party/re2/re2/perl_groups.cc
     0.0%     661   0.0%       0    third_party/re2/re2/unicode_casefold.cc
   4.6%  1.36Mi   4.1%   280Ki    src
    39.4%   549Ki  40.7%   114Ki    src/bloaty.cc
    13.9%   193Ki  15.0%  42.1Ki    src/dwarf.cc
    10.8%   150Ki   0.5%  1.28Ki    src/main.cc
     8.1%   113Ki   8.9%  25.0Ki    src/bloaty.pb.cc
     7.7%   108Ki   9.0%  25.2Ki    src/elf.cc
     7.2%  99.9Ki  10.3%  29.0Ki    src/macho.cc
     4.7%  66.2Ki   7.0%  19.5Ki    src/demangle.cc
     3.5%  49.5Ki   3.8%  10.5Ki    src/webassembly.cc
     2.8%  38.8Ki   2.7%  7.50Ki    src/range_map.cc
     1.9%  26.2Ki   2.1%  5.98Ki    src/disassemble.cc
   2.1%   637Ki   1.7%   117Ki    third_party/demumble
   100.0%   637Ki 100.0%   117Ki    third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
   0.7%   209Ki   1.1%  73.8Ki    third_party/abseil
    19.0%  39.8Ki  19.0%  14.0Ki    third_party/abseil-cpp/absl/strings/internal/charconv_bigint.cc
    15.6%  32.6Ki  13.6%  10.1Ki    third_party/abseil-cpp/absl/strings/escaping.cc
    14.9%  31.1Ki  25.0%  18.5Ki    third_party/abseil-cpp/absl/strings/charconv.cc
    12.3%  25.7Ki  10.6%  7.79Ki    third_party/abseil-cpp/absl/numeric/int128.cc
     8.5%  17.9Ki   9.0%  6.65Ki    third_party/abseil-cpp/absl/strings/numbers.cc
     6.8%  14.3Ki   5.2%  3.87Ki    third_party/abseil-cpp/absl/strings/str_cat.cc
     6.1%  12.7Ki   5.1%  3.75Ki    third_party/abseil-cpp/absl/strings/string_view.cc
     3.6%  7.49Ki   1.7%  1.23Ki    third_party/abseil-cpp/absl/strings/ascii.cc
     2.9%  6.10Ki   3.5%  2.56Ki    third_party/abseil-cpp/absl/strings/internal/charconv_parse.cc
     2.7%  5.75Ki   1.5%  1.13Ki    third_party/abseil-cpp/absl/strings/str_split.cc
     2.3%  4.84Ki   1.9%  1.40Ki    third_party/abseil-cpp/absl/strings/substitute.cc
     1.4%  3.03Ki   1.0%     754    third_party/abseil-cpp/absl/base/internal/raw_logging.cc
     1.1%  2.28Ki   0.4%     302    third_party/abseil-cpp/absl/base/internal/throw_delegate.cc
     0.9%  1.97Ki   1.0%     788    third_party/abseil-cpp/absl/strings/internal/memutil.cc
     0.9%  1.93Ki   0.9%     701    third_party/abseil-cpp/absl/strings/internal/escaping.cc
     0.7%  1.41Ki   0.4%     293    third_party/abseil-cpp/absl/strings/match.cc
     0.3%     556   0.2%     161    third_party/abseil-cpp/absl/strings/internal/utf8.cc
   0.7%   204Ki   3.0%   204Ki    [section .rodata]
   0.2%  56.8Ki   0.8%  56.8Ki    [section .gcc_except_table]
   0.2%  47.7Ki   0.0%       0    [section .debug_str]
   0.2%  46.3Ki   0.0%       0    [section .symtab]
   0.1%  42.0Ki   0.6%  42.0Ki    [section .text]
   0.1%  41.4Ki   0.0%       0    [section .debug_loc]
   0.1%  29.3Ki   0.0%       0    [section .strtab]
   0.0%  12.0Ki   0.2%  11.5Ki    [30 Others]
   0.0%  7.36Ki   0.0%       0    [section .debug_ranges]
   0.0%  6.10Ki   0.1%  6.10Ki    [section .dynstr]
   0.0%  4.99Ki   0.1%  4.99Ki    [section .dynsym]
   0.0%  4.77Ki   0.1%  4.77Ki    [section .eh_frame]
   0.0%  4.59Ki   0.0%       0    [Unmapped]
   0.0%  3.23Ki   0.0%  3.23Ki    [section .plt]
   0.0%  2.50Ki   0.0%       0    [ELF Headers]
 100.0%  29.5Mi 100.0%  6.69Mi    TOTAL
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
$ ./bloaty -c config.bloaty -d bloaty_package,compileunits --source-filter ^src bloaty
    FILE SIZE        VM SIZE    
 --------------  -------------- 
 100.0%  1.36Mi 100.0%   280Ki    src
    39.4%   549Ki  40.7%   114Ki    src/bloaty.cc
    13.9%   193Ki  15.0%  42.1Ki    src/dwarf.cc
    10.8%   150Ki   0.5%  1.28Ki    src/main.cc
     8.1%   113Ki   8.9%  25.0Ki    src/bloaty.pb.cc
     7.7%   108Ki   9.0%  25.2Ki    src/elf.cc
     7.2%  99.9Ki  10.3%  29.0Ki    src/macho.cc
     4.7%  66.2Ki   7.0%  19.5Ki    src/demangle.cc
     3.5%  49.5Ki   3.8%  10.5Ki    src/webassembly.cc
     2.8%  38.8Ki   2.7%  7.50Ki    src/range_map.cc
     1.9%  26.2Ki   2.1%  5.98Ki    src/disassemble.cc
 100.0%  1.36Mi 100.0%   280Ki    TOTAL
Filtering enabled (source_filter); omitted file = 28.1Mi, vm = 6.42Mi of entries
```

