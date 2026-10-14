#!/usr/bin/env python3
"""On any number of cores the waves are the same, to the last bit (the
spec's check 6).

  examples/analog/check/same.py [circuits]

The model is built for one core and for all of them (--parallel); every
circuit named (by default a few: RC circuits, a ladder, two DC circuits,
memristors of two models, crossbars with wires) runs every way it can, on the one-core build and on
the all-cores build with 1, 4, 16 and all threads (OMP_NUM_THREADS), and
every "waves.txt" is to be the one-core build's, byte for byte.
"""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

CIRCUITS = ["rc_step", "ladder", "r2r", "bridge", "memristor_series", "memristor_yakopcic",
            "crossbar_4_10_biolek_write", "crossbar_4_10_vteam_write",
            "crossbar_16_10_biolek_read"]


def main():
    names = sys.argv[1:] or [n for n in CIRCUITS
                             if os.path.isdir(os.path.join(common.CIRCUITS, n))]
    one = common.build()
    all_cores = common.build(["--parallel"])
    threads = ["1", "4", "16", str(os.cpu_count())]
    failed = 0
    for name in names:
        report = []
        for way in (0, 1, 2):
            wanted, said = common.run(one, name, way)
            if "does not do" in said:
                continue
            differ = [count for count in threads
                      if common.run(all_cores, name, way,
                                    environment=dict(os.environ, OMP_NUM_THREADS=count))[0]
                      != wanted]
            if differ or not wanted:
                failed += 1
                report.append(f"way {way} NOT the same on {', '.join(differ)} threads")
            else:
                report.append(f"way {way} the same ({len(wanted)} time points)")
        print(f"{name}: " + "; ".join(report))
    print(f"{len(names)} circuits, {failed} ways not the same on every count of threads")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
