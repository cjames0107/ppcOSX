#!/usr/bin/env python3
"""Compare the pictures written by guest/tests/glcompare for two renderers.

    tools/glcompare.py A_DIR B_DIR [OUT.png]

For each scene, prints the mean absolute difference per channel and the
share of pixels differing by more than 24 in any channel. With OUT.png,
also writes a contact sheet: per scene, A, B and the difference (x4),
scaled up 3x.
"""
import os
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    a_dir, b_dir = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    names = sorted(n for n in os.listdir(a_dir) if n.endswith(".ppm"))
    sheet = []
    scale = 3
    worst = 0.0
    for name in names:
        bpath = os.path.join(b_dir, name)
        if not os.path.exists(bpath):
            print("%-20s missing in %s" % (name, b_dir))
            continue
        w, h, a = read_ppm(os.path.join(a_dir, name))
        _, _, b = read_ppm(bpath)
        diff = [abs(x - y) for x, y in zip(a, b)]
        mean = [sum(diff[c::3]) / (w * h) for c in range(3)]
        bad = sum(1 for i in range(0, len(diff), 3)
                  if max(diff[i:i + 3]) > 24) / (w * h)
        worst = max(worst, bad)
        print("%-20s mean diff %5.1f %5.1f %5.1f   pixels off %5.1f%%"
              % (name[:-4], mean[0], mean[1], mean[2], bad * 100))
        if out:
            for y in range(h):
                row = []
                for img in (a, b, bytes(min(255, d * 4) for d in diff)):
                    for x in range(w):
                        px = img[(y * w + x) * 3:(y * w + x) * 3 + 3]
                        row += list(px) * scale
                    row += [255, 255, 255] * 4
                for _ in range(scale):
                    sheet.append(row)
            for _ in range(4):
                sheet.append([255] * len(sheet[-1]))
    if out and sheet:
        write_png(out, len(sheet[0]) // 3, len(sheet), sheet)
    print("worst scene: %.1f%% of pixels off" % (worst * 100))


if __name__ == "__main__":
    main()
