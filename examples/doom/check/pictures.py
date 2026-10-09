#!/usr/bin/env python3
"""Holds two files of pictures against each other, pixel for pixel.

  examples/doom/check/pictures.py reference.raw port.raw [known]

Each file is pictures of 320 by 200 bytes, one after another. Says which
pictures differ and in how many pixels, and fails if more differ than
`known` (0 where it is not given): the number of pixels that are known
to differ, for a reason that is written down where this is called.
"""
import sys

SIZE = 320 * 200


def main():
    with open(sys.argv[1], "rb") as file:
        reference = file.read()
    with open(sys.argv[2], "rb") as file:
        port = file.read()
    known = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    if len(reference) != len(port):
        sys.exit(f"{len(reference) // SIZE} pictures in the reference, "
                 f"{len(port) // SIZE} in the port")
    pictures = len(reference) // SIZE
    differ = 0
    for picture in range(pictures):
        a = reference[picture * SIZE:(picture + 1) * SIZE]
        b = port[picture * SIZE:(picture + 1) * SIZE]
        if a == b:
            continue
        pixels = [i for i in range(SIZE) if a[i] != b[i]]
        differ += len(pixels)
        first = pixels[0]
        print(f"picture {picture}: {len(pixels)} pixels differ, the first "
              f"at ({first % 320}, {first // 320}): {a[first]} in the "
              f"reference, {b[first]} in the port")
    if differ > known:
        sys.exit(f"{differ} pixels differ, {known} are known to")
    print(f"the same: {pictures} pictures, pixel for pixel"
          + (f" but for the {differ} known" if differ else ""))


if __name__ == "__main__":
    main()
