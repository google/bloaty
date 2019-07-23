
# How Bloaty Works

At a high level, Bloaty's goal is to create a map of the binary where every
byte has a label attached to it.  Every byte starts out as unknown
(unattributed).  As we scan the binary we assign labels to different ranges of
the file.  For example, if the user selected the "symbols" data source we scan
the symbol table and use the symbol name as the label for each range.

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
(optionally) store a member called `other_start`.  This is the offset in the
other space where this range maps to.

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

We must do more than just scan a single table though.  For example, if the user
asked us to scan symbols we can't simply scan the symbol table, add all the
symbol table entries to a `RangeMap`, and call it a day.  The first prerelease
version of Bloaty did just that and I got very understandable complaints that
Bloaty's coverage was low, and very large parts of the binary would show up as
`[None]`.

The problem is that the symbol table only refers to the actual machine code of
the function.  But a function can emit far more artifacts into a binary than
just the machine code.  It can also emit:

* unwind info into `.eh_frame`
* relocations into `.rela.dyn`, etc.
* debugging information into various `.debug_*` sections.
* the symbol table entries themselves (yes, these do take up space!)

If we want to achieve high coverage of the binary, we have to scan all of these
different sections and attribute them as best we can to individual
symbols/compileunits/etc.

All of this means that we have to delve deep into ELF, DWARF, Mach-O, etc. to
get good coverage.  We have to venture into dark arts like `.eh_frame` which is
underspecified and underdocumented, and that few programs ever have any reason
to parse.  For Bloaty to work well, we must go deeper.

## ELF

TODO

## DWARF

TODO

## Mach-O

TODO
