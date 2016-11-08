
# Bloaty McBloatface: a size profiler for binaries

[![Build Status](https://travis-ci.org/google/bloaty.svg?branch=master)](https://travis-ci.org/google/bloaty)

Ever wondered what's making your ELF or Mach-O binary big?
Bloaty McBloatface will show you a size profile of the binary
so you can understand what's taking up space inside.

Bloaty works on binaries, shared objects, object files, and
static libraries (`.a` files).  It supports ELF/DWARF and
Mach-O, though the Mach-O support is much more preliminary
(it shells out to `otool`/`symbols` instead of parsing the
file directly).

This is not an official Google product.

## Building Bloaty

To build, simply run:

```
$ make
```

Bloaty depends on RE2, so the Makefile will download it
(via a Git submodule) and build that also.

To run the tests (requires that `cmake` is installed and
available on your path) run:

```
$ make test
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
     VM SIZE                            FILE SIZE
 --------------                      --------------
  27.9%   128Ki [None]                7.43Mi  95.9%
  12.9%  59.2Ki src/bloaty.cc         59.0Ki   0.7%
   7.3%  33.4Ki re2/re2.cc            32.3Ki   0.4%
   6.9%  31.6Ki re2/dfa.cc            31.6Ki   0.4%
   6.8%  31.4Ki re2/parse.cc          31.4Ki   0.4%
   6.7%  30.9Ki src/dwarf.cc          30.9Ki   0.4%
   6.7%  30.6Ki re2/regexp.cc         27.8Ki   0.4%
   5.1%  23.7Ki re2/compile.cc        23.7Ki   0.3%
   4.3%  19.7Ki re2/simplify.cc       19.7Ki   0.2%
   3.2%  14.8Ki src/elf.cc            14.8Ki   0.2%
   3.1%  14.2Ki re2/nfa.cc            14.2Ki   0.2%
   1.8%  8.34Ki re2/bitstate.cc       8.34Ki   0.1%
   1.7%  7.84Ki re2/prog.cc           7.84Ki   0.1%
   1.6%  7.13Ki re2/tostring.cc       7.13Ki   0.1%
   1.5%  6.67Ki re2/onepass.cc        6.67Ki   0.1%
   1.4%  6.58Ki src/macho.cc          6.58Ki   0.1%
   0.7%  3.27Ki src/main.cc           3.27Ki   0.0%
   0.2%     797 [Other]                  797   0.0%
   0.1%     666 util/stringprintf.cc     666   0.0%
   0.1%     573 util/strutil.cc          573   0.0%
   0.1%     476 util/rune.cc             476   0.0%
 100.0%   460Ki TOTAL                 7.75Mi 100.0%
```


Run Bloaty with `--help` to see a list of available options:

```
$ ./bloaty --help
Bloaty McBloatface: a size profiler for binaries.

USAGE: bloaty [options] file... [-- base_file...]

Options:

  -d <sources>     Comma-separated list of sources to scan.
  -n <num>         How many rows to show per level before collapsing
                   other keys into '[Other]'.  Set to '0' for unlimited.
                   Defaults to 20.
  -r <regex>       Add regex to the list of regexes.
                   Format for regex is:
                     SOURCE:s/PATTERN/REPLACEMENT/
  -s <sortby>      Whether to sort by VM or File size.  Possible values
                   are:
                     -s vm
                     -s file
                     -s both (the default: sorts by max(vm, file)).
  -v               Verbose output.  Dumps warnings encountered during
                   processing and full VM/file maps at the end.
                   Add more v's (-vv, -vvv) for even more.
  --help           Display this message and exit.
  --list-sources   Show a list of available sources and exit.
```

## Size Diffs

You can use Bloaty to see how the size of a binary changed.
On the command-line, pass `--` followed by the files you
want to use as the diff base.

For example, here is a size diff between a couple different versions
of Bloaty, showing how it grew when I added some features.

```
$ ./bloaty bloaty -- oldbloaty
     VM SIZE                         FILE SIZE
 ++++++++++++++ GROWING           ++++++++++++++
  [ = ]       0 .debug_str        +41.2Ki  +5.0%
  [ = ]       0 .debug_info       +36.8Ki  +1.3%
  [ = ]       0 .debug_loc        +12.4Ki  +0.6%
  +1.8% +6.12Ki .text             +6.12Ki  +1.8%
  [ = ]       0 .debug_ranges     +4.47Ki  +0.8%
  [ = ]       0 .debug_line       +2.69Ki  +1.3%
  [ = ]       0 .strtab           +1.52Ki  +3.1%
  +3.9% +1.32Ki .eh_frame         +1.32Ki  +3.9%
  +1.6% +1.12Ki .rodata           +1.12Ki  +1.6%
  [ = ]       0 .symtab              +696  +2.3%
  [ = ]       0 .debug_aranges       +288  +2.4%
  +2.7%    +272 .gcc_except_table    +272  +2.7%
  +2.7%    +136 .eh_frame_hdr        +136  +2.7%
  +1.2%     +48 .dynsym               +48  +1.2%
  +1.4%     +48 .rela.plt             +48  +1.4%
  +1.4%     +32 .plt                  +32  +1.4%
  +0.6%     +22 .dynstr               +22  +0.6%
  +1.3%     +16 .got.plt              +16  +1.3%
  +1.2%      +4 .gnu.version           +4  +1.2%

 -------------- SHRINKING         --------------
 -18.5%     -10 [Unmapped]        -1.14Ki -31.4%
  [ = ]       0 .debug_abbrev         -72  -0.1%

  +1.9% +9.12Ki TOTAL              +107Ki  +1.5%
```

Each line shows the how much each part changed compared to
its previous size.  The "TOTAL" line shows how much the size
changed overall.

## Hierarchical Profiles

Bloaty supports breaking the binary down in lots of
different ways.  You can combine multiple data sources into
a single hierarchical profile.  For example, we can use the
`segments` and `sections` data sources in a single report:

```
$ bloaty -d segments,sections bloaty
      VM SIZE                              FILE SIZE
 --------------                        --------------
   0.0%       0 [Unmapped]              7.31Mi  94.2%
      -NAN%       0 .debug_info             2.97Mi  40.6%
      -NAN%       0 .debug_loc              2.30Mi  31.5%
      -NAN%       0 .debug_str              1.03Mi  14.2%
      -NAN%       0 .debug_ranges            611Ki   8.2%
      -NAN%       0 .debug_line              218Ki   2.9%
      -NAN%       0 .debug_abbrev           85.4Ki   1.1%
      -NAN%       0 .strtab                 62.8Ki   0.8%
      -NAN%       0 .symtab                 27.8Ki   0.4%
      -NAN%       0 .debug_aranges          13.5Ki   0.2%
      -NAN%       0 [Unmapped]              2.82Ki   0.0%
      -NAN%       0 .shstrtab                  371   0.0%
      -NAN%       0 .comment                    43   0.0%
  99.2%   452Ki LOAD [RX]                452Ki   5.7%
      73.4%   332Ki .text                    332Ki  73.4%
      13.3%  60.0Ki .rodata                 60.0Ki  13.3%
       7.0%  31.8Ki .eh_frame               31.8Ki   7.0%
       2.3%  10.5Ki .gcc_except_table       10.5Ki   2.3%
       0.9%  4.18Ki .eh_frame_hdr           4.18Ki   0.9%
       0.8%  3.54Ki .dynsym                 3.54Ki   0.8%
       0.8%  3.52Ki .dynstr                 3.52Ki   0.8%
       0.7%  2.98Ki .rela.plt               2.98Ki   0.7%
       0.4%  2.00Ki .plt                    2.00Ki   0.4%
       0.1%     568 [ELF Headers]              568   0.1%
       0.1%     408 .rela.dyn                  408   0.1%
       0.1%     304 .gnu.version_r             304   0.1%
       0.1%     302 .gnu.version               302   0.1%
       0.0%     216 .gnu.hash                  216   0.0%
       0.0%      36 .note.gnu.build-id          36   0.0%
       0.0%      32 .note.ABI-tag               32   0.0%
       0.0%      28 .interp                     28   0.0%
       0.0%      26 .init                       26   0.0%
       0.0%      18 [Unmapped]                  18   0.0%
       0.0%       9 .fini                        9   0.0%
   0.8%  3.46Ki LOAD [RW]               1.88Ki   0.0%
      45.6%  1.58Ki .bss                         0   0.0%
      29.3%  1.02Ki .got.plt                1.02Ki  54.1%
      14.9%     528 .dynamic                   528  27.4%
       7.1%     252 .data                      252  13.1%
       1.4%      48 .init_array                 48   2.5%
       0.7%      24 .got                        24   1.2%
       0.5%      16 [Unmapped]                  16   0.8%
       0.2%       8 .fini_array                  8   0.4%
       0.2%       8 .jcr                         8   0.4%
       0.1%       4 [None]                       0   0.0%
   0.0%       0 [ELF Headers]           2.38Ki   0.0%
 100.0%   456Ki TOTAL                   7.75Mi 100.0%
```

Bloaty displays a maximum of 20 lines for each level; other
values are grouped into an `[Other]` bin.  Use `-n <num>`
to override this setting.  If you pass `-n 0`, all data
will be output without collapsing anything into `[Other]`.

# Data Sources

Bloaty has many data sources built in.  It's easy to add a
new data source if you have a new way of mapping address
ranges to some interesting higher-level abstraction.

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
functions or variables.  C++ symbols are demangled for
convenience.

```
$ ./bloaty -d symbols bloaty
      VM SIZE                                                                                        FILE SIZE
 --------------                                                                                  --------------
  17.9%  81.9Ki [Unmapped]                                                                        7.39Mi  95.3%
  62.3%   283Ki [Other]                                                                            284Ki   3.6%
   2.7%  12.3Ki re2::RE2::Match(re2::StringPiece const&, int, int, re2::RE2::Anchor, re2::String  12.3Ki   0.2%
   1.7%  7.83Ki re2::unicode_groups                                                               7.83Ki   0.1%
   1.7%  7.56Ki re2::NFA::Search                                                                  7.56Ki   0.1%
   1.3%  5.76Ki re2::BitState::TrySearch                                                          5.76Ki   0.1%
   1.2%  5.43Ki bloaty::Bloaty::ScanAndRollupFile                                                 5.43Ki   0.1%
   1.0%  4.49Ki re2::DFA::DFA                                                                     4.49Ki   0.1%
   1.0%  4.35Ki bool bloaty::(anonymous namespace)::ForEachElf<bloaty::(anonymous namespace)::Do  4.35Ki   0.1%
   1.0%  4.34Ki re2::Regexp::Parse                                                                4.34Ki   0.1%
   0.9%  4.20Ki re2::RE2::Init                                                                    4.20Ki   0.1%
   0.9%  4.09Ki re2::Prog::IsOnePass                                                              4.09Ki   0.1%
   0.9%  4.04Ki re2::Compiler::PostVisit                                                          4.04Ki   0.1%
   0.9%  4.04Ki bloaty::ReadDWARFInlines                                                          4.04Ki   0.1%
   0.9%  3.91Ki re2::Regexp::FactorAlternationRecursive                                           3.91Ki   0.0%
   0.8%  3.77Ki re2::DFA::RunStateOnByte                                                          3.77Ki   0.0%
   0.8%  3.68Ki re2::unicode_casefold                                                             3.68Ki   0.0%
   0.8%  3.52Ki bloaty::ElfFileHandler::ProcessFile                                               3.52Ki   0.0%
   0.7%  3.40Ki re2::DFA::InlinedSearchLoop(re2::DFA::SearchParams*, bool, bool, bool) [clone .c  3.40Ki   0.0%
   0.7%  3.38Ki re2::DFA::InlinedSearchLoop(re2::DFA::SearchParams*, bool, bool, bool) [clone .c  3.38Ki   0.0%
   0.0%     165 [None]                                                                                 0   0.0%
 100.0%   456Ki TOTAL                                                                             7.75Mi 100.0%
```

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
     VM SIZE                            FILE SIZE
 --------------                      --------------
  27.9%   128Ki [None]                7.43Mi  95.9%
  12.9%  59.2Ki src/bloaty.cc         59.0Ki   0.7%
   7.3%  33.4Ki re2/re2.cc            32.3Ki   0.4%
   6.9%  31.6Ki re2/dfa.cc            31.6Ki   0.4%
   6.8%  31.4Ki re2/parse.cc          31.4Ki   0.4%
   6.7%  30.9Ki src/dwarf.cc          30.9Ki   0.4%
   6.7%  30.6Ki re2/regexp.cc         27.8Ki   0.4%
   5.1%  23.7Ki re2/compile.cc        23.7Ki   0.3%
   4.3%  19.7Ki re2/simplify.cc       19.7Ki   0.2%
   3.2%  14.8Ki src/elf.cc            14.8Ki   0.2%
   3.1%  14.2Ki re2/nfa.cc            14.2Ki   0.2%
   1.8%  8.34Ki re2/bitstate.cc       8.34Ki   0.1%
   1.7%  7.84Ki re2/prog.cc           7.84Ki   0.1%
   1.6%  7.13Ki re2/tostring.cc       7.13Ki   0.1%
   1.5%  6.67Ki re2/onepass.cc        6.67Ki   0.1%
   1.4%  6.58Ki src/macho.cc          6.58Ki   0.1%
   0.7%  3.27Ki src/main.cc           3.27Ki   0.0%
   0.2%     797 [Other]                  797   0.0%
   0.1%     666 util/stringprintf.cc     666   0.0%
   0.1%     573 util/strutil.cc          573   0.0%
   0.1%     476 util/rune.cc             476   0.0%
 100.0%   460Ki TOTAL                 7.75Mi 100.0%
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

# Using Regular Expressions

You can filter the lists by using regular expressions.  For
example, to view by source file by group all re2-related
sources together, you can write:

(TODO: this appears to be broken at the moment, needs
fixing!)

```
$ ./bloaty -d inlines bloaty -r 'inlines:s/.*re2.*/RE2/'
     VM SIZE                            FILE SIZE
 --------------                     --------------
  42.5%    200k [None]                6.36M   96.0%
  44.7%    211k RE2                    211k    3.1%
   8.5%   40.1k bloaty.cc             40.0k    0.6%
   2.3%   10.6k elf.cc                10.6k    0.2%
   1.5%   7.16k dwarf.cc              7.16k    0.1%
   0.1%     666 util/stringprintf.cc    666    0.0%
   0.1%     590 macho.cc                590    0.0%
   0.1%     573 util/strutil.cc         573    0.0%
   0.1%     476 util/rune.cc            476    0.0%
   0.1%     287 util/hash.cc            287    0.0%
   0.0%      96 util/valgrind.cc         96    0.0%
 100.0%    472k TOTAL                 6.63M  100.0%
```

Note: this functionality is a bit under-developed and
subject to change.  For example, there is not yet a way to
escape backslashes.

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

## Improving the quality of data sources

One of the things we have to do in Bloaty is deal with
incomplete information.  For examples, `.debug_aranges`
which we use for the `compileunits` data source is often
missing or incomplete.  Refining the input sources to be
more complete and accurate will make help Bloaty's numbers
be even more accurate.
