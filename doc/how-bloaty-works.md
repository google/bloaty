
# How Bloaty Works

At a high level, Bloaty's goal is to create a map of the binary where every
byte has a label attached to it.  Every byte starts out as unknown
(unattributed).  As we scan the binary we assign labels to different ranges of
the file.  For example, if the user selected the "sections" data source we scan
the section table and use the section name as the label for each range.

Ideally these labeled ranges will cover the entire file by the time we are
done.  In practice we usually can't achieve perfect 100% coverage.  To
compensate for this, we have various kinds of "fallback" labels we attach to
mystery regions of the file.  This is how we guarantee an important invariant
of Bloaty: the totals given in Bloaty's output will always match the total size
of the file.  This ensures that we always account for the entire file, even if
we don't have detailed information for every byte.

The ELF/Mach-O/etc. data structures we are traversing were not designed to
enable size profiling.  They were designed to assist linkers, loaders,
debuggers, stack unwinders, etc. to run and debug the binary.  This means that
Bloaty's size analysis is inherently an unconventional use of ELF/Mach-O
metadata.  Bloaty has to be clever about how to use the available information to
achieve its goal.  This can pose a challenge, but also makes Bloaty fun to work
on.  Getting the coverage close to 100% requires a lot of ingenuity (and some
heuristics).

## Range Map

