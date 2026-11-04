# Makes profile-picture.png, the picture examples/clay/sidebar.ent shows:
# a head and shoulders drawn in greys on a round field, 300 by 300,
# with nothing (alpha 0) outside the circle. It is ent-lang's own, in
# place of the photograph Clay's example has. Edges are smoothed by
# taking 4 by 4 samples in each pixel.
#
#   python3 examples/clay/make_picture.py examples/clay/profile-picture.png
import math
import struct
import sys
import zlib

SIZE = 300
SAMPLES = 4


def colour(x, y):
    """The colour (r, g, b, a) at the point x, y (0 to 1 across)."""
    dx, dy = x - 0.5, y - 0.5
    if dx * dx + dy * dy > 0.25:
        return (0, 0, 0, 0)
    # The field: a grey lighter at its top.
    shade = 200 - 60 * y
    r = g = b = shade
    # The shoulders: the top of a wide ellipse rising from the bottom.
    sx, sy = (x - 0.5) / 0.42, (y - 1.05) / 0.38
    if sx * sx + sy * sy < 1:
        r = g = b = 70 + 25 * (1 - y)
    # The neck, then the head: an ellipse a little higher than wide.
    if abs(x - 0.5) < 0.08 and 0.55 < y < 0.75:
        r = g = b = 95
    hx, hy = (x - 0.5) / 0.17, (y - 0.42) / 0.21
    if hx * hx + hy * hy < 1:
        # Lit from the upper left.
        light = max(0.0, -0.5 * hx - 0.6 * hy + 0.6)
        r = g = b = 105 + 60 * light
    return (r, g, b, 255)


def main(path):
    rows = bytearray()
    for py in range(SIZE):
        rows.append(0)  # (no filter)
        for px in range(SIZE):
            total = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    c = colour((px + (sx + 0.5) / SAMPLES) / SIZE,
                               (py + (sy + 0.5) / SAMPLES) / SIZE)
                    # (Colours weighed by how much of them shows.)
                    for k in range(3):
                        total[k] += c[k] * c[3]
                    total[3] += c[3]
            alpha = total[3] / (SAMPLES * SAMPLES)
            for k in range(3):
                rows.append(round(total[k] / total[3]) if total[3] else 0)
            rows.append(round(alpha))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6,
                                             0, 0, 0)))
        out.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        out.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "profile-picture.png")
