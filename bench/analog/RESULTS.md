# The analog circuit: timings

`examples/analog` against ngspice 47 (KLU, the memristors compiled by
OpenVAF) and Xyce 7.10 (serial, KLU, Yakopcic's memristor built in), on
the Ryzen AI 9 HX 370.

## 2026-10-15, first

`python3 bench/analog/run.py`, run in a scope of 14 GB
(`systemd-run --user --scope -p MemoryMax=14G -p MemorySwapMax=1G`):
crossbars of Yakopcic's memristors, N by N, with wires of 0 to 10 ohm,
read and written, and the IBM power grids ibmpg1 and ibmpg2 (their
operating point). A run is a process of its own, its whole time
(reading the circuit included), at SPICE's tolerances; one core is CPU
2, all cores CPUs 0 to 3 (the Zen 5 ones), the model built
`-march=native`. A run is not timed where its waves are more than 1e-2
of a probe's range off the reference (1e-5 V on a grid: "off"), and
where it is past 600 s it is not run on the larger crossbars of the same
wires and scenario. The reference is Xyce at tight tolerances (check 5)
where it is done in 900 s, else way 0 at reltol 1e-5; for a grid the
published solution.

| circuit | nodes | way 0 | way 1 | way 2 | way 0, all cores | way 1, all cores | way 2, all cores | ngspice | Xyce |
|---|---|---|---|---|---|---|---|---|---|
| 16, 0 ohm, read | 32 | 122 ms | 102 ms | 101 ms | 125 ms | 105 ms | 106 ms | 109 ms | 61 ms |
| 16, 0 ohm, write | 20 | 120 ms | 106 ms | 111 ms | 121 ms | 114 ms | 120 ms | off 1.8e-2 | 75 ms |
| 16, 0.1 ohm, read | 544 | 144 ms | 155 s | > 600 s | 155 ms | 246 s | > 600 s | 149 ms | 128 ms |
| 16, 0.1 ohm, write | 532 | 162 ms | > 600 s | > 600 s | 173 ms | > 600 s | > 600 s | off 1.3e-2 | > 600 s |
| 16, 1 ohm, read | 544 | 155 ms | 2.08 s | 8.99 s | 157 ms | 2.92 s | 9.05 s | 148 ms | 125 ms |
| 16, 1 ohm, write | 532 | 161 ms | 18.54 s | 66.86 s | 173 ms | 25.52 s | 70.40 s | 164 ms | 165 ms |
| 16, 10 ohm, read | 544 | 147 ms | 1.07 s | 5.60 s | 158 ms | 1.50 s | 5.50 s | 144 ms | 125 ms |
| 16, 10 ohm, write | 532 | off 1.0e-2 | off 1.0e-2 | off 1.0e-2 | off 1.0e-2 | off 1.0e-2 | off 1.0e-2 | off 1.4e-2 | off 1.0e-2 |
| 64, 0 ohm, read | 128 | 502 ms | 257 ms | 307 ms | 520 ms | 294 ms | 341 ms | 764 ms | 566 ms |
| 64, 0 ohm, write | 80 | 472 ms | 364 ms | 441 ms | 489 ms | 419 ms | 486 ms | off 2.0e-2 | 753 ms |
| 64, 0.1 ohm, read | 8,320 | 2.22 s | > 600 s | > 600 s | 2.26 s | > 600 s | > 600 s | 4.64 s | 4.07 s |
| 64, 0.1 ohm, write | 8,272 | 3.35 s | > 600 s | > 600 s | 3.38 s | > 600 s | > 600 s | 5.71 s | > 600 s |
| 64, 1 ohm, read | 8,320 | 2.21 s | > 600 s | > 600 s | 2.24 s | > 600 s | > 600 s | 4.63 s | 3.98 s |
| 64, 1 ohm, write | 8,272 | off 1.8e-2 | > 600 s | > 600 s | off 1.8e-2 | > 600 s | > 600 s | off 2.4e-2 | off 2.0e-2 |
| 64, 10 ohm, read | 8,320 | 2.20 s | 338 s | > 600 s | 2.26 s | 475 s | > 600 s | 4.63 s | 4.28 s |
| 64, 10 ohm, write | 8,272 | off 1.2e-2 | did not settle | > 600 s | off 1.2e-2 | > 600 s | > 600 s | off 1.9e-2 | off 2.4e-2 |
| 256, 0 ohm, read | 512 | 8.22 s | 2.82 s | 3.50 s | 7.16 s | 1.68 s | 1.79 s | 52.37 s | 20.31 s |
| 256, 0 ohm, write | 320 | 7.53 s | 4.63 s | 5.90 s | 6.18 s | 2.72 s | 2.77 s | off 2.1e-2 | 21.45 s |
| 256, 0.1 ohm, read | 131,584 | 201 s | > 600 s | > 600 s | 200 s | > 600 s | > 600 s | 472 s | 242 s |
| 256, 0.1 ohm, write | 131,392 | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s |
| 256, 1 ohm, read | 131,584 | 199 s | > 600 s | > 600 s | 199 s | > 600 s | > 600 s | 474 s | 242 s |
| 256, 1 ohm, write | 131,392 | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | off 3.4e-2 |
| 256, 10 ohm, read | 131,584 | 199 s | > 600 s | > 600 s | 200 s | > 600 s | > 600 s | 471 s | 239 s |
| 256, 10 ohm, write | 131,392 | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | > 600 s | off 4.6e-2 |
| ibmpg1 | 16,427 | 111 ms | 24.88 s | 95.78 s | 118 ms | 38.14 s | 95.48 s | 222 ms | 714 ms |
| ibmpg2 | 127,025 | 533 ms | did not settle | > 600 s | 552 ms | did not settle | > 600 s | 3.56 s | 6.25 s |

(ngspice ran the deck in which the sources of 0 V were folded into
their nodes; given them back, it takes 2 s on a written crossbar where
folded it took past 600 s, so its column for the written crossbars is
to be run again.)

(The references of the 256 crossbars written with wires are way 0's at
reltol 1e-5: Xyce at tight tolerances was not done in 900 s.)

- Way 0 is the fastest where the wires have resistance and on the
  grids: the 256 crossbars read, 200 s where Xyce takes 240 and ngspice
  470; ibmpg2 in 0.53 s, where ngspice takes 3.6 and Xyce 6.3. It is
  within 1.5e-6 of the range of Xyce's waves on the crossbars read, and
  as near the published grids as ngspice and Xyce (5e-6 V to 6e-6 V).
- Relaxation is the fastest where the wires have none: the 256 crossbar
  read in 1.7 s on four cores, way 0 8.2 s, Xyce 20 s, ngspice 52 s.
  With resistance in the wires it is not to be had: a time point takes
  hundreds to 100,000 rounds (25,000 in the mean on the 16 crossbar of
  0.1 ohm), and ibmpg2's operating point does not settle in 100,000.
- All the cores give way 0 nothing (KLU is the time, and runs on one),
  and the relaxation ways something only on the 256 crossbars without
  wires.
- Written, the waves are not to be held to 1e-2 of a range: on the 64
  crossbars of 1 and 10 ohm every one is off, Xyce at SPICE's tolerances
  included (2.0e-2 and 2.4e-2 off itself at tight ones), as are those of
  the 16 crossbar of 10 ohm, all 1.03e-2 off. Where a device switches
  its time moves with the tolerances, and the waves near it apart.
  ngspice is off on most written ones.
- Way 0 takes 780 MB to 1.1 GB, where ngspice takes 17 MB to 1.9 GB:
  most likely the capacities of `simulate.ent` (for a power grid of 2.2
  million nodes), not yet looked into.
- Written with wires, the 256 crossbars are past 600 s for all, and way 0
  where it takes 200 s read: still to be seen to.

Before this run, the first one was killed by the kernel short of memory:
three Xyce runs of other checks beside it, one of the 1024 crossbar at
9.9 GB. Xyce now runs one at a time across processes, held to 12 GB
(`ANALOG_XYCE_GB`; check/xyce.py).
