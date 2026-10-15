#!/usr/bin/env python3
"""Times the analog simulator (examples/analog) against ngspice and Xyce.

  examples/analog/fetch.sh 1 2                (for the power grids)
  python3 bench/analog/run.py [circuits] [--variants=a,b] [--timeout=S]
                              [--repeats=N]

The circuits are crossbars of Yakopcic's memristors (the model that
ngspice, through OpenVAF, and Xyce, built in, both have; spec section
9.2), as crossbar.py writes them with variation 0.1 and seed 1, named
crossbar_<N>_<R_wire>_yakopcic_<read|write>, and IBM power grids
(ibmpg<k>), which are made and prepared here where they are not yet. By
default: N 16, 64 and 256, wires of 0, 0.1, 1 and 10 ohm, both scenarios,
and ibmpg1 and ibmpg2.

Every simulator runs the same flat circuit (prepare.py's circuit.net,
circuit.cir and circuit.xyce) at SPICE's own tolerances (reltol 1e-3,
vntol 1e-6 V, abstol 1e-12 A), with steps chosen by their error and no
longer than the circuit's (1 ms), and writes its waves. The variants:

  way0, way1, way2        the model, one core (CPU 2, a Zen 5 core)
  way0p, way1p, way2p     the model built with --parallel, on the four
                          Zen 5 cores (CPUs 0 to 3, OMP_NUM_THREADS=4)
  ngspice                 ngspice with KLU, one core (CPU 2)
  xyce                    Xyce serial (KLU), one core (CPU 2)

(Xyce in parallel needs a build with MPI; the one installed has none.)

Before a variant is timed its waves are held against the reference:
Xyce's at tight tolerances, as check 5 runs it (check/xyce.py), where it
is done in 900 s, else the model's way 0 at reltol 1e-5 (which check 5
holds to Xyce's where Xyce is done); for a grid the published solution
(check 7). A variant whose waves are more
than 1e-2 of a probe's range off it (1e-5 V on a grid) is said, with how
far off, and not timed; one that does not finish within the timeout (by
default 600 s) is said too, and not run on the larger crossbars of the same wires and
scenario. Each run is a process of its own, round-robin
over the variants and circuits, `repeats` times (the median is given).
Said for each: the wall time, the time points and those tried again, for
the model the rounds a time point took (in the mean and at most) and the
node steps a time point (relaxation), for ngspice and Xyce Newton's
iterations, how far off the reference, and the most memory.
"""

import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ANALOG = os.path.join(ROOT, "examples", "analog")
CIRCUITS = os.path.join(ROOT, "build", "analog", "circuits")
BENCH = os.path.join(ROOT, "build", "analog", "bench")
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ANALOG, "check"))
import common
import grid
import spice
import xyce

VARIANTS = ["way0", "way1", "way2", "way0p", "way1p", "way2p", "ngspice", "xyce"]
SIZES, WIRES, SCENARIOS = [16, 64, 256], ["0", "0.1", "1", "10"], ["read", "write"]
GRIDS = ["ibmpg1", "ibmpg2"]
SHARE, VOLTS = 1e-2, 1e-5


def made(name):
    """The circuit, written and prepared where it is not yet."""
    if os.path.exists(os.path.join(CIRCUITS, name, "circuit.net")):
        return
    if name.startswith("crossbar_"):
        size, wire, model, scenario = name.split("_")[1:5]
        subprocess.run([sys.executable, os.path.join(ANALOG, "crossbar.py"), size, wire, model,
                        "0.1", "1", scenario], check=True, stdout=subprocess.DEVNULL)
    elif not os.path.exists(os.path.join(ROOT, "build", "analog", "sources", name + ".cir")):
        subprocess.run([os.path.join(ANALOG, "fetch.sh"), name[5:]], check=True)
    subprocess.run([sys.executable, os.path.join(ANALOG, "prepare.py"), name], check=True,
                   stdout=subprocess.DEVNULL)


def builds():
    """The model, for one core and for many."""
    programs = {}
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    environment.setdefault("ENT_CFLAGS", "-march=native")
    for kind, more in (("", []), ("p", ["--parallel"])):
        programs[kind] = os.path.join(BENCH, "simulate" + kind)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", *more,
                        "-I", ANALOG, os.path.join(ANALOG, "simulate.ent"), "-o",
                        programs[kind]], check=True, env=environment)
    return programs


