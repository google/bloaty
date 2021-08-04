#!/usr/bin/env python

import subprocess
import os
import sys

basedir = os.path.dirname(sys.argv[0])
readme = os.path.join(basedir, "doc/using.md")

with open(readme) as f:
  inp = f.read()

out = ""

it = iter(inp.splitlines(True))

for line in it:
  out += line
  if line.startswith("```cmdoutput"):
    # Get command.
    cmd = next(it)
    assert cmd.startswith("$ "), cmd
    real_cmd = cmd[2:].strip()
    out += cmd

    print("Running: " + real_cmd)
    out += subprocess.check_output(real_cmd, shell=True)

    # Skip pre-existing command output.
    line = next(it)
    while not line.startswith("```"):
      line = next(it)
    out += line

with open(readme, "w") as f:
  f.write(out)
