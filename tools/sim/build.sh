#!/bin/sh
# Builds the desktop scene previewer. Needs ArduinoJson, which `pio pkg install`
# (or any firmware build) places under .pio/libdeps.
set -e
cd "$(dirname "$0")"
ROOT=../..
AJ=$(ls -d $ROOT/.pio/libdeps/*/ArduinoJson/src | head -1)
c++ -std=c++17 -O2 -Wall -Wno-unused-function -I"$AJ" -o sim sim.cpp \
    $ROOT/src/gfx/Canvas.cpp $ROOT/src/gfx/DisplayList.cpp $ROOT/src/gfx/FontEngine.cpp \
    $ROOT/src/gfx/Icons.cpp $ROOT/src/gfx/Scenes.cpp $ROOT/src/gfx/StbTrueType.cpp \
    $ROOT/src/gfx/Style.cpp $ROOT/src/gfx/TextLayout.cpp
echo "built tools/sim/sim"
