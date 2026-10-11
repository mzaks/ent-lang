#!/usr/bin/env python3
"""How near the port's chip sounds to the reference's, in numbers.

  examples/doom/check/music_sound.py music.reference.raw music.port.raw

Both are samples of sixteen bits, 44100 a second: the reference's in two
channels (the left is taken), the port's in one. The port's chip does
not make the same samples, so what is compared is how loud the two are
from one moment to the next, and how alike their spectra are (1: the
same) and how strong each range of pitches is over the whole.
"""
import array
import cmath
import math
import sys

WINDOW = 2048
EDGES = [0, 200, 400, 800, 1600, 3200, 6400, 22050]


def load(name, channels):
    samples = array.array("h")
    samples.frombytes(open(name, "rb").read())
    return samples[::channels]


def rms(samples):
    return math.sqrt(sum(v * v for v in samples) / max(len(samples), 1))


def correlation(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    over = math.sqrt(sum((p - ma) ** 2 for p in a) *
                     sum((q - mb) ** 2 for q in b)) + 1e-9
    return sum((p - ma) * (q - mb) for p, q in zip(a, b)) / over


def fft(x):
    n = len(x)
    if n == 1:
        return x
    even, odd = fft(x[0::2]), fft(x[1::2])
    turned = [cmath.exp(-2j * cmath.pi * k / n) * odd[k] for k in range(n // 2)]
    return ([even[k] + turned[k] for k in range(n // 2)] +
            [even[k] - turned[k] for k in range(n // 2)])


def main():
    reference, port = load(sys.argv[1], 2), load(sys.argv[2], 1)
    n = min(len(reference), len(port))
    print(f"{n / 44100:.0f} seconds; as loud: the reference "
          f"{rms(reference[:n]):.0f}, the port {rms(port[:n]):.0f}")
    a = [rms(reference[i:i + WINDOW]) for i in range(0, n - WINDOW, WINDOW)]
    b = [rms(port[i:i + WINDOW]) for i in range(0, n - WINDOW, WINDOW)]
    print(f"loudness from moment to moment goes together by "
          f"{correlation(a, b):.3f}")
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * i / WINDOW) for i in range(WINDOW)]

    def spectrum(samples, at):
        return [abs(v) for v in
                fft([samples[at + k] * hann[k] for k in range(WINDOW)])[:WINDOW // 2]]

    bands_a, bands_b, alike = [0] * 7, [0] * 7, []
    for at in range(0, n - WINDOW, WINDOW * 8):
        sa, sb = spectrum(reference, at), spectrum(port, at)
        na = math.sqrt(sum(v * v for v in sa))
        nb = math.sqrt(sum(v * v for v in sb))
        if na > 1e3 and nb > 1e3:
            alike.append(sum(p * q for p, q in zip(sa, sb)) / (na * nb))
        for k in range(WINDOW // 2):
            pitch = k * 44100 / WINDOW
            for band in range(7):
                if EDGES[band] <= pitch < EDGES[band + 1]:
                    bands_a[band] += sa[k] ** 2
                    bands_b[band] += sb[k] ** 2
    if alike:
        print(f"spectra of {len(alike)} moments are alike by "
              f"{sum(alike) / len(alike):.3f}")
    for band in range(7):
        print(f"  {EDGES[band]:5d} to {EDGES[band + 1]:5d} Hz: the port is "
              f"{10 * math.log10((bands_b[band] + 1) / (bands_a[band] + 1)):+.1f} dB")


if __name__ == "__main__":
    main()
