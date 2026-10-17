#!/usr/bin/env python3
"""Measures the city's epidemic (examples/epidemic) and its reference.

  examples/epidemic/reference/fetch.sh               once
  python3 bench/epidemic/run.py [sizes] [--reference] [--apart] [--tracing]
                                [--days n]

For each size (people: 100000 1000000 by default) a copy of the model is
made that holds so many and notes the time after every system: built for
one core and for all of them (native: ENT_CFLAGS says otherwise), run for
100 days of the reference's baseline with ten infected at the start, and
said: how long the city took to build, how long a day took on average
and of that each system, and the most memory the program had. The build
for one core runs on a fast core (CPU 2); the one for all on the four
fast cores (4 threads) and on all (BENCH_ONE_CPU, BENCH_CPUS and
BENCH_THREADS say otherwise). On all the cores it runs three times and the median of
each time is said: nothing holds a thread to a core there, and what one
thread does lands on a fast core or a slow one.

With --reference the reference (built from its C, -O3 and native) runs
the same days from the same numbers on the same fast core. It says a
line after each day: its set-up is the time to the first, a day the time
from there to the last shared among the days between, so the files it
writes at its end are not counted. Its memory is said twice: as it is, and with the people
met kept for one day in place of ten, which the model does not keep.

With --tracing both run the reference's measures: who has symptoms stays
at home with their household and is tested, and whom their app saw them
meet is told (500 are infected at the start).

With --apart the model's tag for who is ill is stored apart (`tag Ill
apart`): the ill in an archetype of their own.

With --gpu a,b the systems named are `gpu system`s and the model is built
with `--gpu` (docs/gpu.md), on one core and on the fast cores as above
(their times are said next to those without). A `gpu` system's own time
is that of giving the GPU its work; what the GPU takes shows where the CPU
next waits for it (at the latest when the day ends), so a day's time is
what to compare. BENCH_ALL=0 leaves out the runs on all the cores.
"""

import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MODEL = os.path.join(ROOT, "examples", "epidemic")
OUT = os.path.join(ROOT, "build", "epidemic")
BENCH = os.path.join(OUT, "bench")
PANEL = os.path.join(OUT, "run", "baseline_household_demographics.csv")

SETUP = ["parameters", "measures", "panel", "households", "settle", "apps",
         "census", "employ", "wire", "rows", "curve", "rate", "share", "seed",
         "tally"]
DAY = ["tick", "policy", "remember", "infectiousness", "today", "lay", "pair",
       "household", "workplace", "chance", "infect", "progress", "symptoms",
       "tests", "trace", "told", "tally"]


# What is done about the epidemic besides, with --tracing: those with
# symptoms and their households stay at home and are tested, and whom
# their app saw them meet is told, stays at home and is tested
# (examples/epidemic/check/curves.py's `tracing`, with 500 infected at
# the start so that the epidemic is under way at once).
MEASURES = {}
TRACING = {"self_quarantine_fraction": "0.8", "test_on_symptoms": "1",
           "quarantine_household_on_symptoms": "1",
           "quarantine_household_on_positive": "1", "app_turn_on_time": "1",
           "trace_on_symptoms": "1", "trace_on_positive": "1",
           "quarantine_on_traced": "1", "test_on_traced": "1",
           "n_seed_infection": "500"}


def program(people):
    """The model for so many people, with the time noted after every
    system of the set-up and of a day."""
    laps = SETUP + DAY
    fields = ", ".join(f"s{i}: f64" for i in range(len(laps)))
    text = f'''import console
import clock
import parameters
import disease
import city
import transmission
import interventions

capacity Person {people}
capacity Household {people}
capacity LivesIn {people}
capacity WorksWith {people * 11}
capacity Taken {people}
capacity Ring {people}
capacity Order {8 * (people + 1)}
capacity Index {people}
capacity Housemate {2 * people}
capacity ToldBy {8 * people}

unique Laps {{ last: f64, days: i32, {fields} }}

world {{
  Run.people = {people}
  Model.file = "parameters.csv"
}}

system start() {{ Laps.last = wait_until(0.0) }}
'''
    for index in range(len(laps)):
        text += f'''system lap{index}() {{
  let now = wait_until(0.0)
  Laps.s{index} += now - Laps.last
  Laps.last = now
}}
'''
    text += "system counted() { Laps.days += 1 }\n"
    text += "system report() {\n"
    for index, name in enumerate(laps):
        kind = "setup" if index < len(SETUP) else "day"
        text += f'  put("{kind} {name} {{Laps.s{index}}}")\n'
    text += '  put("days {Laps.days} infected {Run.people - Tally.susceptible} dead {Tally.dead}")\n}\n'
    text += "schedule setup() {\n  start()\n"
    for index, name in enumerate(SETUP):
        text += f"  {name}()\n  lap{index}()\n"
    text += "}\nschedule day() {\n"
    for index, name in enumerate(DAY):
        # (The clock has a `tick` of its own.)
        call = "disease::tick" if name == "tick" else name
        text += f"  {call}()\n  lap{len(SETUP) + index}()\n"
    text += "  counted()\n}\nschedule end() { report() }\n"
    text += '''main {
  setup()
  loop { day() } until Clock.day == Run.days || Tally.infected == 0
  end()
}
'''
    return text


