# Change Log

## [Bloaty McBloatface v1.1](https://github.com/google/bloaty/releases/tag/v1.1) (2020-05-23)

### Added

* **Source Filtering**: Bloaty can now filter the results based on a regex
  match. See "Source filter" in [README.md](README.md) for details. (#177)
* **Show only File or VM**: It is possible to restrict the output to show only
  "VM SIZE" or "FILE SIZE" by passing `--domain=vm` or `--domain=file`. (#165)

### Fixed

* **Use system deps in CMake build**: The CMake build can now use system deps
  for protobuf, capstone, and re2 if they are present. Hopefully this will make
  Bloaty easier to package in package managers. (#155)
* **DWARF 4 Support**: Fixed one place in Bloaty that was not accepting DWARF 4.
  (#166)
* **DWARF fix**: Added support for `DW_FORM_ref_udata`. (#197)
* **Big-endian fix**: Added missing byte swapping when getting the build ID for
  a big-endian binary. (#182)
* **WASM demangling**: WASM symbols are now properly demangled. (#149)
* **New WASM sections**: Added support for new DataCount and Event sections
  in WASM. (#178)
* **Scaling fixes**: Fixed integer overflow in 32-bit builds, and other issues
  that arise when using Bloaty with many files and/or large files. (#193)
* **Improved coverage**: Bloaty now properly attributes `.eh_frame` in object
  files, and attributes ELF headers to the corresponding section. (#168)
* **CSV escaping**: Bloaty now properly escapes CSV output when a field contains
  a comma or double quote. (#174)

### Changed

* **File size column moved left**: In the default output, the file size now
  appears on the left. This means that all numbers are now on the left, which
  leads to more readable output when viewing in a proportional font or in a
  limited-width window.

  Old:
  ```
       VM SIZE                         FILE SIZE
   --------------                   --------------
     0.0%       0 .debug_info        7.97Mi  29.5%
     0.0%       0 .debug_loc         6.40Mi  23.7%
  ```

  New:
  ```
      FILE SIZE        VM SIZE    
   --------------  -------------- 
    30.0%  8.85Mi   0.0%       0    .debug_info
    24.7%  7.29Mi   0.0%       0    .debug_loc
  ```

  This shouldn't cause breakage, as anything consuming Bloaty's output
  programmatically should be using `--csv` or `--tsv`. (#165)
* **ELF Segment labels now contain index**: Previously ELF segment labels looked
  like `LOAD [RW]` with segment flags only. Now they also contain the segment
  index, eg. `LOAD #1 [RW]`, so the output can distinguish between different
  segments with the same flags. (#159)

### Security

Bloaty should not be considered fully hardened against malicious binaries.  This
is one of the major reasons why Bloaty is not offered as an in-process library,
and should only be used through its command-line interface in a dedicated
address space. If you do not trust the input, further process sandboxing is
advisable.

However we do perform fuzzing of the parsers, and fix and crash bugs that are
found by fuzzing.

* **Fixed crash bugs found by fuzzing** (#173, #175)

## [Bloaty McBloatface v1.0](https://github.com/google/bloaty/releases/tag/v1.0) (2018-08-07)

This is the first formal release of Bloaty.

Changes since Bloaty was [first announced in Nov
2016](http://blog.reverberate.org/2016/11/07/introducing-bloaty-mcbloatface.html):

* **Much better coverage / data quality**: Bloaty now properly attributes
  sections of the binary like the symbol table, debugging information,
  relocations, and frame unwinding info. We even disassemble the binary looking
  for references to anonymous data members that do not have symbol table
  entries. This all means higher quality output, and much less of the binary is
  attributed to `[None]`.
* **Native Mach-O support**: Bloaty can now parse Mach-O files directly,
  instead of shelling out to other programs. The result is much faster and
  higher-quality output for Mach-O. Also the data sources that require debug
  info (like `-d compileunits`) now work with Mach-O.
* **WebAssembly support (EXPERIMENTAL)**: Bloaty can analyze sections and
  symbols in binary WebAssembly files.
* **Native C++ Demangling**: Bloaty can now demangle C++ symbols without
  shelling out to `c++filt`. There are two separate demangling modes, one which
  strips all template parameters and parameter/return types (`shortsymbols`) and
  one that returns full demangled names (`fullsymbols`).
* **Profiling stripped binaries**: Bloaty can read symbols and debug info from
  separate files. This allows you to profile stripped binaries.
* **Parallel file parsing**: If you pass multiple files to Bloaty, it will
  scan them in parallel. If you are parsing lots of files and have lots of CPUs,
  this can save a lot of time.
* **Configuration files**: All options you can specify on the command-line can
  be put in a configuration file instead (and vice versa). This is helpful if
  the options might otherwise overflow the command-line (for example, if you
  are parsing thousands of files). It also lets you save bits of configuration
  to a file for reuse.
* **Custom data sources**: you can create your own data sources by applying
  regexes to the built-in sources. This lets you bucket symbols, source files,
  etc. in ways that make sense for your project.
* **CSV/TSV output**: this is a robust way to parse Bloaty's output and use it
  in other programs. (The default, human-readable output is not designed to be
  parsed and its format may change in backward-incompatible ways).
* **Lots of bugfixes**: Fixed lots of bugs that people reported in various
  platforms and configurations. Bloaty is fuzzed regularly now, and this has
  led to many bugfixes also.
