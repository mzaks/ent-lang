"""The CL API against the ent-lang dish (ent_dish.py): a loop at 1000 ticks a
second, a stim on one channel every 100 ms, the spikes counted on every
channel, and all of it recorded to HDF5.

    build/cl_bridge/venv/bin/python examples/cl_bridge/demo_loop.py [seconds [seed [channel]]]

Runs in accelerated time (CL_SDK_ACCELERATED_TIME=1, set here unless the
environment says otherwise). Says how many spikes the stimulated channel
had in the 20 ms after each stim and in the other 80 ms of each 100,
per millisecond, so the evoked response shows against the channel's own
background; and the channels that fired most.
"""

import os
import sys

os.environ.setdefault("CL_SDK_ACCELERATED_TIME", "1")

import cl  # noqa: E402

import ent_dish  # noqa: E402

TICKS_PER_SECOND = 1000
PERIOD = 100     # ticks between stims
WINDOW = 20      # ticks after a stim that count as its response


def main() -> int:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    channel = int(sys.argv[3]) if len(sys.argv) > 3 else 27

    attributes = ent_dish.use(seed=seed)
    counts = [0] * 64
    after = between = 0
    stims = 0
    with cl.open() as neurons:
        # (No raw samples: the dish gives spikes only, its frames are zeros.)
        recording = neurons.record(file_suffix=f"ent_dish_seed{seed}",
                                   attributes=attributes,
                                   include_raw_samples=False)
        for tick in neurons.loop(TICKS_PER_SECOND, stop_after_seconds=seconds):
            phase = tick.iteration % PERIOD
            for spike in tick.analysis.spikes:
                counts[spike.channel] += 1
                if spike.channel == channel:
                    if phase < WINDOW and stims:
                        after += 1
                    elif stims:
                        between += 1
            if phase == 0:
                neurons.stim(channel, 1.0)
                stims += 1
        recording.stop()
        path = recording.file["path"] if isinstance(getattr(recording, "file", None), dict) else None

    total = sum(counts)
    print(f"seed {seed}: {seconds:g} s, {total} spikes, {stims} stims on channel {channel}")
    rate_after = after / (stims * WINDOW)
    rate_between = between / (stims * (PERIOD - WINDOW))
    print(f"channel {channel}: {after} spikes in the {WINDOW} ms after a stim "
          f"({rate_after:.3f} a ms), {between} in the rest ({rate_between:.3f} a ms)")
    top = sorted(range(64), key=lambda c: -counts[c])[:5]
    print("most spikes: " + ", ".join(f"channel {c} {counts[c]}" for c in top))
    if path:
        print(f"recorded to {path}")
    print("evoked response: " + ("yes" if rate_after > 3 * rate_between else "NO"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