def parameters(people, days, more=None):
    reference = os.path.join(OUT, "reference", "tests", "data", "baseline_parameters.csv")
    with open(reference) as file:
        names, values = list(csv.reader(file))[:2]
    names = [name.strip() for name in names]
    values = [value.strip() for value in values]
    changes = {"n_total": str(people), "end_time": str(days), "rng_seed": "1"}
    changes.update(MEASURES)
    changes.update(more or {})
    for name, value in changes.items():
        values[names.index(name)] = value
    return names, values


def measure(command, folder, environment=None, cpus=None):
    """How long it took, the most memory it had (MB), and what it said."""
    if cpus:
        command = ["taskset", "-c", cpus] + command
    with tempfile.TemporaryFile() as out:
        started = time.time()
        process = subprocess.Popen(command, stdout=out, cwd=folder, env=environment)
        _, status, usage = os.wait4(process.pid, 0)
        seconds = time.time() - started
        out.seek(0)
        said = out.read().decode()
    if status != 0:
        sys.exit(f"{command[-1]} ended with status {status}")
    return seconds, usage.ru_maxrss / 1024, said


def build_model(people, parallel, apart, gpu=()):
    folder = os.path.join(BENCH, f"model_{people}{'_apart' if apart else ''}"
                                 f"{'_gpu' if gpu else ''}")
    os.makedirs(folder, exist_ok=True)
    for name in os.listdir(MODEL):
        if name.endswith(".ent"):
            text = open(os.path.join(MODEL, name)).read()
            if apart and name == "disease.ent":
                changed = text.replace("tag Ill\n", "tag Ill apart\n")
                if changed == text:
                    sys.exit("disease.ent has no `tag Ill` to store apart")
                text = changed
            for system in gpu:
                text = re.sub(rf"^system {system}\(", f"gpu system {system}(",
                              text, flags=re.M)
            with open(os.path.join(folder, name), "w") as file:
                file.write(text)
    with open(os.path.join(folder, "bench.ent"), "w") as file:
        file.write(program(people))
    shutil.copy(PANEL, folder)
    built = os.path.join(folder, "bench_all" if parallel else "bench_one")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    environment.setdefault("ENT_CFLAGS", "-march=native")
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build"] +
        (["--parallel"] if parallel else []) + (["--gpu"] if gpu else []) +
        [os.path.join(folder, "bench.ent"), "-o", built],
        check=True, env=environment)
    return folder, built


def run_model(people, days, parallel, apart, cpus, threads, gpu=()):
    folder, built = build_model(people, parallel, apart, gpu)
    with open(os.path.join(folder, "parameters.csv"), "w", newline="") as file:
        csv.writer(file).writerows(parameters(people, days))
    environment = dict(os.environ)
    if threads:
        environment["OMP_NUM_THREADS"] = str(threads)
    seconds, memory, said = measure([built], folder, environment, cpus)
    setup, day, count = {}, {}, 0
    infected = dead = 0
    for line in said.split("\n"):
        words = line.split()
        if len(words) == 3 and words[0] in ("setup", "day"):
            (setup if words[0] == "setup" else day)[words[1]] = float(words[2])
        elif words[:1] == ["days"]:
            count, infected, dead = int(words[1]), int(words[3]), int(words[5])
    return {"seconds": seconds, "memory": memory, "setup": setup, "day": day,
            "days": count, "infected": infected, "dead": dead}


def build_reference():
    built = os.path.join(BENCH, "reference_native")
    if os.path.isfile(built):
        return built
    source = os.path.join(OUT, "reference", "src")
    files = [f for f in sorted(os.listdir(source))
             if f.endswith(".c") and f != "R_C_interface.c" and "_wrap" not in f]
    subprocess.run(["cc", "-O3", "-march=native", "-w", "-o", built] + files +
                   ["-lgsl", "-lgslcblas", "-lm"], check=True, cwd=source)
    return built


