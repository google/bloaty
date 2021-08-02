#!/usr/bin/python3

import os
import glob
import re
import sys
import subprocess
import shutil
import tempfile

successes = 0
failures = []

def TestFile(filename):
  cwd = os.getcwd()
  abspath = os.path.abspath(filename)
  global successes
  global failures
  with open(abspath) as f:
    print(filename)
    tmpdir = tempfile.mkdtemp(prefix="/tmp/bloaty-")
    contents = f.read()
    file_count = len(re.findall(r'^---', contents, flags=re.MULTILINE))
    for i in range(1, file_count + 1):
      subprocess.check_call('yaml2obj {0} --docnum={1} > {1}'.format(abspath, str(i)), shell=True, cwd=tmpdir)
    lines = contents.splitlines()
    while len(lines[0]) == 0 or lines[0][0] != '$':
      lines.pop(0)
    bloaty_invocation = os.path.join(cwd, lines[0][2:]) + ' > actual'
    lines.pop(0)
    expected_output = "\n".join(lines) + "\n"
    failure = None
    if subprocess.call(bloaty_invocation, shell=True, cwd=tmpdir) != 0:
      failure = "CRASHED"
    else:
      with open(os.path.join(tmpdir, 'expected'), 'w') as expected:
        expected.write(expected_output)
      if subprocess.call('diff -u expected actual', shell=True, cwd=tmpdir) != 0:
        failure = "FAILED"

    if failure:
      print("{}: {}".format(failure, filename))
      print("{}: output in {}".format(failure, tmpdir))
      failures.append((filename, tmpdir))
    else:
      successes += 1
      shutil.rmtree(tmpdir)

def TestArg(arg):
  if os.path.isdir(arg):
    files = sorted(glob.glob(os.path.join(arg, "**/*.test"), recursive=True))
    for file in files:
      TestFile(file)
  else:
    TestFile(arg)

args = sys.argv[1:]

if not args:
  print("Usage: tester.py <FILE-OR-DIR> ...")
  sys.exit(1)

for arg in sys.argv[1:]:
  TestArg(arg)

if not failures:
  print("SUCCESS: {} test(s) passed".format(successes))
else:
  print("\nFAILURE: {} tests passed, {} tests failed:".format(successes, len(failures)))
  for failure in failures:
    print("  - {} (output in {})".format(*failure))
  sys.exit(1)
