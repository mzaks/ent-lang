#!/usr/bin/env python3
"""Makes the Spiking Heidelberg Digits flat for examples/snn_train/shd.ent.

  examples/snn_train/fetch.sh            once: SHD into build/snn_train/data
  python3 -m venv build/snn_train/venv
  build/snn_train/venv/bin/pip install h5py numpy
  build/snn_train/venv/bin/python examples/snn_train/prepare.py

Each sample's spikes are put into STEPS bins of BIN seconds (a spike after
the last bin is left out; there are none at 100 bins of 14 ms, the longest
sample being 1.37 s), and a channel is on in a bin where it spiked in it,
once or more. Writes build/snn_train/data/shd.bin, the training samples
first and then the test samples:

  header, 32 bytes, little-endian u32s:
    "SHD1", train, test, steps, channels, bytes a step, bytes a record, 0
  then a record for each sample:
    the label (0..19), three bytes of nought,
    then for each step its channels, a bit each: channel c is bit c % 8
    of byte c / 8 of the step

and says how many spikes fell together into one bin.
"""

import os
import struct
import sys

import h5py
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(os.path.dirname(HERE)), "build", "snn_train", "data")
STEPS = 100
BIN = 0.014
CHANNELS = 700
STEP_BYTES = (CHANNELS + 7) // 8
RECORD = 4 + STEPS * STEP_BYTES


def records(path):
    with h5py.File(path, "r") as f:
        times = f["spikes/times"]
        units = f["spikes/units"]
        labels = f["labels"][:]
        out = bytearray(len(labels) * RECORD)
        spikes = merged = late = 0
        for i in range(len(labels)):
            t = np.asarray(times[i])
            u = np.asarray(units[i]).astype(np.int64)
            step = np.floor(t / BIN).astype(np.int64)
            inside = step < STEPS
            late += int((~inside).sum())
            step, u = step[inside], u[inside]
            bits = np.zeros((STEPS, STEP_BYTES * 8), dtype=np.uint8)
            bits[step, u] = 1
            spikes += len(u)
            merged += len(u) - int(bits.sum())
            packed = np.packbits(bits, axis=1, bitorder="little")
            base = i * RECORD
            out[base] = int(labels[i])
            out[base + 4:base + RECORD] = packed.tobytes()
        return len(labels), bytes(out), spikes, merged, late


def main():
    train_n, train, s1, m1, l1 = records(os.path.join(DATA, "shd_train.h5"))
    test_n, test, s2, m2, l2 = records(os.path.join(DATA, "shd_test.h5"))
    header = b"SHD1" + struct.pack("<7I", train_n, test_n, STEPS, CHANNELS,
                                    STEP_BYTES, RECORD, 0)
    target = os.path.join(DATA, "shd.bin")
    with open(target, "wb") as f:
        f.write(header)
        f.write(train)
        f.write(test)
    spikes, merged, late = s1 + s2, m1 + m2, l1 + l2
    print(f"{target}: {train_n} training and {test_n} test samples, "
          f"{STEPS} steps of {BIN * 1000:g} ms, {CHANNELS} channels, "
          f"{len(header) + len(train) + len(test)} bytes")
    print(f"{spikes} spikes, {merged} ({100 * merged / spikes:.1f}%) in a bin "
          f"with another of their channel, {late} after the last bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
