#!/usr/bin/env python3
"""The vehicles' motion against the circle's formula (checks 1.1, 1.2, 1.3).

  examples/parking/check/circle.py [one.txt all.txt]

Runs examples/parking/check/circle.ent: every vehicle of vehicles.ent
held at a steering angle and a speed (full lock to the left and a third
of it to the right, forwards and backwards) for 80 s in steps of 1/120 s.

1.1 A vehicle whose velocity at the rear axle points `rho` off its
    heading and that turns at a rate w = v cos(rho) (tan(steer) -
    tan(rho)) / wheelbase is, after a time t, at
      x0 + v/w (sin(h0 + rho + w t) - sin(h0 + rho))
      y0 - v/w (cos(h0 + rho + w t) - cos(h0 + rho))
    with the heading h0 + w t, on a circle of the radius |v/w|. Every
    pose printed is within 0.01 mm of that circle and within 0.00001
    rad of where on it it should be.
1.2 The lock the model worked out is the one this script works out of
    the vehicle's numbers, and at full lock the outer front wheel's path
    is as wide as the turning circle, within 1 mm.
1.3 On one core and on all, the same output.

Given two files, they are what the program wrote on one core and on
all (test/Integration/parking_circle.test); else it runs it. That needs
the compiler built (build/bin) and its LLVM (LLVM_PREFIX, as for
tools/ent).
"""

import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PROGRAM = os.path.join(HERE, "circle.ent")
DT = 1.0 / 120.0


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", int(bits)))[0]


def f64(bits):
    return struct.unpack("<d", struct.pack("<Q", int(bits)))[0]


def run(*flags):
    return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "run", *flags,
                           "-I", os.path.dirname(HERE), PROGRAM],
                          capture_output=True, text=True, check=True).stdout


def lock_of(wheelbase, track, circle, rear_steer):
    half, side, k = circle / 2, track / 2, math.tan(math.radians(rear_steer))
    qa, qb = 1 + k * k, 2 * (side - wheelbase * k)
    qc = side * side + wheelbase * wheelbase - half * half
    r = (math.sqrt(qb * qb - 4 * qa * qc) - qb) / (2 * qa)
    return math.atan((wheelbase - k * r) / r)


def main():
    if len(sys.argv) == 3:
        out, all_cores = open(sys.argv[1]).read(), open(sys.argv[2]).read()
    else:
        out, all_cores = run(), run("--parallel")
    failed = 0
    if all_cores != out:
        print("FAIL 1.3: one core and all cores differ")
        failed += 1
    cars, worst_place, worst_heading, worst_wide = {}, 0.0, 0.0, 0.0
    for line in out.splitlines():
        word = line.split()
        if word[0] == "car":
            number = int(word[1])
            wheelbase, track, circle, rear_steer = map(float, word[2:6])
            lock, steer, v = map(f32, word[6:9])
            x, y, heading = map(f64, word[9:12])
            want = lock_of(wheelbase, track, circle, rear_steer)
            if abs(lock - want) > 1e-6:
                print(f"FAIL 1.2: car {number} lock {lock} is not {want}")
                failed += 1
            rho = -math.radians(rear_steer) / lock * steer
            w = v * math.cos(rho) * (math.tan(steer) - math.tan(rho)) / wheelbase
            cars[number] = (circle, v, w, rho, x, y, heading)
        elif word[0] == "at":
            step, number = int(word[1]), int(word[2])
            x, y, heading = map(f64, word[3:6])
            circle, v, w, rho, x0, y0, h0 = cars[number]
            t = step * DT
            cx = x0 - v / w * math.sin(h0 + rho)
            cy = y0 + v / w * math.cos(h0 + rho)
            off = abs(math.hypot(x - cx, y - cy) - abs(v / w))
            along = math.atan2((y - cy) * math.copysign(1, v / w), (x - cx) * math.copysign(1, v / w))
            slip = (along - (h0 + rho + w * t - math.pi / 2) + math.pi) % (2 * math.pi) - math.pi
            turn = (heading - (h0 + w * t) + math.pi) % (2 * math.pi) - math.pi
            turn = max(abs(turn), abs(slip))
            worst_place, worst_heading = max(worst_place, off), max(worst_heading, turn)
            if off > 0.00001 or turn > 0.00001:
                print(f"FAIL 1.1: car {number} step {step} is {off * 1000:.4f} mm off its "
                      f"circle and {turn:.7f} rad along it")
                failed += 1
        elif word[0] == "wide" and int(word[1]) % 4 < 2:
            number, wide = int(word[1]), f32(word[2])
            circle = cars[number][0]
            worst_wide = max(worst_wide, abs(wide - circle))
            if abs(wide - circle) > 0.001:
                print(f"FAIL 1.2: car {number} turns in {wide:.4f} m, not {circle}")
                failed += 1
    print(f"{len(cars)} vehicles: at most {worst_place * 1000:.5f} mm off their circles and "
          f"{worst_heading:.8f} rad along them, turning circles within {worst_wide * 1000:.3f} mm")
    if failed:
        sys.exit(f"{failed} failed")
    print("ok")


if __name__ == "__main__":
    main()
