#!/bin/sh
# No root, Raspberry Pi, or physical-memory access needed.
set -eu
vec_test_dir=$(mktemp -d)
trap 'rm -rf "$vec_test_dir"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Werror \
  -Isrc tests/vec_mono525.cpp src/VecMono525.cpp -o "$vec_test_dir/vec_mono525"
"$vec_test_dir/vec_mono525"