def readied(name, variant):
    """The folder a variant runs the circuit in, with what it reads: the
    command, the environment, the CPUs, and the file its waves go to."""
    folder = os.path.join(BENCH, f"{name}_{variant}")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    source = os.path.join(CIRCUITS, name)
    net = common.Netlist(name)
    if variant.startswith("way"):
        shutil.copy(os.path.join(source, "circuit.net"), folder)
        with open(os.path.join(folder, "run.txt"), "w") as file:
            file.write(f"{variant[3]} 1 1 0.001 0.000001 1e-12 100000 0 0 0\n")
        parallel = variant.endswith("p")
        return (folder, [PROGRAMS["p" if parallel else ""]],
                {"OMP_NUM_THREADS": "4" if parallel else "1"}, "0-3" if parallel else "2",
                "waves.txt")
    if variant == "ngspice":
        with open(os.path.join(source, "circuit.cir")) as file:
            deck = file.read()
        for model in re.findall(r"^pre_osdi (\S+)", deck, re.M):
            shutil.copy(os.path.join(spice.MODELS, model), folder)
        deck = re.sub(r"^\.tran .*$", lambda m: tran(net), deck, flags=re.M)
        deck = deck.replace("\n.control", "\n.options klu\n.control", 1)
        deck = deck.replace("\nrun\n", "\nrun\nrusage tranpoints accept rejected traniter "
                            "totiter\n", 1)
        if not net.tran:
            # (A grid: every node, written as ngspice's raw file.)
            deck = re.sub(r"set wr_singlescale\n.*?wrdata [^\n]*\n", "", deck, flags=re.S)
            deck = deck.replace("\n.endc", "\nset filetype=ascii\nwrite op.raw\n.endc", 1)
        with open(os.path.join(folder, "circuit.cir"), "w") as file:
            file.write(deck)
        return (folder, ["ngspice", "-b", "circuit.cir"], {"OMP_NUM_THREADS": "1"}, "2",
                "spice.txt" if net.tran else "op.raw")
    with open(os.path.join(source, "circuit.xyce")) as file:
        deck = file.read()
    deck = re.sub(r"^\.tran .*$", lambda m: tran(net), deck, flags=re.M)
    if not net.tran:
        deck = re.sub(r"^\.print dc .*$", ".print dc format=noindex precision=17 file=xyce.txt "
                      "V(*)", deck, flags=re.M)
    with open(os.path.join(folder, "circuit.xyce"), "w") as file:
        file.write(deck)
    return folder, ["Xyce", "circuit.xyce"], {"OMP_NUM_THREADS": "1"}, "2", "xyce.txt"


def tran(net):
    """The .tran line: the circuit's, its largest step no more than its step."""
    step, stop = net.tran[0], net.tran[1]
    most = min(net.tran[2], step) if len(net.tran) > 2 else step
    return f".tran {step!r} {stop!r} 0 {most!r}"


def timed(command, folder, environment, cpus, timeout):
    """Wall seconds (None past the timeout), what it said, and its most
    memory in MB (read from the process as it runs). Xyce runs alone and
    held in memory, as check 5 runs it (xyce.alone)."""
    if command[0] == "Xyce":
        with xyce.alone():
            return measured(command, folder, environment, cpus, timeout, xyce.limited)
    return measured(command, folder, environment, cpus, timeout, None)


def measured(command, folder, environment, cpus, timeout, limit):
    started = time.time()
    process = subprocess.Popen(["taskset", "-c", cpus] + command, cwd=folder,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               env=dict(os.environ, **environment), preexec_fn=limit)
    most = 0.0
    output = []
    import threading
    reader = threading.Thread(target=lambda: output.append(process.stdout.read()))
    reader.start()
    while process.poll() is None:
        try:
            with open(f"/proc/{process.pid}/status") as status:
                for line in status:
                    if line.startswith("VmHWM:"):
                        most = max(most, int(line.split()[1]) / 1024)
        except OSError:
            pass
        if time.time() - started > timeout:
            process.kill()
            process.wait()
            reader.join()
            return None, "".join(output), most
        time.sleep(0.005)
    took = time.time() - started
    reader.join()
    return took, "".join(output), most


def numbers(path):
    lines = []
    if os.path.exists(path):
        with open(path) as file:
            for line in file:
                try:
                    lines.append([float(w) for w in line.split()])
                except ValueError:
                    continue
    return lines


