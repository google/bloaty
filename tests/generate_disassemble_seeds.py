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
"""Generates the seed corpus for disassemble_fuzz_target.

Each seed is  <elf binary>$FN$<function name>  .  The ELF part is a minimal
but valid ELF64 (PT_LOAD covering the whole file, .text with a valid x86-64
prologue, .symtab with a FUNC symbol "fn"), and the name part matches that
symbol so the disassembly path is actually entered.

Usage: generate_disassemble_seeds.py <output-dir>
"""

import os
import struct
import sys

MARKER = b"$FN$"
BASE_VADDR = 0x400000


def minimal_elf64(text=b"\x55\x48\x89\xe5\x5d\xc3\x90\x90",
                  symbols=(b"fn",)):
    eh_size, phentsize, phnum = 64, 56, 1
    shentsize, shnum = 64, 5

    text_off = eh_size + phentsize * phnum
    symtab_off = text_off + len(text)
    symtab_size = 24 * (1 + len(symbols))
    strtab_off = symtab_off + symtab_size
    strtab = b"\0" + b"\0".join(symbols) + b"\0"
    shstrtab_off = strtab_off + len(strtab)
    shstrtab = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    shoff = shstrtab_off + len(shstrtab)

    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH", b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8,
        2, 62, 1, BASE_VADDR + text_off, eh_size, shoff, 0,
        eh_size, phentsize, phnum, shentsize, shnum, 4)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, BASE_VADDR, BASE_VADDR,
                       shoff, shoff, 0x1000)

    def shdr(name, typ, addr, offset, size, link=0, info=0, entsize=0):
        return struct.pack("<IIQQQQIIQQ", name, typ, 0, addr, offset, size,
                           link, info, 1, entsize)

    name_off = {"text": 1, "symtab": 7, "strtab": 15, "shstrtab": 23}
    symtab = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    for i, name in enumerate(symbols):
        st_name = 1 + sum(len(s) + 1 for s in symbols[:i])
        symtab += struct.pack("<IBBHQQ", st_name, 0x12, 0, 1,
                              BASE_VADDR + text_off, len(text))
    shdrs = (
        shdr(0, 0, 0, 0, 0) +
        shdr(name_off["text"], 1, BASE_VADDR + text_off, text_off,
             len(text)) +
        shdr(name_off["symtab"], 2, 0, symtab_off, symtab_size, link=3,
             info=1, entsize=24) +
        shdr(name_off["strtab"], 3, 0, strtab_off, len(strtab)) +
        shdr(name_off["shstrtab"], 3, 0, shstrtab_off, len(shstrtab)))

    return ehdr + phdr + text + symtab + strtab + shstrtab + shdrs


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    elf = minimal_elf64()
    seeds = [
        elf + MARKER + b"fn",
        # Longer function body: prologue + arithmetic + epilogue.
        minimal_elf64(
            b"\x55\x48\x89\xe5\x89\x7d\xfc\x8b\x45\xfc\x83\xc0\x01\x5d\xc3")
        + MARKER + b"fn",
        # Mismatching name: exercises the lookup-miss error path cheaply.
        elf + MARKER + b"does_not_exist",
    ]
    for i, seed in enumerate(seeds):
        with open(os.path.join(out_dir, "seed_%d" % i), "wb") as f:
            f.write(seed)


if __name__ == "__main__":
    main()
