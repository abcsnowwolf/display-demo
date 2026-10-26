#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
tools/sim/build.sh
mkdir -p docs/html-preview
for name in nested ratio chinese wrap; do
    tools/sim/sim fonts.bin "examples/html/$name.html" "docs/html-preview/$name.png"
done
