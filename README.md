
# Bloaty McBloatface: a size profiler for binaries

Ever wondered what's making your ELF or Mach-O binary big?
Bloaty McBloatface will show you a size profile of the binary
so you can understand why it's bloated and how to make it
smaller.

## Building Bloaty

To build, simply run:

```
$ make
```

Bloaty depends on RE2, so the Makefile will download it
(via a Git submodule) and build that also.

## Running Bloaty

Run it directly on a binary target.  For example, run it on itself.

```
$ ./bloaty bloaty
```

On Linux you'll see output something like:

```
     VM SIZE                         FILE SIZE
 --------------                  --------------
   0.0%       0 .debug_info        2.40M   36.1%
   0.0%       0 .debug_loc         1.78M   26.9%
   0.0%       0 .debug_str          839k   12.4%
   0.0%       0 .debug_ranges       533k    7.9%
  71.0%    335k .text               335k    4.9%
   0.0%       0 .debug_pubnames     259k    3.8%
   0.0%       0 .debug_line         195k    2.9%
   0.0%       0 .debug_abbrev      74.3k    1.1%
  14.5%   68.5k .rodata            68.5k    1.0%
   0.0%       0 .strtab            46.4k    0.7%
   0.0%       0 .debug_pubtypes    44.7k    0.7%
   6.5%   30.5k .eh_frame          30.5k    0.4%
   0.0%       0 .symtab            29.6k    0.4%
   2.8%   13.1k .gcc_except_table  13.1k    0.2%
   0.0%       0 .debug_aranges     11.8k    0.2%
   1.8%   8.61k [Other]            8.10k    0.1%
   1.0%   4.70k .eh_frame_hdr      4.70k    0.1%
   0.8%   3.89k .dynsym            3.89k    0.1%
   0.8%   3.61k .dynstr            3.61k    0.1%
   0.7%   3.38k .rela.plt          3.38k    0.0%
   0.1%     612 [None]             3.24k    0.0%
 100.0%    472k TOTAL              6.63M  100.0%
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

## Hierarchical Profiles

Bloaty supports breaking the binary down in lots of
different ways.  You can combine multiple data sources into
a single hierarchical profile.  For example, we can use the
`segments` and `sections` data sources in a single report:

```
$ bloaty -d segments,sections bloaty
     VM SIZE                              FILE SIZE
 --------------                       --------------
   0.0%       0 [None]                  6.17M   93.1%
       0.0%       0 .debug_info             2.40M   38.8%
       0.0%       0 .debug_loc              1.78M   28.9%
       0.0%       0 .debug_str               839k   13.3%
       0.0%       0 .debug_ranges            533k    8.4%
       0.0%       0 .debug_pubnames          259k    4.1%
       0.0%       0 .debug_line              195k    3.1%
       0.0%       0 .debug_abbrev           74.3k    1.2%
       0.0%       0 .strtab                 46.4k    0.7%
       0.0%       0 .debug_pubtypes         44.7k    0.7%
       0.0%       0 .symtab                 29.6k    0.5%
       0.0%       0 .debug_aranges          11.8k    0.2%
       0.0%       0 [None]                  2.65k    0.0%
       0.0%       0 .shstrtab                 403    0.0%
       0.0%       0 .comment                  199    0.0%
  98.9%    467k LOAD [R E]               467k    6.9%
      71.8%    335k .text                    335k   71.8%
      14.7%   68.5k .rodata                 68.5k   14.7%
       6.5%   30.5k .eh_frame               30.5k    6.5%
       2.8%   13.1k .gcc_except_table       13.1k    2.8%
       1.0%   4.70k .eh_frame_hdr           4.70k    1.0%
       0.8%   3.89k .dynsym                 3.89k    0.8%
       0.8%   3.61k .dynstr                 3.61k    0.8%
       0.7%   3.38k .rela.plt               3.38k    0.7%
       0.5%   2.27k .plt                    2.27k    0.5%
       0.1%     600 [None]                    600    0.1%
       0.1%     360 .rela.dyn                 360    0.1%
       0.1%     332 .gnu.version              332    0.1%
       0.1%     304 .gnu.version_r            304    0.1%
       0.0%     212 .gnu.hash                 212    0.0%
       0.0%      36 .note.gnu.build-id         36    0.0%
       0.0%      32 .note.ABI-tag              32    0.0%
       0.0%      28 .interp                    28    0.0%
       0.0%      26 .init                      26    0.0%
       0.0%       9 .fini                       9    0.0%
   1.1%   5.05k LOAD [RW ]              3.95k    0.1%
      43.3%   2.18k .data                   2.18k   55.3%
      22.8%   1.15k .got.plt                1.15k   29.1%
      21.7%   1.09k .bss                        0    0.0%
      10.5%     544 .dynamic                  544   13.5%
       0.8%      40 .init_array                40    1.0%
       0.5%      24 .got                       24    0.6%
       0.2%      12 [None]                      8    0.2%
       0.2%       8 .jcr                        8    0.2%
       0.2%       8 .fini_array                 8    0.2%
 100.0%    472k TOTAL                   6.63M  100.0%
