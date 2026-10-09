"""Writes a small WAD, `doom1.wad`, into the directory it is given: a
palette and one level, E1M1, of a square room with a step in it
(doom_level.test)."""
import struct
import sys


def name(text):
    return text.encode().ljust(8, b"\0")


def main():
    vertexes = [(0, 0), (256, 0), (256, 256), (0, 256), (128, 0), (128, 256)]
    # floor, ceiling, light
    sectors = [(0, 128, 160), (24, 128, 200)]
    # (sector)
    sides = [0, 0, 1, 1, 0, 1, 0]
    # v1, v2, flags, special, tag, front, back
    lines = [
        (0, 4, 1, 0, 0, 0, -1), (4, 1, 1, 0, 0, 2, -1),
        (1, 2, 1, 0, 0, 3, -1), (2, 5, 1, 0, 0, 2, -1),
        (5, 3, 1, 0, 0, 1, -1), (3, 0, 1, 0, 0, 0, -1),
        # The step, with two sides; and a line the map does not show.
        (4, 5, 4, 0, 0, 4, 5), (0, 2, 4 | 128, 0, 0, 6, -1),
    ]
    lumps = [
        ("PLAYPAL", bytes(v for i in range(256)
                          for v in (i, 255 - i, i // 2))),
        ("E1M1", b""),
        ("THINGS", b""),
        ("LINEDEFS", b"".join(struct.pack("<7h", *line) for line in lines)),
        ("SIDEDEFS", b"".join(
            struct.pack("<hh8s8s8sh", 0, 0, name("-"), name("-"),
                        name("STARTAN3"), sector) for sector in sides)),
        ("VERTEXES", b"".join(struct.pack("<hh", *v) for v in vertexes)),
        ("SEGS", b""), ("SSECTORS", b""), ("NODES", b""),
        ("SECTORS", b"".join(
            struct.pack("<hh8s8shhh", floor, ceiling, name("FLOOR4_8"),
                        name("CEIL3_5"), light, 0, 0)
            for floor, ceiling, light in sectors)),
        ("REJECT", b""), ("BLOCKMAP", b""),
    ]
    body = b"".join(data for _, data in lumps)
    table = b""
    at = 12
    for lump, data in lumps:
        table += struct.pack("<ii8s", at, len(data), name(lump))
        at += len(data)
    with open(sys.argv[1] + "/doom1.wad", "wb") as file:
        file.write(b"IWAD" + struct.pack("<ii", len(lumps), 12 + len(body)))
        file.write(body + table)


if __name__ == "__main__":
    main()
