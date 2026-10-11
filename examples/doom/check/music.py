#!/usr/bin/env python3
"""Holds what the port writes to the chip against what the reference does.

  examples/doom/check/music.py opl.reference opl.port [seconds]

The reference's lines are a time in microseconds and a register and a
value in hex; the port's a time in samples (44100 a second) and the two
in decimal. The values must be the same in the same order, and the time
from each to the next the same but for what the reference's clock
rounds (it steps a sample at a time, and counts in whole microseconds).
"""
import sys

ALLOWED = 60  # microseconds


def main():
    reference = [line.split() for line in open(sys.argv[1])]
    port = [line.split() for line in open(sys.argv[2])]
    reference = [(int(t), int(r, 16), int(v, 16)) for t, r, v in reference]
    port = [(int(t) * 1000000 / 44100, int(r), int(v)) for t, r, v in port]
    for i, (a, b) in enumerate(zip(reference, port)):
        if a[1:] != b[1:]:
            sys.exit(f"write {i} differs: the reference writes {a[2]:x} to "
                     f"{a[1]:x} at {a[0] / 1e6:.3f} s, the port {b[2]:x} to "
                     f"{b[1]:x} at {b[0] / 1e6:.3f} s")
        if i > 0:
            ours = b[0] - port[i - 1][0]
            theirs = a[0] - reference[i - 1][0]
            if abs(ours - theirs) > ALLOWED:
                sys.exit(f"write {i} ({a[2]:x} to {a[1]:x}) comes "
                         f"{theirs} microseconds after the one before in "
                         f"the reference, {ours:.0f} in the port")
    # (Where the time that is played ends on a tic of the tune, one of
    # the two may have that tic's writes and the other not.)
    longer = reference if len(reference) > len(port) else port
    end = float(sys.argv[3]) * 1e6 if len(sys.argv) > 3 else None
    more = longer[min(len(reference), len(port)):]
    if more and (end is None or min(w[0] for w in more) < end - 2000):
        sys.exit(f"{len(reference)} writes in the reference, {len(port)} "
                 f"in the port (the same as far as both go)")
    seconds = reference[-1][0] / 1e6 if reference else 0
    print(f"the same: {min(len(port), len(reference))} writes over "
          f"{seconds:.0f} seconds")


if __name__ == "__main__":
    main()
