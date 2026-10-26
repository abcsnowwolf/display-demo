#!/usr/bin/env python3
"""Pack the subset fonts in fonts/ into the flash font partition image.

Layout (little endian), read by src/gfx/FontEngine.cpp:

    char     magic[4] = "FPK1"
    uint32   count
    count x { char name[24]; uint32 offset; uint32 size; }   # offset from pack start
    font blobs, each 4-byte aligned

Usage: python3 tools/build_fonts.py [output]   (default: fonts.bin)
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "fonts")

# Pack name -> file in fonts/. The names are the identifiers used by firmware.
FONTS = [
    ("sans", "sans.otf"),
    ("sans-bold", "sans-bold.otf"),
    ("kai", "kai.ttf"),
    ("display", "display.otf"),
    ("icons", "icons.otf"),
    ("emoji", "emoji.ttf"),
]


def build(out_path):
    blobs = []
    for name, filename in FONTS:
        path = os.path.join(FONT_DIR, filename)
        if not os.path.exists(path):
            print(f"[fonts] skip {name}: {filename} not found")
            continue
        with open(path, "rb") as f:
            blobs.append((name, f.read()))

    header_size = 8 + len(blobs) * 32
    offset = (header_size + 3) & ~3
    header = bytearray(b"FPK1" + struct.pack("<I", len(blobs)))
    body = bytearray()
    for name, data in blobs:
        header += struct.pack("<24sII", name.encode(), offset + len(body), len(data))
        body += data
        body += b"\0" * ((-len(body)) % 4)
    header += b"\0" * (offset - len(header))

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(body)
    return len(header) + len(body)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "fonts.bin")
    size = build(out)
    print(f"[fonts] wrote {out} ({size / 1024 / 1024:.2f} MiB)")