def counts(variant, said):
    """Time points, those tried again, and rounds (Newton's iterations) and
    node steps in all, from what the run said."""
    if variant.startswith("way"):
        found = re.search(r"(\d+) time points, (\d+) tried again, settled (\w+), (\d+) rounds, "
                          r"at most (\d+) at one, (\d+) node steps", said)
        if not found:
            return None
        points, again, settled, rounds, most, steps = found.groups()
        return {"points": int(points), "again": int(again), "settled": settled == "true",
                "rounds": int(rounds), "most": int(most), "steps": int(steps)}
    if variant == "ngspice":
        get = lambda key: int((re.search(key + r" = (\d+)", said) or [0, 0])[1])
        return {"points": get("Accepted timepoints"), "again": get("Rejected timepoints"),
                "settled": True, "rounds": get("Total iterations"), "most": 0, "steps": 0}
    steps = re.findall(r"Number Successful Steps Taken:\s+(\d+)", said)
    failed = re.findall(r"Number Failed Steps Attempted:\s+(\d+)", said)
    newton = re.findall(r"Number Jacobians Evaluated:\s+(\d+)", said)
    return {"points": int(steps[-1]) if steps else 0, "again": int(failed[-1]) if failed else 0,
            "settled": True, "rounds": int(newton[-1]) if newton else 0, "most": 0, "steps": 0}


def off(name, variant, folder, wavefile, reference):
    """How far the waves are off the reference: in shares of a probe's
    range (crossbars), or in volts (grids)."""
    path = os.path.join(folder, wavefile)
    if name.startswith("ibmpg"):
        index = grid.nodes(name)
        if variant.startswith("way"):
            waves = numbers(path)
            if not waves:
                return None
            net = common.Netlist(name)
            worst, _ = grid.compared(waves, net, index, reference)
            return worst
        voltage = {0: 0.0}
        if variant == "ngspice":
            voltage.update(raw(path))
        else:
            with open(path) as file:
                head = file.readline().split()
                values = file.readline().split()
            for label, value in zip(head, values):
                found = re.fullmatch(r"V\((\d+)\)", label)
                if found:
                    voltage[int(found.group(1))] = float(value)
        worst = 0.0
        for node, want in reference.items():
            if index.get(node) not in voltage:
                return None
            worst = max(worst, abs(voltage[index[node]] - want))
        return worst
    waves = numbers(path)
    if not waves:
        return None
    times = [line[0] for line in reference]
    ranges = [max(1e-12, max(line[k] for line in reference) - min(line[k] for line in reference))
              for k in range(1, len(reference[0]))]
    import bisect
    worst = 0.0
    for line in waves:
        j = min(max(bisect.bisect_left(times, line[0]), 1), len(times) - 1)
        t0, t1 = times[j - 1], times[j]
        share = (line[0] - t0) / (t1 - t0) if t1 > t0 else 0.0
        for k in range(1, len(reference[0])):
            want = reference[j - 1][k] + (reference[j][k] - reference[j - 1][k]) * share
            worst = max(worst, abs(line[k] - want) / ranges[k - 1])
    return worst


def raw(path):
    """An ngspice raw file, written as text: the voltage of each node."""
    voltage = {}
    with open(path) as file:
        text = file.read()
    variables = re.search(r"Variables:\n(.*?)\nValues:\n(.*)", text, re.S)
    names = [line.split()[1] for line in variables.group(1).splitlines() if line.strip()]
    values = variables.group(2).split()[1:]
    for name, value in zip(names, values):
        found = re.fullmatch(r"v\((\d+)\)|(\d+)", name)
        if found:
            voltage[int(found.group(1) or found.group(2))] = float(value)
    return voltage


def referenced(name):
    """The reference: Xyce at tight tolerances (check 5), or where it is
    not done in REFERENCE seconds the model's way 0 at reltol 1e-5 (which
    check 5 holds to Xyce's where Xyce is done); or a grid's published
    solution (check 7). Said which; None where there is none."""
    if name.startswith("ibmpg"):
        WHOSE[name] = "published"
        return grid.published(name)
    net = common.Netlist(name)
    for whose in ("xyce", "way0"):
        path = os.path.join(BENCH, f"{name}.{whose}.reference")
        if os.path.exists(path):
            WHOSE[name] = whose
            return numbers(path)
    lines, why, took = xyce.xyce(name, net, REFERENCE)
    whose = "xyce"
    if lines is None:
        print(f"{name}: Xyce at tight tolerances: {why}; way 0 at reltol 1e-5 instead",
              file=sys.stderr, flush=True)
        lines, said = common.run(PROGRAMS[""], name, 0, reltol=1e-5,
                                 folder=os.path.join(BENCH, name + "_reference"))
        whose = "way0"
        if not lines or "settled true" not in said:
            return None
    with open(os.path.join(BENCH, f"{name}.{whose}.reference"), "w") as file:
        file.writelines(" ".join(repr(v) for v in line) + "\n" for line in lines)
    WHOSE[name] = whose
    return lines


WHOSE = {}
REFERENCE = 900