```

Bloaty displays a maximum of 20 lines for each level; other
values are grouped into an `[Other]` bin.  TODO: make this a
parameter.

# Data Sources

Bloaty has many data sources built in.  It's easy to add a
new data source if you have a new way of mapping address
ranges to some interesting higher-level abstraction.

## Segments

Segments are what the run-time loader uses to determine what
parts of the binary need to be loaded/mapped into memory.
There are usually just a few segments: one for each set of
`mmap()` permissions required:

```
$ bloaty -d segments bloaty
     VM SIZE                  FILE SIZE
 --------------           --------------
   0.0%       0 [None]      6.17M   93.1%
  98.9%    467k LOAD [R E]   467k    6.9%
   1.1%   5.05k LOAD [RW ]  3.95k    0.1%
 100.0%    472k TOTAL       6.63M  100.0%
```

Here we see one segment mapped `[R E]` (read/execute) and
one segment mapped `[RW ]` (read/write).  A large part of
the binary is not loaded into memory, which we see as
`[None]`.

## Sections

Sections give us a bit more granular look into the binary.
If we want to find the symbol table, the unwind information,
or the debug information, each kind of information lives in
its own section.  Bloaty's default output is sections.

```
$ bloaty -d sections bloaty
     VM SIZE                         FILE SIZE
 --------------                  --------------
   0.0%       0 .debug_info        2.40M   36.1%
   0.0%       0 .debug_loc         1.78M   26.9%
   0.0%       0 .debug_str          839k   12.4%
   0.0%       0 .debug_ranges       533k    7.9%
  71.0%    335k .text               335k    4.9%
   0.0%       0 .debug_pubnames     259k    3.8%
   0.0%       0 .debug_line         195k    2.9%
   0.0%       0 .debug_abbrev      74.3k    1.1%
  14.5%   68.5k .rodata            68.5k    1.0%
   0.0%       0 .strtab            46.4k    0.7%
   0.0%       0 .debug_pubtypes    44.7k    0.7%
   6.5%   30.5k .eh_frame          30.5k    0.4%
   0.0%       0 .symtab            29.6k    0.4%
   2.8%   13.1k .gcc_except_table  13.1k    0.2%
   0.0%       0 .debug_aranges     11.8k    0.2%
   1.8%   8.61k [Other]            8.10k    0.1%
   1.0%   4.70k .eh_frame_hdr      4.70k    0.1%
   0.8%   3.89k .dynsym            3.89k    0.1%
   0.8%   3.61k .dynstr            3.61k    0.1%
   0.7%   3.38k .rela.plt          3.38k    0.0%
   0.1%     612 [None]             3.24k    0.0%
 100.0%    472k TOTAL              6.63M  100.0%
```

## Symbols

Symbols come from the symbol table, and represent individual
functions or variables.  C++ symbols are demangled for
convenience.

```
$ bloaty -d symbols
     VM SIZE                                               FILE SIZE
 --------------                                        --------------
  17.7%   83.5k [None]                                   6.25M   94.3%
  60.9%    287k [Other]                                   286k    4.2%
   2.8%   13.1k GCC_except_table2                        13.1k    0.2%
   2.6%   12.3k re2::RE2::Match                          12.3k    0.2%
   1.7%   7.83k re2::unicode_groups                      7.83k    0.1%
   1.6%   7.56k re2::NFA::Search                         7.56k    0.1%
   1.2%   5.76k re2::BitState::TrySearch                 5.76k    0.1%
   1.0%   4.81k _dwarf_exec_frame_instr                  4.81k    0.1%
   0.9%   4.49k re2::DFA::DFA                            4.49k    0.1%
   0.9%   4.34k re2::Regexp::Parse                       4.34k    0.1%
   0.9%   4.20k re2::RE2::Init                           4.20k    0.1%
   0.9%   4.09k re2::Prog::IsOnePass                     4.09k    0.1%
   0.9%   4.04k re2::Compiler::PostVisit                 4.04k    0.1%
   0.8%   4.01k _dwarf_internal_srclines                 4.01k    0.1%
   0.8%   3.91k re2::Regexp::FactorAlternationRecursive  3.91k    0.1%
   0.8%   3.77k re2::DFA::RunStateOnByte                 3.77k    0.1%
   0.8%   3.68k re2::unicode_casefold                    3.68k    0.1%
   0.7%   3.40k main                                     3.40k    0.1%
   0.7%   3.40k re2::DFA::InlinedSearchLoop              3.40k    0.1%
   0.7%   3.38k re2::DFA::InlinedSearchLoop              3.38k    0.0%
   0.7%   3.37k bloaty::ReadELFSymbols                   3.37k    0.0%
 100.0%    472k TOTAL                                    6.63M  100.0%
