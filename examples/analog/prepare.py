#!/usr/bin/env python3
"""Makes a circuit ready for the analog simulator and its references.

  examples/analog/prepare.py divider rc_step ...     or: prepare.py all
  examples/analog/prepare.py path/to/some.cir

For each circuit named (a SPICE netlist in examples/analog/circuits, or
in build/analog/sources where crossbar.py writes them, or any path) it
reads the netlist, makes it flat, and writes into
build/analog/circuits/<name>/:

  circuit.net   the flat netlist the model reads (spec section 4)
  circuit.cir   the same circuit for ngspice

Both come from the one flat circuit, so the model and ngspice simulate
the same elements with the same numbers. (The deck for Xyce comes with
milestone M5, when Xyce is there.)

What is read, of SPICE: a title line; `*` comments, `;` after a line,
`+` going on with the line before; resistors R, capacitors C (with
IC=), voltage and current sources V and I (a DC value, and SIN, PULSE
or PWL), memristors written `X<name> a b <model> name=value ...`, where
the model is one of examples/analog/models/*.va, and subcircuits
(`.subckt` ... `.ends`, `X<name> nodes... <subckt>`), which are made
flat; `.op`, `.tran tstep tstop [tstart [tmax]]`, and what goes into the
waves, `.print`, `.probe` or `.save` with v(node), v(a,b) is not done,
and i(device). Numbers may have SPICE's suffixes (k, meg, u, n, ...);
in circuit.net they are plain, as many digits as it takes for the same
f64.

Says for each circuit how many nodes, devices and probes it has.
"""

import decimal
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CIRCUITS = os.path.join(HERE, "circuits")
SOURCES = os.path.join(ROOT, "build", "analog", "sources")
OUT = os.path.join(ROOT, "build", "analog", "circuits")
MODELS = os.path.join(HERE, "models")

# (Powers of ten, so that 10u is the f64 nearest to 1e-5, as written.)
SCALE = [("meg", 6), ("mil", None), ("t", 12), ("g", 9), ("k", 3), ("m", -3), ("u", -6),
         ("n", -9), ("p", -12), ("f", -15), ("a", -18)]


class Wrong(Exception):
    pass


def value(word):
    """A SPICE number: 2k, 10meg, 1.5u, 3e-9F, 100ohm."""
    found = re.fullmatch(r"([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)([a-zA-Z]*)", word)
    if not found:
        raise Wrong(f"'{word}' is no number")
    number, rest = decimal.Decimal(found.group(1)), found.group(2).lower()
    for suffix, power in SCALE:
        if rest.startswith(suffix):
            return float(number * decimal.Decimal("25.4e-6")) if power is None else \
                float(number.scaleb(power))
    return float(number)


def text(number):
    """A number as few digits as give the same f64."""
    return repr(float(number)) if number != int(number) or abs(number) >= 1e16 else \
        str(int(number))


def parameters(model):
    """A memristor model's parameters, in the order its .va declares them."""
    path = os.path.join(MODELS, model + ".va")
    if not os.path.isfile(path):
        return None
    with open(path) as file:
        found = re.findall(r"^\s*parameter\s+(?:real|integer)\s+(\w+)\s*=\s*([^\s;]+)",
                           file.read(), re.M)
    if not found or found[-1][0] != "w0":
        raise Wrong(f"{path}: the last parameter is to be w0")
    return [(name, value(default)) for name, default in found]


def lines_of(path):
    """The netlist's lines, comments gone and continued lines joined."""
    with open(path) as file:
        raw = file.read().split("\n")
    lines = []
    for number, line in enumerate(raw[1:], 2):
        line = line.split(";")[0].rstrip()
        if not line.strip() or line.lstrip().startswith("*"):
            continue
        if line.lstrip().startswith("+"):
            if not lines:
                raise Wrong(f"line {number}: '+' with no line before")
            lines[-1] = (lines[-1][0], lines[-1][1] + " " + line.lstrip()[1:])
        else:
            lines.append((number, line.strip()))
    return lines


def words(line):
    """A line's words: brackets and commas apart, name=value kept whole."""
    line = re.sub(r"\s*=\s*", "=", line.replace(",", " "))
    return re.sub(r"[()]", " ", line).split()


