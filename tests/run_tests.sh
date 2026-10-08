#!/usr/bin/env sh
# Host unit tests of the protocol codec (no ESP32 needed).
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  components/truma_inetx/cbor.cpp components/truma_inetx/frame.cpp tests/codec_test.cpp \
  -o build/codec_test
./build/codec_test
