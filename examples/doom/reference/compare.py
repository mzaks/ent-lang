#!/usr/bin/env python3
"""The first tic at which two traces differ, and in what.

  examples/doom/reference/compare.py build/doom/run/demo1.trace port.trace

A trace has a line for each tic of a level (see trace.patch): episode, map,
tic, the index into the random numbers, where the player is (x, y, z,
angle), the player's health, how many map objects there are and a hash of
them all, which does not depend on their order.
"""
import sys

FIELDS = ["episode", "map", "tic", "random", "x", "y", "z", "angle",
          "health", "objects", "hash"]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1]) as file:
        reference = [line.split() for line in file]
    with open(sys.argv[2]) as file:
        port = [line.split() for line in file]
    for number, (want, got) in enumerate(zip(reference, port)):
        if want == got:
            continue
        print(f"differs at line {number + 1} (tic {want[2]}), "
              f"after {number} tics that are the same:")
        for name, a, b in zip(FIELDS, want, got):
            if a != b:
                print(f"  {name}: {a} in the reference, {b} in the port")
        sys.exit(1)
    if len(reference) != len(port):
        print(f"the same for {min(len(reference), len(port))} tics, but the "
              f"reference has {len(reference)} and the port {len(port)}")
        sys.exit(1)
    print(f"the same: {len(reference)} tics")


if __name__ == "__main__":
    main()
