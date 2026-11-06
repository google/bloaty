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
"""Generates seeds for the (extended) fuzz_target.

Structurally valid, minimal binaries for every object format bloaty parses,
so that mutations stay inside each parser's deep code instead of bouncing off
the magic-byte checks: ELF64 (with build-id note and symtab), Mach-O 64, a
universal (fat) binary, PE/COFF, WebAssembly (with name custom section) and
an ar archive embedding an ELF member.

Usage: generate_fuzz_target_seeds.py <output-dir>
"""

import os
import struct
import sys

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


def _shdr(name, typ, addr, offset, size, link=0, info=0, entsize=0):
    return struct.pack("<IIQQQQIIQQ", name, typ, 0, addr, offset, size, link,
                       info, 1, entsize)


def _symtab(symbols):
    symtab = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    for i, name in enumerate(symbols):
        st_name = 1 + sum(len(s) + 1 for s in symbols[:i])
        symtab += struct.pack("<IBBHQQ", st_name, 0x12, 0, 1,
                              BASE_VADDR + 0x60, 6)
    return symtab


def elf64(text=b"\x55\x48\x89\xe5\x5d\xc3", symbols=(b"fn", b"_Z3foov"),
          note=None):
    """ELF64 with PT_LOAD, .text, optional .note.gnu.build-id, .symtab."""
    eh_size, phentsize = 64, 56
    text_off = eh_size + phentsize
    note_off = text_off + len(text)
    symtab_off = note_off + (len(note) if note else 0)
    symtab_size = 24 * (1 + len(symbols))
    strtab_off = symtab_off + symtab_size
    strtab = b"\0" + b"\0".join(symbols) + b"\0"
    shstrtab_off = strtab_off + len(strtab)
    shstrtab = b"\0.text\0.note.gnu.build-id\0.symtab\0.strtab\0.shstrtab\0"
    shoff = shstrtab_off + len(shstrtab)
    shnum = 6 if note else 5
    shstrndx = shnum - 1

    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH", b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8,
        2, 62, 1, BASE_VADDR + text_off, eh_size, shoff, 0,
        eh_size, phentsize, 1, 64, shnum, shstrndx)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, BASE_VADDR, BASE_VADDR,
                       shoff, shoff, 0x1000)
    offs = {"text": 1, "note": 7, "symtab": 27, "strtab": 35, "shstrtab": 43}
    if note:
        shdrs = (
            _shdr(0, 0, 0, 0, 0) +
            _shdr(offs["text"], 1, BASE_VADDR + text_off, text_off,
                  len(text)) +
            _shdr(offs["note"], 7, 0, note_off, len(note)) +
            _shdr(offs["symtab"], 2, 0, symtab_off, symtab_size, link=4,
                  info=1, entsize=24) +
            _shdr(offs["strtab"], 3, 0, strtab_off, len(strtab)) +
            _shdr(offs["shstrtab"], 3, 0, shstrtab_off, len(shstrtab)))
    else:
        shdrs = (
            _shdr(0, 0, 0, 0, 0) +
            _shdr(offs["text"], 1, BASE_VADDR + text_off, text_off,
                  len(text)) +
            _shdr(offs["symtab"], 2, 0, symtab_off, symtab_size, link=3,
                  info=1, entsize=24) +
            _shdr(offs["strtab"], 3, 0, strtab_off, len(strtab)) +
            _shdr(offs["shstrtab"], 3, 0, shstrtab_off, len(shstrtab)))

    body = ehdr + phdr + text
    if note:
        body += note
    return body + _symtab(symbols) + strtab + shstrtab + shdrs


def build_id_note(build_id=b"\xde\xad\xbe\xef" * 2):
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    pad = (4 - len(note) % 4) % 4
    return note + b"\0" * pad

def macho64(text=b"\x55\x48\x89\xe5\x5d\xc3"):
    header = struct.pack("<IIiiIIII", 0xFEEDFACF, 0x01000007, 3, 2, 1,
                         152, 0, 0)
    seg = struct.pack("<II16sQQQQiiII", 0x19, 152, b"__TEXT",
                      BASE_VADDR, 0x1000, 0, 216 + len(text), 7, 5, 1, 0)
    sect = struct.pack("<16s16sQQiiIIIIIi", b"__text", b"__TEXT",
                       BASE_VADDR + 216, len(text), 216, 4, 0, 0, 0x80000400,
                       0, 0, 0)
    return header + seg + sect + text


def universal(slice):
    fat = struct.pack(">IIIII", 0xCAFEBABE, 1, 7, 3, 16)
    fat += struct.pack(">II", 20, len(slice))
    return fat + slice


def pe64(text=b"\x55\x48\x89\xe5\x5d\xc3"):
    dos = b"MZ" + b"\0" * 58 + struct.pack("<I", 0x40)
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = struct.pack("<HBB", 0x20B, 14, 0) + b"\0" * 234
    text_ptr = len(dos) + 4 + 20 + 240
    sect = struct.pack("<8sIIIIIIHHI", b".text", len(text), text_ptr,
                       len(text), text_ptr, 0, 0, 0, 0, 0x60000020)
    return dos + b"PE\0\0" + coff + opt + sect + text


def wasm():
    out = b"\x00asm" + struct.pack("<I", 1)
    name_sec = (uleb(4) + b"name" + uleb(2) +
                b"\x01" + uleb(4) + b"\x00" + uleb(3) + b"fn")
    out += bytes([0]) + uleb(len(name_sec)) + name_sec
    body = b"\x00\x0b"
    code = uleb(1) + uleb(len(body)) + body
    out += bytes([10]) + uleb(len(code)) + code
    return out


def ar(member):
    header = (b"member.o/".ljust(16, b" ") + b"0".ljust(12, b" ") +
              b"0".ljust(6, b" ") + b"0".ljust(6, b" ") +
              b"644".ljust(8, b" ") + str(len(member)).ljust(10).encode() +
              b"`\n")
    return b"!<arch>\n" + header + member + (b"\n" if len(member) % 2 else b"")


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    elf = elf64(note=build_id_note())
    binaries = {
        "seed_elf": elf,
        "seed_elf_nosym": elf64(symbols=(), note=build_id_note()),
        "seed_macho": macho64(),
        "seed_universal": universal(macho64()),
        "seed_pe": pe64(),
        "seed_wasm": wasm(),
        "seed_ar": ar(elf),
    }
    for name, data in binaries.items():
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
