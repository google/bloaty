#!/bin/bash

CMD=`pwd`/experimental/users/haberman/bloaty/bloaty_test
cd experimental/users/haberman/bloaty/tests/testdata/$1 && exec $CMD
