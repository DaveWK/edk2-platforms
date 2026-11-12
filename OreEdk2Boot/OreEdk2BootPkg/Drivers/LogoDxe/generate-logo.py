#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 DaveWK
"""Generate the original unbranded RV2 splash bitmap from simple geometry.

No imported images, fonts or external assets. The bitmap is GPL-2.0-only,
like this generator. The native Logo DXE implementation retains its BSD notices.
"""
import argparse
from pathlib import Path
import struct

OUTPUT = Path(__file__).resolve().parent / 'Logo.bmp'


def bitmap():
    width = height = 128
    stride = (width * 3 + 3) & ~3
    rows = bytearray()
    for y in range(height - 1, -1, -1):
        row = bytearray()
        for x in range(width):
            # A thin square frame and three ascending bars, all integer geometry.
            frame = (12 <= x < 116 and y in (12, 13, 114, 115)) or (12 <= y < 116 and x in (12, 13, 114, 115))
            bars = (28 <= x < 44 and 76 <= y < 100) or (56 <= x < 72 and 56 <= y < 100) or (84 <= x < 100 and 36 <= y < 100)
            r, g, b = (242, 128, 36) if frame or bars else (20, 28, 36)
            row.extend((b, g, r))
        rows.extend(row + bytes(stride - len(row)))
    size = 54 + len(rows)
    file_header = struct.pack('<2sIHHI', b'BM', size, 0, 0, 54)
    dib = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(rows), 2835, 2835, 0, 0)
    return file_header + dib + rows


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='verify the tracked bitmap without writing')
    args = parser.parse_args()
    expected = bitmap()
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_bytes() != expected:
            raise SystemExit('RV2 procedural bitmap differs; run scripts/generate-logo.py')
        print('PASS original procedural RV2 bitmap')
    else:
        OUTPUT.write_bytes(expected)
        print('Generated ' + OUTPUT.name)
