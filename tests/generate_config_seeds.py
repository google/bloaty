#!/usr/bin/env python3
# Copyright 2024 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Generates the seed corpus for config_fuzz_target.

Each seed is  <argv tokens, NUL-separated>$CFG$<config file text>.  The argv
tokens exercise bloaty's option parser and the config part is a protobuf
TextFormat Options file (bloaty's `-c` config language) that can define custom
data sources with regex rewrites.  The scanned binary is a minimal but valid
ELF64 with a symbol table so that symbol data sources and rewrites have
something to chew on.

Usage: generate_config_seeds.py <output-dir>
"""

import os
import struct
import sys

MARKER = b"$CFG$"
BASE_VADDR = 0x400000


def uleb(n):
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        if n:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def minimal_elf64(text=b"\x55\x48\x89\xe5\x5d\xc3",
                  symbols=(b"fn", b"_Z3foov")):
    """Builds a minimal ELF64: PT_LOAD + .text + .symtab with `symbols`."""
    ehsize, phentsize, phnum = 64, 56, 1
    shentsize, shnum = 64, 5

    text_off = ehsize + phentsize * phnum
    symtab_off = text_off + len(text)
    symtab_size = 24 * (1 + len(symbols))
    strtab_off = symtab_off + symtab_size
    strtab = b"\0" + b"\0".join(symbols) + b"\0"
    shstrtab_off = strtab_off + len(strtab)
    shstrtab = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    shoff = shstrtab_off + len(shstrtab)

    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH", b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8,
        2, 62, 1, BASE_VADDR + text_off, ehsize, shoff, 0,
        ehsize, phentsize, phnum, shentsize, shnum, 4)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, BASE_VADDR, BASE_VADDR,
                       shoff, shoff, 0x1000)

    def shdr(name, typ, addr, offset, size, link=0, info=0, entsize=0):
        return struct.pack("<IIQQQQIIQQ", name, typ, 0, addr, offset, size,
                           link, info, 1, entsize)

    name_off = {"text": 1, "symtab": 7, "strtab": 15, "shstrtab": 23}
    symtab = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    names = {}
    for i, name in enumerate(symbols):
        names[name] = (BASE_VADDR + text_off, len(text))
        symtab += struct.pack(
            "<IBBHQQ", 1 + sum(len(s) + 1 for s in symbols[:i]), 0x12, 0, 1,
            BASE_VADDR + text_off, len(text))
    shdrs = (
        shdr(0, 0, 0, 0, 0) +
        shdr(name_off["text"], 1, BASE_VADDR + text_off, text_off,
             len(text)) +
        shdr(name_off["symtab"], 2, 0, symtab_off, symtab_size, link=3,
             info=1, entsize=24) +
        shdr(name_off["strtab"], 3, 0, strtab_off, len(strtab)) +
        shdr(name_off["shstrtab"], 3, 0, shstrtab_off, len(shstrtab)))

    return (ehdr + phdr + text + symtab + strtab + shstrtab + shdrs)


def seed(argv_tokens, config):
    return b"\0".join(t.encode() for t in argv_tokens) + MARKER + config.encode()


def seeds():
    custom = (
        'custom_data_source {\n'
        '  name: "c"\n'
        '  base_data_source: "symbols"\n'
        '  rewrite {\n'
        '    pattern: "^_"\n'
        '    replacement: "U_"\n'
        '  }\n'
        '}\n'
        'data_source: "c"\n')
    custom2 = (
        'custom_data_source {\n'
        '  name: "a"\n'
        '  base_data_source: "segments"\n'
        '  rewrite {\n'
        '    pattern: "^fn"\n'
        '    replacement: "FN"\n'
        '  }\n'
        '  rewrite {\n'
        '    pattern: "f.o"\n'
        '    replacement: "BAR"\n'
        '  }\n'
        '}\n'
        'data_source: "a"\n')
    return [
        seed(["-d", "symbols", "--csv"], custom),
        seed(["-d", "compileunits"], 'data_source: "segments"\n'),
        seed(["--tsv", "-n", "5", "-s", "vm"], 'data_source: "symbols"\n'),
        seed(["--source-filter", ".*", "--demangle", "full"],
             'data_source: "segments"\n'),
        seed(["-d", "sections"], custom2),
        seed(["-n", "0"], 'max_rows_per_level: 0\ndata_source: "symbols"\n'),
        seed(["--domain", "file", "--raw-map"], custom),
    ]


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    binary = minimal_elf64()
    for i, s in enumerate(seeds()):
        with open(os.path.join(out_dir, "seed_%d" % i), "wb") as f:
            f.write(binary + s)


if __name__ == "__main__":
    main()
