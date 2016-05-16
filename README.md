
# Bloaty McBloatface: a size profile for binaries

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
