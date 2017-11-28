#!/bin/bash
# Copyright 2016 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if [ "$#" == "0" ] ; then
  echo "Usage: make_test_files.sh <output dir>"
  exit 1
fi

cd $1
OUTPUT_DIR=`pwd`
TMP=`mktemp -d`
CC="${CC:-cc}"
echo Writing output to $OUTPUT_DIR
echo Working in $TMP
cd $TMP

function publish() {
  echo $1
  cp $1 $OUTPUT_DIR
}

function make_tmp_obj() {
  FILE=$1
  CONTENTS="$2"
  CFILE=`basename $1`.c
  echo "$CONTENTS" > $CFILE
  $CC -g -fPIC -o $FILE -c $CFILE
}

function make_obj() {
  FILE=$1
  CONTENTS="$2"
  make_tmp_obj $FILE "$CONTENTS"
  publish $FILE
}

function make_ar() {
  FILE=$1
  shift
  ar rcs $FILE "$@"
  publish $FILE
}

function make_so() {
  FILE=$1
  shift
  $CC -g -shared -o $FILE "$@"
  publish $FILE
}

function make_binary() {
  FILE=$1
  shift
  $CC -o $FILE "$@"
  publish $FILE
}

make_obj "01-empty.o" ""

make_obj "02-simple.o" "
#include <stdint.h>
uint64_t bss_a = 0;
uint32_t bss_b = 0;
uint64_t data_a = 1;
uint32_t data_b = 2;
const uint64_t rodata_a = 1;
const uint32_t rodata_b = 2;
uint32_t func1() { return bss_b / 17; }
uint32_t func2() { return data_b / 17; }"

make_tmp_obj "foo.o" "
int foo_x[1000] = {0};
int foo_y = 0;
int foo_func() { return foo_y / 17; }
"

make_tmp_obj "bar.o" "
int bar_x[1000] = {1};
int bar_y = 1;
int bar_z = 0;
int bar_func() { return bar_y / 17; }
"

make_tmp_obj "a_filename_longer_than_sixteen_chars.o" "
int long_filename_x[3] = {1};
int long_filename_y = 2;
"

make_ar "03-simple.a" "foo.o" "bar.o" "a_filename_longer_than_sixteen_chars.o"
make_so "04-simple.so" "foo.o" "bar.o"

make_tmp_obj "main.o" "int main() {}"

make_binary "05-binary.bin" "foo.o" "bar.o" "main.o"

# Make an object like foo.o but with different sizes.

make_tmp_obj "foo2.o" "
int foo_x[500] = {0};
long long foo_y = 0;
int foo_func() { return foo_y / 17 * 37 / 21; }
"

make_ar "06-diff.a" "foo2.o" "bar.o" "a_filename_longer_than_sixteen_chars.o"

cp "05-binary.bin" "07-binary-stripped.bin"
strip "07-binary-stripped.bin"
publish "07-binary-stripped.bin"
