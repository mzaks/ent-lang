#!/usr/bin/env python3
"""The Mojo device on the GPU against ent on the CPU, by size of job.

Each measurement runs in its own process, round-robin; the first job of a
process opens the GPU and is reported apart. Warm is the second half of a
process's jobs, of which it does enough to run for most of a second: a
core just woken is not up to speed yet. All is built native. One thread runs on one Zen 5 core, a run with the GPU on all
of them, since its worker inherits the mask and must not share the ticks'
core; all threads are left to the runtime. Stops unless on mains power and
unless every variant gives the same steps.

  examples/mojo/setup.sh           once
  python3 bench/mojo/run.py        [--repeats 5]
"""
import argparse
import os
import re
import statistics
import time
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "bench", "mojo")
WORK = os.path.join(ROOT, "build", "bench-mojo")
ENT = os.path.join(ROOT, "tools", "ent")
SIZES = [100000, 1000000, 4000000]
ONE_CORE = "2"
FAST_CORES = "0-3,12-15"
ENVIRONMENT = dict(os.environ, ENT_CFLAGS="-march=native")


def on_mains():
    path = "/sys/class/power_supply/AC0/online"
    return not os.path.exists(path) or open(path).read().strip() == "1"


def build(source, name, rows, jobs, *flags):
    text = open(os.path.join(HERE, source)).read()
    text = text.replace("const ROWS: i32 = 1000000", f"const ROWS: i32 = {rows}")
    text = text.replace("const JOBS: i32 = 21", f"const JOBS: i32 = {jobs}")
    text = text.replace("capacity 1000000", f"capacity {rows}")
    path = os.path.join(WORK, name + ".ent")
    with open(path, "w") as out:
        out.write(text)
    binary = os.path.join(WORK, name)
    subprocess.run([ENT, "build", *flags, "-I", os.path.join(ROOT, "examples", "mojo"),
                    "-o", binary, path], check=True, env=ENVIRONMENT,
                   stdout=subprocess.DEVNULL)
    return binary


def pinned(cores, binary):
    command = ([] if cores is None else ["taskset", "-c", cores]) + [binary]
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def jobs_of(output):
    rows = []
    for line in output.splitlines():
        if line.startswith("job "):
            words = line.split()
            rows.append({words[i]: float(words[i + 1]) for i in range(2, len(words), 2)})
    return rows


def median(rows, key, keep=lambda r: r):
    return statistics.median(keep(r)[key] for r in rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=5)
    arguments = parser.parse_args()
    if not on_mains():
        sys.exit("run.py: on battery; timings from battery do not compare")
    os.makedirs(WORK, exist_ok=True)

    binaries = {}
    for rows in SIZES:
        gpu_jobs = max(21, 40000000 // rows)
        cpu_jobs = max(11, 10000000 // rows)
        binaries[rows, "gpu"] = (FAST_CORES, build("job.ent", f"gpu{rows}", rows, gpu_jobs))
        binaries[rows, "one"] = (ONE_CORE, build("cpu.ent", f"one{rows}", rows, cpu_jobs))
        binaries[rows, "all"] = (None, build("cpu.ent", f"all{rows}", rows, cpu_jobs, "--parallel"))
    idle = build("job.ent", "idle", 1000, 0)

    runs = {key: [] for key in binaries}
    for _ in range(arguments.repeats):
        for key, (cores, binary) in binaries.items():
            runs[key].append(jobs_of(pinned(cores, binary)))

    for rows in SIZES:
        steps = {r["steps"] for key in ("gpu", "one", "all")
                 for run in runs[rows, key] for r in run}
        if len(steps) != 1:
            sys.exit(f"run.py: {rows} rows gave different steps: {steps}")

    print("rows, ms: cold first job, warm trip | 1 thread, all threads | "
          "fill, submit, take+spawn, read+destroy | ticks out, ns/tick, worst tick us")
    for rows in SIZES:
        cold = statistics.median(run[0]["trip"] for run in runs[rows, "gpu"]) * 1e3
        warm = [r for run in runs[rows, "gpu"] for r in run[len(run) // 2:]]
        one = [r for run in runs[rows, "one"] for r in run[len(run) // 2:]]
        every = [r for run in runs[rows, "all"] for r in run[len(run) // 2:]]
        worst = sorted(r["worst"] for r in warm)
        print(f"{rows}: {cold:.1f}, {median(warm, 'trip') * 1e3:.2f} | "
              f"{median(one, 'took') * 1e3:.2f}, {median(every, 'took') * 1e3:.2f} | "
              f"{median(warm, 'fill') * 1e3:.2f}, {median(warm, 'submit') * 1e3:.2f}, "
              f"{median(warm, 'take') * 1e3:.2f}, {median(warm, 'read') * 1e3:.2f} | "
              f"{median(warm, 'ticks'):.0f}, "
              f"{(median(warm, 'trip') - median(warm, 'submit')) / median(warm, 'ticks') * 1e9:.0f}, "
              f"median {worst[len(worst) // 2] * 1e6:.1f} max {worst[-1] * 1e6:.1f}")

    ticks = []
    for _ in range(arguments.repeats):
        match = re.search(r"idle (\S+)", pinned(FAST_CORES, idle))
        ticks.append(float(match.group(1)))
    print(f"a tick with the device idle: {statistics.median(ticks):.0f} ns")

    print("start, ms:", end="")
    for name, source in (("empty", "import console\nsystem s() { put(\"x\") }\n"),
                         ("device", "import console\nimport gpujob\nsystem s() { put(\"x\") }\n")):
        path = os.path.join(WORK, name + ".ent")
        with open(path, "w") as out:
            out.write(source + "schedule f() { s() }\nmain { f() }\n")
        binary = os.path.join(WORK, name)
        subprocess.run([ENT, "build", "-I", os.path.join(ROOT, "examples", "mojo"), "-o",
                        binary, path], check=True, env=ENVIRONMENT, stdout=subprocess.DEVNULL)
        times = []
        for _ in range(30):
            start = time.perf_counter()
            subprocess.run(["taskset", "-c", ONE_CORE, binary], stdout=subprocess.DEVNULL)
            times.append(time.perf_counter() - start)
        print(f" {name} {min(times) * 1e3:.2f}", end="")
    print()


if __name__ == "__main__":
    main()