class Circuit:
    def __init__(self, path):
        self.path = path
        self.nodes = {"0": 0}
        self.names = ["0"]
        self.devices = []     # (kind, name, a, b, numbers)
        self.named = {}
        self.probes = []
        self.tran = None
        self.op = False
        self.subckts = {}
        lines = lines_of(path)
        self.collect(lines)
        self.read([line for line in lines if line], "", {})

    def node(self, name, prefix, ports):
        name = name.lower()
        if name in ("0", "gnd"):
            return 0
        if name in ports:
            return ports[name]
        name = prefix + name
        if name not in self.nodes:
            self.nodes[name] = len(self.names)
            self.names.append(name)
        return self.nodes[name]

    def collect(self, lines):
        """The subcircuits, taken out of the lines."""
        inside = None
        for at, (number, line) in enumerate(lines):
            first = line.split()[0].lower()
            if first == ".subckt":
                if inside:
                    raise Wrong(f"line {number}: a .subckt inside another")
                parts = line.split()
                inside = parts[1].lower()
                self.subckts[inside] = ([p.lower() for p in parts[2:]], [])
                lines[at] = None
            elif first == ".ends":
                inside = None
                lines[at] = None
            elif inside:
                self.subckts[inside][1].append((number, line))
                lines[at] = None

    def read(self, lines, prefix, ports):
        for number, line in lines:
            try:
                self.line(line, prefix, ports)
            except Wrong as wrong:
                raise Wrong(f"{self.path}, line {number}: {wrong}")

    def add(self, kind, name, a, b, numbers):
        if name.lower() in self.named:
            raise Wrong(f"a second device named {name}")
        self.named[name.lower()] = len(self.devices)
        self.devices.append((kind, name, a, b, numbers))

    def line(self, line, prefix, ports):
        parts = words(line)
        first = parts[0].lower()
        if first.startswith("."):
            self.control(first, parts[1:], prefix, line)
            return
        kind = first[0]
        name = prefix + parts[0]
        if kind in "rc":
            if len(parts) < 4:
                raise Wrong("an R or C needs two nodes and a value")
            a, b = self.node(parts[1], prefix, ports), self.node(parts[2], prefix, ports)
            amount = value(parts[3])
            if kind == "r":
                if amount <= 0:
                    raise Wrong("a resistor of no resistance")
                self.add("R", name, a, b, [amount])
            else:
                start = 0.0
                for word in parts[4:]:
                    if word.lower().startswith("ic="):
                        start = value(word[3:])
                self.add("C", name, a, b, [amount, start])
        elif kind in "vi":
            a, b = self.node(parts[1], prefix, ports), self.node(parts[2], prefix, ports)
            self.add(kind.upper(), name, a, b, self.wave(parts[3:]))
        elif kind == "x":
            self.instance(parts, name, prefix, ports)
        else:
            raise Wrong(f"an element '{parts[0]}', which is not done")

    def wave(self, parts):
        """A source's waveform: its kind and its numbers, defaults filled in."""
        dc, shape, numbers, at = 0.0, "dc", [], 0
        while at < len(parts):
            word = parts[at].lower()
            if word == "dc":
                dc, at = value(parts[at + 1]), at + 2
            elif word in ("sin", "pulse", "pwl"):
                shape, numbers = word, [value(p) for p in parts[at + 1:]]
                at = len(parts)
            elif word in ("ac",):
                raise Wrong("AC sources are not done")
            else:
                dc, at = value(parts[at]), at + 1
        if shape == "dc":
            return ["dc", dc]
        return [shape, numbers]

    def instance(self, parts, name, prefix, ports):
        settings = [p for p in parts[1:] if "=" in p]
        rest = [p for p in parts[1:] if "=" not in p]
        what = rest[-1].lower()
        if what in self.subckts:
            inner, body = self.subckts[what]
            if len(rest) - 1 != len(inner):
                raise Wrong(f"{what} has {len(inner)} ports, given {len(rest) - 1}")
            joined = {port: self.node(node, prefix, ports) for port, node in zip(inner, rest)}
            self.read(body, name.lower() + ".", joined)
            return
        declared = parameters(what)
        if declared is None:
            raise Wrong(f"'{what}' is no subcircuit and no model in {MODELS}")
        if len(rest) != 3:
            raise Wrong("a memristor has two nodes")
        given = {}
        for setting in settings:
            key, _, amount = setting.partition("=")
            given[key.lower()] = value(amount)
        names = [n.lower() for n, _ in declared]
        unknown = set(given) - set(names)
        if unknown:
            raise Wrong(f"{what} has no parameter {', '.join(sorted(unknown))}")
        numbers = [given.get(n.lower(), default) for n, default in declared]
        self.add("M", name, self.node(rest[0], prefix, ports), self.node(rest[1], prefix, ports),
                 [what, numbers])

    def control(self, first, parts, prefix, line):
        if prefix and first not in (".model",):
            raise Wrong(f"{first} inside a subcircuit")
        if first == ".tran":
            numbers = [value(p) for p in parts if "=" not in p and p.lower() != "uic"]
            if len(numbers) < 2:
                raise Wrong(".tran needs a step and a stop")
            if len(numbers) > 2 and numbers[2] != 0:
                raise Wrong(".tran from a later start is not done")
            self.tran = (numbers[0], numbers[1], numbers[3] if len(numbers) > 3 else None)
        elif first == ".op":
            self.op = True
        elif first in (".print", ".probe", ".save"):
            for kind, what in re.findall(r"\b([vViI])\s*\(([^)]*)\)", line):
                self.probes.append((kind.lower(), " ".join(what.replace(",", " ").split())))
        elif first in (".end", ".options", ".option", ".model"):
            pass
        else:
            raise Wrong(f"{first}, which is not done")

    def settle(self):
        """What can only be worked out with the whole circuit: the probes,
        and the waveforms' defaults that SPICE takes from the .tran."""
        probes = []
        for kind, what in self.probes:
            what = what.lower()
            if kind == "v":
                if " " in what:
                    raise Wrong(f"{self.path}: v(a,b) is not done; probe the two nodes")
                if what not in self.nodes and what not in ("0", "gnd"):
                    raise Wrong(f"{self.path}: no node '{what}' to probe")
                probes.append(("v", self.nodes.get(what, 0)))
            else:
                if what not in self.named:
                    raise Wrong(f"{self.path}: no device '{what}' to probe")
                probes.append(("i", self.named[what]))
        self.settled = probes
        step, stop = (self.tran[0], self.tran[1]) if self.tran else (1.0, 1.0)
        for index, (kind, name, a, b, numbers) in enumerate(self.devices):
            if kind in "VI" and numbers[0] == "sin":
                given = numbers[1]
                if len(given) < 2:
                    raise Wrong(f"{self.path}: {name}: SIN needs an offset and an amplitude")
                full = given + [1 / stop, 0.0, 0.0, 0.0][len(given) - 2:]
                self.devices[index] = (kind, name, a, b, ["sin", full[:6]])
            elif kind in "VI" and numbers[0] == "pulse":
                given = numbers[1]
                if len(given) < 2:
                    raise Wrong(f"{self.path}: {name}: PULSE needs two values")
                full = given + [0.0, step, step, stop, stop][len(given) - 2:]
                self.devices[index] = (kind, name, a, b, ["pulse", full[:7]])
            elif kind in "VI" and numbers[0] == "pwl":
                given = numbers[1]
                if len(given) < 2 or len(given) % 2:
                    raise Wrong(f"{self.path}: {name}: PWL needs pairs of time and value")
                times = given[0::2]
                if any(b <= a for a, b in zip(times, times[1:])):
                    raise Wrong(f"{self.path}: {name}: PWL's times are to rise")

    # --- writing -------------------------------------------------------

    def flat(self):
        lines = ["analog 1",
                 f"nodes {len(self.names)} devices {len(self.devices)} probes {len(self.settled)}"]
        lines += [f"node {index} {name}" for index, name in enumerate(self.names)]
        for kind, name, a, b, numbers in self.devices:
            if kind in "RC":
                lines.append(f"{kind} {a} {b} " + " ".join(text(n) for n in numbers))
            elif kind in "VI":
                shape, given = numbers
                if shape == "dc":
                    lines.append(f"{kind} {a} {b} dc {text(given)}")
                elif shape == "pwl":
                    lines.append(f"{kind} {a} {b} pwl {len(given) // 2} " +
                                 " ".join(text(n) for n in given))
                else:
                    lines.append(f"{kind} {a} {b} {shape} " + " ".join(text(n) for n in given))
            else:
                model, given = numbers
                lines.append(f"M {a} {b} {model} " + " ".join(text(n) for n in given))
        lines += [f"probe {kind} {which}" for kind, which in self.settled]
        if self.op:
            lines.append("op")
        if self.tran:
            step, stop, most = self.tran
            lines.append(f"tran {text(step)} {text(stop)}" + (f" {text(most)}" if most else ""))
        return "\n".join(lines) + "\n"

    def spice(self, title):
        """The flat circuit for ngspice: every device by the name the flat
        netlist's order gives it, every node by its number."""
        lines = [title]
        models = {}
        for index, (kind, name, a, b, numbers) in enumerate(self.devices):
            if kind in "RC":
                lines.append(f"{kind}{index} {a} {b} {text(numbers[0])}" +
                             (f" IC={text(numbers[1])}" if kind == "C" else ""))
            elif kind in "VI":
                shape, given = numbers
                body = f"DC {text(given)}" if shape == "dc" else \
                    shape.upper() + "(" + " ".join(text(n) for n in given) + ")"
                lines.append(f"{kind}{index} {a} {b} {body}")
            else:
                model, given = numbers
                names = [n for n, _ in parameters(model)]
                key = (model, tuple(given))
                if key not in models:
                    models[key] = f"{model}_{len(models)}"
                    lines.append(f".model {models[key]} {model} " +
                                 " ".join(f"{n}={text(v)}" for n, v in zip(names, given)))
                lines.append(f"N{index} {a} {b} {models[key]}")
        if self.op:
            lines.append(".op")
        if self.tran:
            step, stop, most = self.tran
            lines.append(f".tran {text(step)} {text(stop)}" + (f" 0 {text(most)}" if most else ""))
        shown = [f"v({which})" if kind == "v" else
                 f"i({self.devices[which][0]}{which})" for kind, which in self.settled]
        lines.append(".control")
        if models:
            for model in sorted({model for model, _ in models}):
                lines.append(f"pre_osdi {model}.osdi")
        lines.append("run")
        if shown:
            lines.append("set wr_singlescale")
            lines.append("set wr_vecnames")
            lines.append("option numdgt=17")
            lines.append("wrdata spice.txt " + " ".join(shown))
        lines.append(".endc")
        lines.append(".end")
        return "\n".join(lines) + "\n"


