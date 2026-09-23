#!/bin/sh
# Run after building the project; no display required.
set -eu
preroll_test_dir=$(mktemp -d)
trap 'rm -rf "$preroll_test_dir"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -Wall -Wextra \
  -Isrc -Ilibs/pcm_utils/pcm_utils \
  tests/pcm_preroll.cpp src/PCMPreroll.cpp src/LineGeneratorStage.cpp \
  src/PCMFrmageStage.cpp src/pcmframe.cpp src/pcmline.cpp \
  src/samplestairsitherator.cpp \
  "${1:-build}/libs/libpcm_utils.a" -o "$preroll_test_dir/pcm_preroll"
"$preroll_test_dir/pcm_preroll"