```

## Source Files

Using debug information, we can tell what source file each
bit of the binary came from.  Specifically, this uses the
"address ranges" or "aranges" information from DWARF.  It's
not perfect and sometimes you'll see some of the binary show
up as `[None]` if it's not mentioned in aranges.  But it can
tell us a lot.

```
$ ./bloaty -d sourcefiles bloaty
     VM SIZE                            FILE SIZE
 --------------                     --------------
  42.5%    200k [None]                6.36M   96.0%
   8.5%   40.1k bloaty.cc             40.0k    0.6%
   6.9%   32.5k re2/re2.cc            32.5k    0.5%
   6.7%   31.6k re2/dfa.cc            31.6k    0.5%
   6.6%   31.4k re2/parse.cc          31.4k    0.5%
   5.9%   27.8k re2/regexp.cc         27.8k    0.4%
   5.0%   23.7k re2/compile.cc        23.7k    0.3%
   4.2%   19.7k re2/simplify.cc       19.7k    0.3%
   3.0%   14.2k re2/nfa.cc            14.2k    0.2%
   2.3%   10.6k elf.cc                10.6k    0.2%
   1.8%   8.34k re2/bitstate.cc       8.34k    0.1%
   1.7%   7.84k re2/prog.cc           7.84k    0.1%
   1.5%   7.16k dwarf.cc              7.16k    0.1%
   1.5%   7.13k re2/tostring.cc       7.13k    0.1%
   1.4%   6.67k re2/onepass.cc        6.67k    0.1%
   0.1%     666 util/stringprintf.cc    666    0.0%
   0.1%     590 macho.cc                590    0.0%
   0.1%     573 util/strutil.cc         573    0.0%
   0.1%     476 util/rune.cc            476    0.0%
   0.1%     414 re2/stringpiece.cc      414    0.0%
   0.1%     383 [Other]                 383    0.0%
 100.0%    472k TOTAL                 6.63M  100.0%
```

## Source Lines

The DWARF debugging information also contains "source line"
information that understands inlining.  So within a
function, it will know which instructions came from an
inlined function from a header file.  This is the
information the debugger uses to point at a specific source
line as you're tracing through a program.

There are two variations of this data source.  One shows
source file and line together, and one shows just source
files:

```
$ ./bloaty -d lineinfo bloaty
     VM SIZE                                                                                        FILE SIZE
 --------------                                                                                 --------------
  41.8%    197k [None]                                                                            6.36M   96.0%
  11.8%   55.7k /usr/include/c++/4.8/bits/char_traits.h:243                                       55.7k    0.8%
  11.5%   54.2k /usr/local/google/home/haberman/code/bloaty/third_party/re2/./util/sparse_set.h:  54.2k    0.8%
   9.7%   46.0k [Other]                                                                           46.0k    0.7%
   9.4%   44.5k /usr/include/c++/4.8/ext/atomicity.h:84                                           44.5k    0.7%
   8.7%   40.9k /usr/local/google/home/haberman/code/bloaty/bloaty.cc:134                         40.9k    0.6%
   3.8%   17.7k /usr/bin/../lib/gcc/x86_64-linux-gnu/4.8/../../../../include/c++/4.8/bits/char_t  17.7k    0.3%
   0.5%   2.26k /usr/include/c++/4.8/bits/basic_ios.h:456                                         2.26k    0.0%
   0.5%   2.21k /usr/include/c++/4.8/ostream:535                                                  2.21k    0.0%
   0.4%   1.77k /usr/include/c++/4.8/streambuf:466                                                1.77k    0.0%
   0.4%   1.67k /usr/include/c++/4.8/bits/basic_string.h:539                                      1.67k    0.0%
   0.2%   1.04k /usr/include/c++/4.8/bits/basic_ios.h:276                                         1.04k    0.0%
   0.2%     877 /usr/include/c++/4.8/ostream:385                                                    877    0.0%
   0.2%     864 /usr/include/c++/4.8/bits/basic_string.h:1009                                       864    0.0%
   0.2%     864 /usr/include/c++/4.8/bits/basic_string.h:583                                        864    0.0%
   0.2%     803 /usr/include/c++/4.8/bits/basic_string.h:249                                        803    0.0%
   0.2%     726 /usr/include/c++/4.8/sstream:424                                                    726    0.0%
   0.1%     721 /usr/include/c++/4.8/bits/char_traits.h:271                                         721    0.0%
   0.1%     678 /usr/include/c++/4.8/bits/basic_string.h:275                                        678    0.0%
   0.1%     666 /usr/include/c++/4.8/ostream:93                                                     666    0.0%
   0.1%     650 /usr/local/google/home/haberman/code/bloaty/third_party/re2/./util/logging.h:62     650    0.0%
 100.0%    472k TOTAL                                                                             6.63M  100.0%
