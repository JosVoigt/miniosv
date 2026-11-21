#!/usr/bin/env python3
"""Zero the first <GiB> of a devdax device before it backs a new guest.

Guest RAM backed by devdax (run.py --mem-path /dev/dax0.0) keeps whatever the
previous VM left in it; anonymous RAM is zero. OVMF can hang on such a dirty
boot, so scripts/lsched-run.sh clears the region before every boot.

    dax-zero.py /dev/dax0.0 8
"""
import mmap
import os
import sys

dev, gib = sys.argv[1], int(sys.argv[2])
size = gib << 30
fd = os.open(dev, os.O_RDWR)
try:
    m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    chunk = bytes(1 << 26)
    for off in range(0, size, len(chunk)):
        m[off:off + len(chunk)] = chunk
    m.close()
finally:
    os.close(fd)