def run_reference(people, days, cpus):
    """The reference says a line after each day: its set-up is the time to
    the first, a day the time from there to the last shared among the
    days between. (What it writes to files after that is not counted.)"""
    built = build_reference()
    folder = os.path.join(BENCH, f"reference_{people}")
    os.makedirs(folder, exist_ok=True)

    def one(more=None):
        with open(os.path.join(folder, "parameters.csv"), "w", newline="") as file:
            csv.writer(file).writerows(parameters(people, days, more))
        command = ["taskset", "-c", cpus, "stdbuf", "-oL", built, "parameters.csv",
                   "1", ".", PANEL]
        started = time.time()
        process = subprocess.Popen(command, stdout=subprocess.PIPE, cwd=folder,
                                   text=True)
        stamps = []
        infected = 0
        for line in process.stdout:
            if line[:1].isdigit():
                stamps.append(time.time())
                infected = int(line.split(",")[6])
        _, status, usage = os.wait4(process.pid, 0)
        if status != 0 or len(stamps) < 2:
            sys.exit(f"the reference ended with status {status}")
        return (stamps[0] - started, (stamps[-1] - stamps[0]) / (len(stamps) - 1),
                usage.ru_maxrss / 1024, infected)
    setup, day, memory, infected = one()
    _, _, without, _ = one({"days_of_interactions": "1", "quarantine_days": "1"})
    return {"setup": setup, "day": day, "memory": memory, "without": without,
            "infected": infected}


def middle(runs):
    """Of several runs of one program the median of each time (of the
    set-up's and the day's systems each by itself, and of a day as a
    whole: `all`) and of the memory. On all the cores the parts of a day
    that one thread does run on whichever core it gets, a fast one or
    not, and a single run says little."""
    def median(values):
        return sorted(values)[len(values) // 2]
    first = runs[0]
    return dict(first,
                setup={name: median([run["setup"][name] for run in runs])
                       for name in first["setup"]},
                day={name: median([run["day"][name] for run in runs])
                     for name in first["day"]},
                all=median([sum(run["day"].values()) for run in runs]),
                memory=median([run["memory"] for run in runs]))


def main():
    arguments = sys.argv[1:]
    days = 100
    if "--days" in arguments:
        days = int(arguments[arguments.index("--days") + 1])
        del arguments[arguments.index("--days"):arguments.index("--days") + 2]
    sizes = [int(a) for a in arguments if a.isdigit()] or [100000, 1000000]
    apart = "--apart" in arguments
    gpu = ()
    if "--gpu" in arguments:
        gpu = tuple(arguments[arguments.index("--gpu") + 1].split(","))
        del arguments[arguments.index("--gpu"):arguments.index("--gpu") + 2]
    if "--tracing" in arguments:
        MEASURES.update(TRACING)
    if not os.path.isfile(PANEL):
        sys.exit("no household panel: run examples/epidemic/reference/fetch.sh")
    os.makedirs(BENCH, exist_ok=True)
    one = os.environ.get("BENCH_ONE_CPU", "2")
    fast = os.environ.get("BENCH_CPUS", "0-3")
    threads = os.environ.get("BENCH_THREADS", "4")
    for people in sizes:
        print(f"{people} people, {days} days{', the ill apart' if apart else ''}"
              f"{', with tracing' if MEASURES else ''}")
        runs = [("one core", run_model(people, days, False, apart, one, None)),
                (f"{threads} threads on the fast cores",
                 run_model(people, days, True, apart, fast, threads))]
        if gpu:
            runs += [("one core and the GPU",
                      run_model(people, days, False, apart, one, None, gpu)),
                     (f"{threads} threads and the GPU",
                      run_model(people, days, True, apart, fast, threads, gpu))]
        if os.environ.get("BENCH_ALL", "1") != "0":
            runs.append(("all the cores, the median of three",
                         middle([run_model(people, days, True, apart, None, None)
                                 for _ in range(3)])))
        names = [name for name in DAY]
        print("  | | set-up s | a day ms | " + " | ".join(names) + " | memory MB | infected |")
        print("  |---|---|---|" + "---|" * len(names) + "---|---|")
        for name, run in runs:
            count = max(1, run["days"])
            per = [1000 * run["day"][n] / count for n in names]
            whole = 1000 * run["all"] / count if "all" in run else sum(per)
            print(f"  | {name} | {sum(run['setup'].values()):.2f} | {whole:.1f} | " +
                  " | ".join(f"{p:.2f}" for p in per) +
                  f" | {run['memory']:.0f} | {run['infected']} |")
        first = runs[0][1]
        print("  set-up, one core: " + ", ".join(
            f"{name} {seconds:.2f}" for name, seconds in first["setup"].items()
            if seconds >= 0.005))
        if "--reference" in arguments:
            reference = run_reference(people, days, one)
            print(f"  reference, one core: set-up {reference['setup']:.2f} s, "
                  f"a day {1000 * reference['day']:.1f} ms, memory "
                  f"{reference['memory']:.0f} MB ({reference['without']:.0f} MB "
                  f"keeping one day of meetings), infected {reference['infected']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