```

For files only:

```
$ ./bloaty -d lineinfo:file
     VM SIZE                                                                                        FILE SIZE
 --------------                                                                                 --------------
  41.9%    198k [None]                                                                            6.36M   96.0%
  11.9%   56.4k /usr/local/google/home/haberman/code/bloaty/third_party/re2/./util/sparse_set.h   56.4k    0.8%
   8.7%   41.0k /usr/local/google/home/haberman/code/bloaty/bloaty.cc                             41.0k    0.6%
   6.2%   29.3k [Other]                                                                           29.3k    0.4%
   4.8%   22.6k /usr/include/c++/4.8/bits/basic_string.h                                          22.6k    0.3%
   4.0%   18.8k /usr/include/c++/4.8/ext/atomicity.h                                              18.8k    0.3%
   3.8%   17.7k /usr/bin/../lib/gcc/x86_64-linux-gnu/4.8/../../../../include/c++/4.8/bits/char_t  17.7k    0.3%
   2.2%   10.4k /usr/local/google/home/haberman/code/bloaty/third_party/re2/re2/compile.cc        10.4k    0.2%
   2.1%   10.1k /usr/include/c++/4.8/ostream                                                      10.1k    0.1%
   1.9%   8.97k /usr/local/google/home/haberman/code/bloaty/third_party/re2/re2/dfa.cc            8.97k    0.1%
   1.7%   7.98k /usr/include/c++/4.8/bits/basic_ios.h                                             7.98k    0.1%
   1.5%   6.91k /usr/include/c++/4.8/sstream                                                      6.91k    0.1%
   1.4%   6.46k /usr/local/google/home/haberman/code/bloaty/third_party/re2/re2/re2.cc            6.46k    0.1%
   1.2%   5.86k /usr/local/google/home/haberman/code/bloaty/third_party/re2/./util/logging.h      5.86k    0.1%
   1.2%   5.51k /usr/include/c++/4.8/bits/stl_tree.h                                              5.51k    0.1%
   1.1%   5.39k /usr/include/c++/4.8/streambuf                                                    5.39k    0.1%
   1.1%   5.22k /usr/include/c++/4.8/bits/basic_string.tcc                                        5.22k    0.1%
   1.0%   4.49k /usr/include/c++/4.8/bits/stl_deque.h                                             4.49k    0.1%
   0.8%   3.85k /usr/local/google/home/haberman/code/bloaty/third_party/re2/re2/simplify.cc       3.85k    0.1%
   0.8%   3.83k /usr/local/google/home/haberman/code/bloaty/third_party/re2/./re2/walker-inl.h    3.83k    0.1%
   0.7%   3.38k /usr/include/c++/4.8/bits/char_traits.h                                           3.38k    0.0%
 100.0%    472k TOTAL                                                                             6.63M  100.0%
```

# Using Regular Expressions

You can filter the lists by using regular expressions.  For
example, to view by source file by group all re2-related
sources together, you can write:

```
$ ./bloaty -d sourcefiles bloaty -r 'sourcefiles+=/.*re2.*/RE2/'
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

## Size Diffs

If the program or toolchain changed, a user might want to
view a size *diff* between two programs.  To support this
the program should allow setting a baseline program.  Then
everything showed in the tally would be a diff instead of
an absolute size.

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

We will probably also want to let the user optionally map
symbols to higher-level abstractions, like rules in their
build language.  I've prototyped this in earlier CLs.
