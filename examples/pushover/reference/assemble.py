#!/usr/bin/env python3
"""Puts the pictures of the dominoes into one, as the game's own
data/sources/assembler.cpp does: `assemble.py OUT.png HEIGHT GAP WIDTH
LIST`, where each line of LIST is `x y clip FILE` (lines that start with
a # say nothing). Every picture gets HEIGHT rows of OUT and GAP rows of
black under them, one under the other in the list's order: at `x`, and
so that its row `HEIGHT - clip - y` is the first of its rows.

Reads and writes PNGs with what Python has (no library needed)."""
import struct
import sys
import zlib


def read_png(path):
    """Width, height and the rows as RGBA bytes."""
    data = open(path, "rb").read()
    at = 8
    packed = b""
    palette = None
    clear = None
    while at < len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        chunk = data[at + 8:at + 8 + size]
        at += 12 + size
        if kind == b"IHDR":
            width, height, depth, colour, _, _, laced = struct.unpack(">IIBBBBB", chunk)
            if depth != 8 or laced or colour not in (2, 3, 6):
                raise SystemExit(f"{path}: a kind of PNG this does not read")
        elif kind == b"PLTE":
            palette = chunk
        elif kind == b"tRNS":
            clear = chunk
        elif kind == b"IDAT":
            packed += chunk
    step = {2: 3, 3: 1, 6: 4}[colour]
    raw = zlib.decompress(packed)
    stride = width * step
    before = bytearray(stride)
    rows = []
    at = 0
    for _ in range(height):
        how = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            left = line[i - step] if i >= step else 0
            up = before[i]
            corner = before[i - step] if i >= step else 0
            if how == 1:
                line[i] = (line[i] + left) & 255
            elif how == 2:
                line[i] = (line[i] + up) & 255
            elif how == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif how == 4:
                guess = left + up - corner
                a, b, c = abs(guess - left), abs(guess - up), abs(guess - corner)
                line[i] = (line[i] + (left if a <= b and a <= c else up if b <= c else corner)) & 255
        before = line
        if colour == 6:
            rows.append(bytes(line))
        elif colour == 2:
            rows.append(b"".join(line[i:i + 3] + b"\xff" for i in range(0, stride, 3)))
        else:
            rows.append(b"".join(
                palette[3 * v:3 * v + 3] + bytes([clear[v] if clear and v < len(clear) else 255])
                for v in line))
    return width, height, rows


def write_png(path, width, height, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + bytes(row) for row in rows)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        out.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        out.write(chunk(b"IEND", b""))


def main():
    out, height, gap, wide, listed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    pictures = []
    for line in open(listed):
        if line.startswith("#") or not line.strip():
            continue
        x, y, clip, name = line.split()
        pictures.append((int(x), int(y), int(clip), name))
    rows = [bytearray(wide * 4) for _ in range((height + gap) * len(pictures))]
    top = 0
    for x, y, clip, name in pictures:
        width, high, lines = read_png(name)
        crop = height - clip
        first = crop - y
        to = top + crop
        if first < 0:
            to -= first
            first = 0
        for line in lines[first:]:
            if 0 <= to < len(rows):
                span = line[:max(0, min(width, wide - x)) * 4]
                rows[to][x * 4:x * 4 + len(span)] = span
            to += 1
        for under in range(top + height, top + height + gap):
            rows[under][:] = b"\0\0\0\xff" * wide
        top += height + gap
    write_png(out, wide, len(rows), rows)
    print(f"{out}: {len(pictures)} pictures, {wide} by {len(rows)}")


main()