RangeMap (as defined in [range_map.h](https://github.com/google/bloaty/blob/master/src/range_map.h))
is the core data structure of Bloaty.  It is a sparse map of
`[start, end) -> std::string` that associates regions of VM or file space to
a label.

By the time Bloaty is finished, it has built a complete map of both VM and file
space for the binary. You can view these maps by running Bloaty with '-v':

```
$ ./bloaty bloaty -v -d sections
FILE MAP:
0000000-00002e0         736             [LOAD #2 [R]]
00002e0-00002fc          28             .interp
00002fc-0000320          36             .note.gnu.build-id
0000320-0000340          32             .note.ABI-tag
0000340-0000510         464             .gnu.hash
0000510-0001db8        6312             .dynsym
0001db8-0003c8d        7893             .dynstr
0003c8d-0003c8e           1             [LOAD #2 [R]]
0003c8e-0003e9c         526             .gnu.version
0003e9c-0003ea0           4             [LOAD #2 [R]]
0003ea0-0004020         384             .gnu.version_r
0004020-0066f30      405264             .rela.dyn
0066f30-00680b8        4488             .rela.plt
00680b8-0069000        3912             [Unmapped]
0069000-0069017          23             .init
0069017-0069020           9             [LOAD #3 [RX]]
0069020-0069be0        3008             .plt
0069be0-0069c40          96             .plt.got
0069c40-02874d1     2218129             .text
[...]

VM MAP:
000000-0002e0           736             [LOAD #2 [R]]
0002e0-0002fc            28             .interp
0002fc-000320            36             .note.gnu.build-id
000320-000340            32             .note.ABI-tag
000340-000510           464             .gnu.hash
000510-001db8          6312             .dynsym
001db8-003c8d          7893             .dynstr
003c8d-003c8e             1             [LOAD #2 [R]]
003c8e-003e9c           526             .gnu.version
003e9c-003ea0             4             [LOAD #2 [R]]
003ea0-004020           384             .gnu.version_r
004020-066f30        405264             .rela.dyn
066f30-0680b8          4488             .rela.plt
0680b8-069000          3912             [-- Nothing mapped --]
069000-069017            23             .init
069017-069020             9             [LOAD #3 [RX]]
[...]
```

The file map refers to file offsets, and these always run from 0 to the size of
the file. The VM map refers to VM addresses; these start at 0 for shared
libraries and position-independent binaries, but these will start at some
non-zero address if the binary was linked to be loaded at a fixed address.

Note that some of the regions in the map have labels like `[LOAD #2 [R]]`
instead of a true section name. This is because the section table does not
always cover every byte of the file. Bloaty gives these regions a fallback label
that contains the segment name instead. We must attach some kind of label to
every byte of the file, otherwise Bloaty's totals would not match the file size.

Also notice that there is an entry in the VM map that says `[-- Nothing mapped
--]`. This is calling attention to the fact that there is a gap in the address
space here. Since nothing is mapped, these regions of the VM space don't
actually need to accessible in the target process image. However, unless this
unused space aligns with page boundaries, it will probably end up getting
mapped anyway.

Sometimes we know a region's start but not its end.  For example, Mach-O
symbols have an address but not a size (whereas ELF symbols have both).
To support this case, `RangeMap` supports adding an address with `kUnknownSize`.
A range with unknown size will automatically extend to the beginning of the
next region, even if the next region is added later.

If we try to add a label to a range of the binary that has already been assigned
a label, the first label assigned takes precedence.  This means that the order
in which we scan data structures is significant.  So our general strategy is to
scan our most granular and detailed information first.  We scan generic
information as a last resort, to give at least some information for parts of the
binary that we couldn't find any more specific information about.

## VM Space and File Space

Loadable binaries have two fundamental domains of space we are trying to map:
*VM space* and *file space*.  File space is the bytes of the input file.  VM
space is the bytes of memory when the executable is loaded at runtime.  Some
regions of the binary exist only in file space (like debug info) and some
regions exist only in VM space (like `.bss`, zero-initialized data).  Even
entities that exist in both spaces can have different sizes in each.

We create two separate `RangeMap` structures for these two domains.  For
convenience, we put them together into a single structure called `DualMap`:

```cpp
struct DualMap {
  RangeMap vm_map;
  RangeMap file_map;
};
```

We populate these two maps simultaneously as we scan the file.  We must populate
both maps even if we only care about one of them, because most of the metadata we're
scanning gives us VM addresses *or* file offsets, not both.  For example,
debug info always refers to VM addresses, because it's intended for debugging at
runtime.  Even if we only care about file size, we still have to scan VM addresses
and translate them to file offsets.

Bloaty's overall analysis algorithm (in pseudo-code) is:

```c++
for (auto f : files) {
  // Always start by creating the base map.
  DualMap base_map = ScanBaseMap(f);

  // Scan once for every data source the user selected with '-d'.
  std::vector<DualMap> maps;
  for (auto s : data_sources) {
    maps.push_back(ScanDataSource(f, s));
  }
}
```

## Base Map

To translate between VM and file space, we always begin by creating a "base
map."  The base map is just a `DualMap` like any other, but we give it special
meaning:

* It defines what ranges of file and VM space constitute "the entire binary"
  (ie. the "TOTALS" row of the final report).
* We use it to translate between VM space and File space.

This means that the base map must be exhaustive, and must also provide
translation for any entity that exists in both VM and file space.  For example,
suppose we are scanning the "symbols" data source and we see in the symbol
table that address `0x12345` corresponds to symbol `foo`.  We will add that to
VM map immediately, but we will also use the base map to translate address
`0x12355` to a file offset so we can add that range to the file map.

How does the base map store translation info?  I left one thing out about
`RangeMap` above.  In addition to storing a label for every region, it can also
(optionally) store a member called `other_start`.  This stores the
corresponding offset in the other space, and lets you translate addresses from
one to the other. The `other_start` member is only used in the base map.

We build the base map by scanning either the segments (program headers) or
sections of the binary.  These give both VM address and file offset for regions
of the binary that are loaded into memory.  To make sure we cover the entire
file space, we use `[Unmapped]` as a last ditch fallback for any regions of the
on-disk binary that didn't have any segment/section data associated with them.
This ensures that Bloaty always accounts for the entire physical binary, even if
we can't find any information about it.

## Scanning Data Sources

Once we have built the base map, we can get on to the meat of Bloaty's work.
We can now scan the binary according to whatever data source(s) the user has
selected.

### Segments and Sections

The `segments` and `sections` data sources are relatively straightforward. For
the most part we can simply scan the segments/sections table and call it a day.

For ELF, segments and sections have distinct tables in the binary that can be
scanned independently. This means that technically a section could span multiple
segments, but in practice segments/sections form a 1:many relationship, where
each section is contained entirely within a single segment.

Currently Bloaty only reports `PT_LOAD` and `PT_TLS` segments. We scan `PT_LOAD`
segments first, so if there is overlap with `PT_TLS` the `PT_LOAD` label will
win. In the future It may make sense to scan `PT_TLS` first, as this is more
granular data that can give insight into the per-thread runtime overhead of TLS
variables. It may also make sense to scan other segment types, to give more
granular info.

ELF segments do not have names. To distinguish between different `PT_LOAD`
segments, we include both a segment offset and the segment flags in the label,
eg. `LOAD #2 [R]`.

For Mach-O, segments are contained within a file-level table of "load
commands." Each load command has a type, and technically speaking, segments are
a subset of all load commands. However Bloaty's `segments` data source reports
many non-segment load commands such as the symbol table (`LC_SYMTAB`,
`LC_DYSYMTAB`), code signature (`LC_CODE_SIGNATURE`), and more. Segments can
have zero or more sections, so in Mach-O files the 1:many nature of segments
and sections is enforced by the file format.

For `segments` and `sections` we have to decide how to attribute the regions of
the file that correspond to the segment/section headers themselves. Bloaty's
general philosophy is to include the metadata with the data, so each label
shows the true weight of everything associated with that label. This would
suggest that the `.text` label should include the `.text` section as well as
the section header entry for the `.text` section. However this would hide the
overhead of the ELF headers, which can be significant if there are many
sections. Bloaty currently has no higher-level data source that could show the
ELF headers separately from the ELF data, and even if there was such a data
source it would have narrow usefulness so people would probably not think to
use it very often.

There is not an easy answer to this question.  At the moment Bloaty will
include section headers with the corresponding section, but will *not* include
segment headers with the corresponding segment.  This may or may not be the
best solution to this problem, and this may change if another solution proves
to work better.

### Symbols

The `symbols` data source is where Bloaty's deep parsing of the binary delivers
the most benefit, as it provides detailed information that you cannot get
from a linker map or symbol table.

For example, take the following data from running Bloaty on itself:

```
$ ./bloaty bloaty -d symbols,sections
    FILE SIZE        VM SIZE
 --------------  --------------
[...]
   0.2%   116Ki   1.6%   116Ki    AArch64_printInst
    84.9%  98.8Ki  84.9%  98.8Ki    .text
    14.9%  17.4Ki  14.9%  17.4Ki    .rodata
     0.1%     156   0.1%     156    .eh_frame
     0.0%      24   0.0%       0    .symtab
     0.0%      18   0.0%       0    .strtab
     0.0%       8   0.0%       8    .eh_frame_hdr
[...]
   0.1%  50.1Ki   0.7%  49.8Ki    reg_name_maps
    59.6%  29.8Ki  59.8%  29.8Ki    .rela.dyn
    40.0%  20.0Ki  40.2%  20.0Ki    .data.rel.ro
     0.4%     216   0.0%       0    .symtab
     0.0%      14   0.0%       0    .strtab
```

I excerpted two symbols from the report. Between these two symbols, Bloaty has
found seven distinct kinds of data that contributed to these two symbols. If
you wrote a tool that naively just parsed the symbol table, you would only find
the first of these seven:

1. `.text.`/`.data.rel.ro`: this is the data we obtain by simply following the
   symbol table entry. This is the primary code or data emitted by the function
   or variable.
2. `.eh_frame`: this is the "unwind information" for a function. [It is used for
   many things](https://stackoverflow.com/a/26302715/77070), including C++
   exceptions and stack traces when no frame pointer is available.
3. `.eh_frame_hdr`: this is metadata about the `.eh_frame` section.
4. `.symtab`: this is the function/variable's symbol table entry itself. It is a
   fixed size for every entry. The fact that `reg_name_maps` above has a
   `.symtab` size of 216 indicates that there must actually be 9 different
   symbols being represented by this entry. Bloaty has combined them because
   they all have the same name. We can break them apart if we want using:

   ```
   $ ./bloaty bloaty -d compileunits,symbols --source-filter=reg_name_maps$
       FILE SIZE        VM SIZE
    --------------  --------------
     20.3%  10.2Ki  20.3%  10.1Ki    ../third_party/capstone/arch/AArch64/AArch64Mapping.c
      100.0%  10.2Ki 100.0%  10.1Ki    reg_name_maps
     18.9%  9.45Ki  18.9%  9.43Ki    ../third_party/capstone/arch/X86/X86Mapping.c
      100.0%  9.45Ki 100.0%  9.43Ki    reg_name_maps
     16.4%  8.20Ki  16.4%  8.18Ki    ../third_party/capstone/arch/PowerPC/PPCMapping.c
      100.0%  8.20Ki 100.0%  8.18Ki    reg_name_maps
     10.7%  5.35Ki  10.7%  5.33Ki    ../third_party/capstone/arch/Mips/MipsMapping.c
      100.0%  5.35Ki 100.0%  5.33Ki    reg_name_maps
      9.1%  4.57Ki   9.1%  4.55Ki    ../third_party/capstone/arch/SystemZ/SystemZMapping.c
      100.0%  4.57Ki 100.0%  4.55Ki    reg_name_maps
      8.7%  4.35Ki   8.7%  4.31Ki    ../third_party/capstone/arch/ARM/ARMMapping.c
      100.0%  4.35Ki 100.0%  4.31Ki    reg_name_maps
      7.0%  3.52Ki   7.0%  3.49Ki    ../third_party/capstone/arch/TMS320C64x/TMS320C64xMapping.c
      100.0%  3.52Ki 100.0%  3.49Ki    reg_name_maps
      6.9%  3.44Ki   6.9%  3.41Ki    ../third_party/capstone/arch/Sparc/SparcMapping.c
      100.0%  3.44Ki 100.0%  3.41Ki    reg_name_maps
      2.0%  1.02Ki   2.0%    1016    ../third_party/capstone/arch/XCore/XCoreMapping.c
      100.0%  1.02Ki 100.0%    1016    reg_name_maps
    100.0%  50.1Ki 100.0%  49.8Ki    TOTAL
   Filtering enabled (source_filter); omitted file = 46.7Mi, vm = 7.08Mi of entries
   ```
5. `.strtab`: this is the text of the function/variables's name itself in the
   string table. Longer names take up more space in the binary, and Bloaty's
   analysis here reflects that (though the symbol table is not loaded at
   runtime, so it's not costing RAM).
6. `.rela.dyn`: these are relocations embedded into the executable. Normally we
   would associate relocations with `.o` files and not the final linked binary.
   However shared objects and position-independent executables must also emit
   relocations for any global variable that is initialized to an address of some
   other data. These relocations can take up a significant amount of space,
   indeed more space than the data itself in this case! Without this deep
   analysis of the binary, this cost would be invisible. Bloaty scans all
   relocation tables and "charges" each relocation entry to the function/data
   that requires the relocation (*not* the function being pointed to).
7. `.rodata`: Bloaty has found some data associated with this function.
   Sometimes data doesn't get its own symbol table entry, for whatever reason.
   Bloaty can attribute anonymous data to the function that uses it by
   disassembling the binary looking for instructions that reference a different
   part of the binary. If the same anonymous data is used by more than one
   function, then the first one scanned will "win" and assume the whole cost,
   as Bloaty has no concept of sharing the cost. Every byte of the file must
   have exactly one label associated with it.

Note that this is more granular information than you can get from a linker map
file. A linker map file will break down some of these sections by compile unit,
but the symbol-level granularity is limited to the primary code/data for each
symbol (#1 in the list above).

### Compile Units

Like symbols, we can see that Bloaty is capable of breaking down lots of
sections by compile unit:

```
$ ./bloaty bloaty -d compileunits,sections
    FILE SIZE        VM SIZE
 --------------  --------------
  37.9%  17.7Mi  49.4%  3.52Mi    [160 Others]
  15.0%  7.04Mi   3.4%   246Ki    ../third_party/protobuf/src/google/protobuf/descriptor.cc
    33.9%  2.38Mi   0.0%       0    .debug_info
    32.6%  2.29Mi   0.0%       0    .debug_loc
    17.2%  1.21Mi   0.0%       0    .debug_str
     6.5%   468Ki   0.0%       0    .debug_ranges
     5.3%   381Ki   0.0%       0    .debug_line
     2.8%   204Ki  83.1%   204Ki    .text
     1.0%  70.9Ki   0.0%       0    .strtab
     0.4%  25.7Ki  10.4%  25.7Ki    .eh_frame
     0.2%  13.3Ki   0.0%       0    .symtab
     0.1%  10.6Ki   4.3%  10.6Ki    .rodata
     0.1%  3.97Ki   1.6%  3.97Ki    .eh_frame_hdr
     0.0%  1.03Ki   0.4%  1.03Ki    .rela.dyn
     0.0%     368   0.1%     368    .data.rel.ro
     0.0%       0   0.0%      81    .bss
[...]
```

To attribute all of the different `.debug_*` sections, Bloaty includes parsers
for all of the different DWARF formats that live in these sections. We also use
the DWARF data to find which symbols belong to which compile units.

The `compileunits` data source contains much of the same data that you could get
from a linker map. Since each compile unit generally comes from a separate `.o`
file, a linker map can often give good data about which parts of the binary came
from which translation units. However Bloaty is able to derive this data without
needing a linker map file, which may be tricky to obtain. The `compileunits`
data source is also useful when combined with other data sources in hierarchical
profiles.