def found():
    """Every circuit there is: its name and its file."""
    circuits = {}
    for folder in (CIRCUITS, SOURCES):
        if os.path.isdir(folder):
            for file in sorted(os.listdir(folder)):
                if file.endswith(".cir"):
                    circuits[file[:-4]] = os.path.join(folder, file)
    return circuits


def prepare(name, path):
    with open(path) as file:
        title = file.readline().strip()
    circuit = Circuit(path)
    circuit.settle()
    folder = os.path.join(OUT, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "circuit.net"), "w") as file:
        file.write(circuit.flat())
    with open(os.path.join(folder, "circuit.cir"), "w") as file:
        file.write(circuit.spice(title))
    print(f"{name}: {len(circuit.names)} nodes, {len(circuit.devices)} devices, "
          f"{len(circuit.settled)} probes")


def main():
    names = sys.argv[1:]
    circuits = found()
    if names == ["all"]:
        names = list(circuits)
    if not names:
        sys.exit(__doc__)
    for name in names:
        path = circuits.get(name)
        if path is None and os.path.isfile(name):
            path, name = name, os.path.splitext(os.path.basename(name))[0]
        if path is None:
            sys.exit(f"no circuit '{name}'")
        try:
            prepare(name, path)
        except Wrong as wrong:
            sys.exit(str(wrong))
    return 0


if __name__ == "__main__":
    sys.exit(main())
