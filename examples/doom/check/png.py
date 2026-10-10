#!/usr/bin/env python3
"""Pictures to look at, of a file of screens (320 by 200 bytes each).

  examples/doom/check/png.py doom1.wad shots.port out [other.raw]

Writes out0.png, out1.png, ..: each screen in the WAD's colours, twice
as big. With another file of screens, the two side by side and after
them what differs, in white.
"""
import struct
import sys
import zlib

W, H = 320, 200


def palette(wad):
    data = open(wad, "rb").read()
    count, table = struct.unpack_from("<ii", data, 4)
    for lump in range(count):
        at, size = struct.unpack_from("<ii", data, table + lump * 16)
        if data[table + lump * 16 + 8:table + lump * 16 + 16].rstrip(b"\0") == b"PLAYPAL":
            return data[at:at + 768]
    sys.exit("no PLAYPAL")


def png(name, width, height, rgb):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    with open(name, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" +
                   chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    pal = palette(sys.argv[1])
    one = open(sys.argv[2], "rb").read()
    two = open(sys.argv[4], "rb").read() if len(sys.argv) > 4 else None
    for n in range(len(one) // (W * H)):
        a = one[n * W * H:(n + 1) * W * H]
        panels = [a]
        if two:
            b = two[n * W * H:(n + 1) * W * H]
            panels.append(b)
        width = W * 2 * (3 if two else 1)
        out = bytearray(width * H * 2 * 3)
        for y in range(H * 2):
            for p, panel in enumerate(panels):
                for x in range(W * 2):
                    c = panel[(y // 2) * W + x // 2]
                    at = (y * width + p * W * 2 + x) * 3
                    out[at:at + 3] = pal[c * 3:c * 3 + 3]
            if two:
                for x in range(W * 2):
                    i = (y // 2) * W + x // 2
                    if panels[0][i] != panels[1][i]:
                        at = (y * width + 2 * W * 2 + x) * 3
                        out[at:at + 3] = b"\xff\xff\xff"
        png(f"{sys.argv[3]}{n}.png", width, H * 2, bytes(out))


if __name__ == "__main__":
    main()
