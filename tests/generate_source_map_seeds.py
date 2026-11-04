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
"""Generates the seed corpus for source_map_fuzz_target.

Each seed is  <wasm binary>$SM$<source map text>  :
- the WASM part is a structurally valid module carrying a "sourceMappingURL"
  custom section whose content is the registered source map name, which is how
  bloaty links a source map to a module (GetBuildId -> sourcemap_files_ lookup);
- the source map part is a structurally valid map with a "sources" array and a
  "mappings" string in the custom Base64 VLQ dialect parsed by src/source_map.cc.

Usage: generate_source_map_seeds.py <output-dir>
"""

import os
import struct
import sys

MARKER = b"$SM$"
SOURCEMAP_NAME = b"sourcemap_file"

# Terminal digits (end a VLQ number) and continuation digits, matching the
# custom dialect in src/source_map.cc ReadBase64VLQ().
CONT = "ghijklmnopqrstuvwxyz0123456789+/"
TERM = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"


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


def vlq(value):
    """Encodes a signed value in the VLQ dialect the source map parser uses."""
    raw = (abs(value) << 1) | (1 if value < 0 else 0)
    groups = []
    while True:
        groups.append(raw & 31)
        raw >>= 5
        if raw == 0:
            break
    out = ""
    for i, group in enumerate(groups):
        out += (CONT if i + 1 < len(groups) else TERM)[group]
    return out.encode()


def custom_section(name, payload):
    content = uleb(len(name)) + name + payload
    return bytes([0]) + uleb(len(content)) + content


def wasm_with_sourcemap():
    out = b"\x00asm" + struct.pack("<I", 1)
    # GetBuildId() reads uleb(size) + bytes from the sourceMappingURL contents.
    sm_payload = uleb(len(SOURCEMAP_NAME)) + SOURCEMAP_NAME
    out += custom_section(b"sourceMappingURL", sm_payload)
    # Code section: one function with an empty body (locals=0, 'end').
    body = b"\x00\x0b"
    code = uleb(1) + uleb(len(body)) + body
    out += bytes([10]) + uleb(len(code)) + code
    return out


def sourcemap(sources, mappings):
    src = ",".join('"%s"' % s for s in sources)
    return (
        '{"version":3,"file":"out.js","sources":[%s],"names":[],'
        '"mappings":"%s"}' % (src, mappings)
    ).encode()


def seeds():
    return [
        sourcemap(["a.js", "b.js"], "AAAA,CAAC"),
        sourcemap(["a.js", "b.js"], "AAAAA,CAACG"),  # segments with lengths
        sourcemap(["src/main.js", "src/util.js", "vendor/lib.js"],
                  "ACAA,CEAG,CCAC"),
        sourcemap(["x.js"], "AAAA"),  # whitespace variant
        b'{"version":3,"sources":[],"mappings":""}',  # empty arrays
        b'{"version":3}',  # error path: "sources" missing
        sourcemap(["a.js", "b.js"], "AAAA,AAAA,AAAA,AAAA"),
    ]


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    wasm = wasm_with_sourcemap()
    for i, sm in enumerate(seeds()):
        path = os.path.join(out_dir, "seed_%d" % i)
        with open(path, "wb") as f:
            f.write(wasm + MARKER + sm)


if __name__ == "__main__":
    main()