def main():
    global PROGRAMS
    arguments = sys.argv[1:]
    option = lambda key, default: next((a.split("=", 1)[1] for a in arguments
                                        if a.startswith(f"--{key}=")), default)
    variants = option("variants", ",".join(VARIANTS)).split(",")
    timeout = float(option("timeout", "600"))
    repeats = int(option("repeats", "1"))
    names = [a for a in arguments if not a.startswith("--")] or \
        [f"crossbar_{n}_{w}_yakopcic_{s}" for n in SIZES for w in WIRES for s in SCENARIOS] + \
        GRIDS
    os.makedirs(BENCH, exist_ok=True)
    for name in names:
        made(name)
    spice.models()
    PROGRAMS = builds()
    references = {name: referenced(name) for name in names}
    results = {}   # (name, variant) -> dict, or a word for why there is no time
    for round_ in range(repeats):
        for name in names:
            for variant in variants:
                key = (name, variant)
                if isinstance(results.get(key), str):
                    continue
                # (A crossbar a variant did not finish in time at a smaller
                # size, with the same wires and scenario, it is not given.)
                if name.startswith("crossbar_"):
                    size, rest = int(name.split("_")[1]), name.split("_", 2)[2]
                    smaller = [k for k in results if k[1] == variant and
                               k[0].startswith("crossbar_") and k[0].split("_", 2)[2] == rest
                               and int(k[0].split("_")[1]) < size
                               and isinstance(results[k], str) and results[k].startswith(">")]
                    if smaller:
                        results[key] = results[smaller[0]]
                        continue
                if variant == "xyce" and not os.path.exists(
                        os.path.join(CIRCUITS, name, "circuit.xyce")):
                    results[key] = "no deck"
                    continue
                folder, command, environment, cpus, wavefile = readied(name, variant)
                took, said, memory = timed(command, folder, environment, cpus, timeout)
                if took is None:
                    results[key] = f"> {timeout:.0f} s"
                    print(f"{name} {variant}: past {timeout:.0f} s", file=sys.stderr, flush=True)
                    continue
                count = counts(variant, said)
                if count is None or not count["settled"]:
                    results[key] = "did not settle"
                    print(f"{name} {variant}: did not settle: {said.strip()[-300:]}",
                          file=sys.stderr, flush=True)
                    continue
                if round_ == 0:
                    reference = references[name]
                    error = off(name, variant, folder, wavefile, reference) \
                        if reference else None
                    bound = VOLTS if name.startswith("ibmpg") else SHARE
                    if reference and (error is None or error > bound):
                        results[key] = "no waves" if error is None else f"off {error:.1e}"
                        print(f"{name} {variant}: off the reference ({error})", file=sys.stderr,
                              flush=True)
                        continue
                    results[key] = dict(count, error=error, times=[], memory=memory)
                results[key]["times"].append(took)
                results[key]["memory"] = max(results[key]["memory"], memory)
                print(f"{name} {variant}: {took:.2f} s", file=sys.stderr, flush=True)
                shutil.rmtree(folder, ignore_errors=True)
    report(names, variants, results)
    return 0


def report(names, variants, results):
    def cell(name, variant):
        result = results.get((name, variant))
        if result is None:
            return "—"
        if isinstance(result, str):
            return result
        return seconds(statistics.median(result["times"]))

    print("Wall time of a run (reading the circuit included):\n")
    print("| circuit | nodes | reference | " + " | ".join(variants) + " |")
    print("|---|---|---|" + "---|" * len(variants))
    for name in names:
        net = common.Netlist(name)
        print(f"| {name} | {net.nodes - 1:,} | {WHOSE.get(name, 'none')} | " +
              " | ".join(cell(name, v) for v in variants) + " |")
    print("\nTime points (tried again), rounds a time point (mean, most), node steps a "
          "time point, off the reference, most memory:\n")
    print("| circuit | variant | time points | rounds | node steps | off | memory |")
    print("|---|---|---|---|---|---|---|")
    for name in names:
        for variant in variants:
            result = results.get((name, variant))
            if not isinstance(result, dict):
                continue
            points = max(1, result["points"])
            rounds = f"{result['rounds'] / points:,.1f}" + \
                (f", {result['most']:,}" if result["most"] else "")
            steps = f"{result['steps'] / points:,.0f}" if result["steps"] else ""
            error = "" if result["error"] is None else \
                (f"{result['error']:.1e} V" if name.startswith("ibmpg") else
                 f"{result['error']:.1e}")
            print(f"| {name} | {variant} | {result['points']:,} ({result['again']:,}) | "
                  f"{rounds} | {steps} | {error} | {result['memory']:,.0f} MB |")


def seconds(value):
    return f"{value * 1000:.0f} ms" if value < 1 else f"{value:.2f} s" if value < 100 else \
        f"{value:,.0f} s"


if __name__ == "__main__":
    sys.exit(main())
