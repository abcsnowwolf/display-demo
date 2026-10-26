#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
AJ=$(ls -d .pio/libdeps/*/ArduinoJson/src | head -1)
mkdir -p build/tests
# SANITIZE=1 enables the same tests under address/undefined-behavior sanitizers.
FLAGS='-O2'
if [ "${SANITIZE:-0}" = 1 ]; then FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'; fi
# Intentional shell splitting for compiler flags.
c++ -std=c++17 $FLAGS -Wall -Wextra -Wno-unused-function -I"$AJ" \
    tools/tests/html_layout_test.cpp src/gfx/*.cpp -o build/tests/html_layout_test
build/tests/html_layout_test fonts.bin
c++ -std=c++17 $FLAGS -Wall -Wextra -Wno-unused-function -Itools/tests/stubs -I"$AJ" \
    tools/tests/html_mcp_test.cpp src/DisplayTools.cpp src/gfx/*.cpp -o build/tests/html_mcp_test
build/tests/html_mcp_test fonts.bin
