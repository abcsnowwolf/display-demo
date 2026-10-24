"""PlatformIO extra script: adds the `uploadfonts` target.

    pio run -t uploadfonts

packs fonts/ into fonts.bin and writes it to the "fonts" partition declared in
partitions.csv. Only needed once per board (or after changing the fonts).
"""
import csv
import os
import sys

Import("env")  # noqa: F821  (provided by SCons)

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
sys.path.insert(0, os.path.join(PROJECT_DIR, "tools"))
import build_fonts  # noqa: E402


def font_partition():
    with open(os.path.join(PROJECT_DIR, "partitions.csv")) as f:
        rows = csv.reader(l for l in f if l.strip() and not l.lstrip().startswith("#"))
        for row in rows:
            row = [c.strip() for c in row]
            if row[0] == "fonts":
                return int(row[3], 0), int(row[4], 0)
    raise RuntimeError("partitions.csv has no 'fonts' partition")


def upload_fonts(target, source, env):
    offset, capacity = font_partition()
    image = os.path.join(env.subst("$BUILD_DIR"), "fonts.bin")
    size = build_fonts.build(image)
    if size > capacity:
        raise RuntimeError(f"font pack is {size} bytes but the partition holds {capacity}")
    print(f"[fonts] {size / 1024 / 1024:.2f} MiB -> partition @ 0x{offset:X}")
    env.AutodetectUploadPort()
    cmd = " ".join([
        '"$PYTHONEXE"', '"$UPLOADER"',
        "--chip", env.BoardConfig().get("build.mcu", "esp32s3"),
        "--port", '"$UPLOAD_PORT"', "--baud", "$UPLOAD_SPEED",
        "write_flash", "-z", hex(offset), f'"{image}"',
    ])
    return env.Execute(env.VerboseAction(cmd, "Writing font partition"))


env.AddCustomTarget(  # noqa: F821
    name="uploadfonts",
    dependencies=None,
    actions=[upload_fonts],
    title="Upload Fonts",
    description="Pack fonts/ and flash them to the fonts partition",
)
