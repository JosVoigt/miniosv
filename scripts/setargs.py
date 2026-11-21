#!/usr/bin/env python3
"""Set (or show) the application arguments stored in a boot image.

    scripts/setargs.py build/release.x64/loader.img "duckdb -c 'SELECT 42'"
    scripts/setargs.py build/release.x64/loader.img          # show current
    scripts/setargs.py build/release.x64/loader.img --clear

The arguments live in one sector of the boot disk, at the LBA declared in
include/osv/bootargs.hh. 
That gap -- between the end of the GPT partition entry array at LBA 33 and 
the start of the ESP at LBA 2048 -- belongs to no partition, so writing there 
disturbs nothing.

Editing the image in place means changing arguments costs no rebuild, and the
same image can be re-tasked before each boot.
"""

import argparse
import struct
import sys

# Keep in sync with include/osv/bootargs.hh.
LBA = 34
SECTOR = 512
MAGIC = b"MINIARGS"
MAX_ARGS = 448
HEADER = struct.Struct("<8sII")     # magic, length, reserved


def read_block(path):
    with open(path, "rb") as fh:
        fh.seek(LBA * SECTOR)
        return fh.read(SECTOR)


def show(path):
    block = read_block(path)
    if len(block) < HEADER.size:
        print(f"setargs: {path} is too small to hold a block", file=sys.stderr)
        return 1
    magic, length, _ = HEADER.unpack_from(block)
    if magic != MAGIC:
        print("setargs: no argument block (image will boot with no arguments)")
        return 0
    length = min(length, MAX_ARGS - 1)
    args = block[HEADER.size:HEADER.size + length].decode("utf-8", "replace")
    print(args)
    return 0


def write(path, args):
    encoded = args.encode("utf-8")
    if len(encoded) > MAX_ARGS - 1:
        print(f"setargs: arguments are {len(encoded)} bytes, limit is "
              f"{MAX_ARGS - 1}", file=sys.stderr)
        return 1

    block = bytearray(SECTOR)
    HEADER.pack_into(block, 0, MAGIC, len(encoded), 0)
    block[HEADER.size:HEADER.size + len(encoded)] = encoded

    with open(path, "r+b") as fh:
        fh.seek(LBA * SECTOR)
        fh.write(block)
    print(f"setargs: wrote {len(encoded)} bytes of arguments to {path}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="boot image (loader.img)")
    ap.add_argument("args", nargs="?", help="argument string to store")
    ap.add_argument("--clear", action="store_true",
                    help="remove the block, so the image boots with no arguments")
    opts = ap.parse_args()

    if opts.clear:
        return write(opts.image, "")
    if opts.args is None:
        return show(opts.image)
    return write(opts.image, opts.args)


if __name__ == "__main__":
    sys.exit(main())
