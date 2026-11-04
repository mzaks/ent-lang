# The .png files examples/rust/pngs.ent reads: one of every colour
# type and bit depth, plain and interlaced (Adam7), with all five row
# filters, a palette with tRNS, and not_a.png, which is none; written
# into the directory given:
#
#   python3 examples/rust/make_pngs.py DIR
import os, random, struct, sys, zlib

random.seed(7)
CH = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)


def pack_row(samples, d):
    if d == 8:
        return bytes(samples)
    if d == 16:
        return b''.join(struct.pack('>H', s) for s in samples)
    out = bytearray(); acc = 0; n = 0
    for s in samples:
        acc = (acc << d) | s; n += d
        if n == 8:
            out.append(acc); acc = 0; n = 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def filt(raw, bpp, ft, prev):
    out = bytearray()
    for i, x in enumerate(raw):
        a = raw[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if (prev and i >= bpp) else 0
        if ft == 0: p = 0
        elif ft == 1: p = a
        elif ft == 2: p = b
        elif ft == 3: p = (a + b) // 2
        else:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
        out.append((x - p) & 255)
    return bytes(out)


def encode(img, ct, d):
    bpp = max(1, (CH[ct] * d) // 8); data = bytearray(); prev = None
    for y, row in enumerate(img):
        raw = pack_row(row, d); ft = y % 5
        data.append(ft); data += filt(raw, bpp, ft, prev); prev = raw
    return bytes(data)


def write(name, w, h, ct, d, interlace=False, trns=False):
    mx = (1 << d) - 1
    img = [[random.randint(0, mx) if random.random() < 0.5 else (x // CH[ct]) * mx // (w - 1)
            for x in range(w * CH[ct])] for y in range(h)]
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, d, ct, 0, 0, int(interlace)))
    if ct == 3:
        out += chunk(b'PLTE', bytes(random.randint(0, 255) for _ in range(3 * (mx + 1))))
        if trns:
            out += chunk(b'tRNS', bytes(random.randint(0, 255) for _ in range(mx + 1)))
    if not interlace:
        data = encode(img, ct, d)
    else:
        data = bytearray()
        for xs, ys, dx, dy in [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
                               (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]:
            sub = [[img[y][x * CH[ct] + k] for x in range(xs, w, dx) for k in range(CH[ct])]
                   for y in range(ys, h, dy)]
            if sub and sub[0]:
                data += encode(sub, ct, d)
    out += chunk(b'IDAT', zlib.compress(bytes(data), 6)) + chunk(b'IEND', b'')
    open(name, 'wb').write(out)


os.chdir(sys.argv[1])
n = 0
for ct, ds in {0: [1, 2, 4, 8, 16], 2: [8, 16], 3: [1, 2, 4, 8], 4: [8, 16], 6: [8, 16]}.items():
    for d in ds:
        write(f'k{ct}_{d}.png', 37, 23, ct, d)
        write(f'k{ct}_{d}_i.png', 37, 23, ct, d, interlace=True)
        n += 2
write('k3_8_trns.png', 37, 23, 3, 8, trns=True)
open('not_a.png', 'wb').write(b'not a picture\n')
print(n + 1, 'pngs')
