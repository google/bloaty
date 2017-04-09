#!/bin/bash
if ! shasum -a 256 -c "$@"/.expected_shasums; then
  echo from "$@"/.expected_shasums expected:
  cat "$@"/.expected_shasums
  echo
  echo "Actually:"
  shasum -a 256 "$@"/*
  exit 1
fi
