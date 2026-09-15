#!/bin/sh
# Run from the repository root. Requires SDL2 and libdrm development packages.
set -eu
kms_test_dir=$(mktemp -d)
trap 'rm -rf "$kms_test_dir"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Werror \
  ${KMS_TEST_CXXFLAGS:-} \
  -Isrc -Ilibs/pcm_utils/pcm_utils \
  tests/kms_scanout.cpp src/KMSDisplayConsumer.cpp src/SDL2DisplayConsumerBase.cpp \
  $(pkg-config --cflags --libs sdl2 libdrm) -o "$kms_test_dir/kms_scanout"
"$kms_test_dir/kms_scanout"
