#!/usr/bin/env bash

# Makes a release tarball.  We include our dependencies/submodules,
# but we heavily prune their file lists to avoid including lots of
# extraneous baggage.  We also leave out Bloaty's tests, especially
# because some of the test data is large.

set -e

if [ "$#" -ne 1 ]; then
  echo "Usage: make-release.tarball.sh VERSION"
  exit 1
fi

VERSION=$1

FILES=$(git ls-files --exclude-standard --recurse-submodules |
          grep -v googletest |
          grep -v ^tests |
          grep -v third_party/protobuf |
          grep -v 'third_party/capstone/\(suite\|bindings\|xcode\|msvc\|contrib\)' |
          grep -v third_party/abseil-cpp/absl/time/internal/cctz/testdata |
          grep -v ^.git)
FILES="$FILES $(git ls-files --exclude-standard --recurse-submodules |
          grep 'third_party/protobuf/\(src\|cmake\|configure.ac\)')"

# Unfortunately tar on Mac doesn't support --transform, so we have to
# actually move our files to a different directory to get the prefix.
DIR=/tmp/bloaty-$VERSION
rm -rf $DIR
mkdir $DIR
rsync -R $FILES $DIR

BASE=$PWD
cd /tmp
OUT=bloaty-$VERSION.tar.bz2
tar cjf $BASE/$OUT bloaty-$VERSION

echo "Created $OUT"

