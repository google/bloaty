#!/bin/bash
shasum -a 256 -c "$@"/.expected_shasums
