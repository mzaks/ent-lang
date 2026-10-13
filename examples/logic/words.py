#!/usr/bin/env python3
"""Writes a circuit as ent-lang code word by word: every operation of
its Verilog a line.

  examples/logic/prepare.py cpu
  examples/logic/words.py cpu [operations a system]

Where examples/logic/compile.py writes a line for every AND of the
circuit taken apart into gates, this keeps the circuit's words: Yosys
reads the Verilog and leaves an addition an addition, a choice between
two 32-bit values one choice and a memory a memory
(build/logic/circuits/<name>.json), and each of these is a line on a
u64 here, a memory a buffer. That is what Verilator does with Verilog.
The module is `netcode` again, in build/logic/words/<name>/netcode.ent,
with the same `sized()` and `net()`, so examples/logic/coded.ent and
cpu_coded.ent run it in place of the gates: `net()` takes the inputs
bit by bit from logic.ent's `Stim` and gives the outputs to its `Seen`
in the order of the gates' netlist (<name>.aag, which is read for it).
The circuit's ports are also the fields of a unique, `Port`, whole
words, and `schedule core()` is the cycle between them without `Stim`
and `Seen` (cpu_words.ent).

A value is one run of the circuit here, not 64 side by side: the bits
of a word are next to each other in the u64 instead. Where an output is
1, all 64 bits of its row of `Seen` are.

With few enough operations (5,000 a system) the whole circuit is one
system: every signal a `let`, and the flip-flops, fields of the unique
`Net`, take their new values at its end. Otherwise a signal read in
another system is a field too, and the flip-flops' new values wait in
`Next`.

Signals wider than 64 bits are not done (a choice among several, which
Yosys writes as one wide signal, is), nor memories read at the clock's
edge, nor more than one clock.
"""

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
CODE = os.path.join(ROOT, "build", "logic", "words")
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import prepare  # which file of Verilog a circuit is

WORDS = {"if", "else", "for", "in", "let", "var", "fn", "system", "unique", "buffer", "loop",
         "until", "while", "where", "import", "const", "as", "true", "false", "mut", "match",
         "schedule", "main", "world", "entity", "relation", "capacity", "by", "turn", "return"}


def netlist(name):
    """The circuit's words, as Yosys writes them down."""
    circuits = prepare.found()
    if name not in circuits:
        sys.exit(f"no circuit '{name}' among those fetched")
    out = os.path.join(CIRCUITS, name + ".json")
    top = "-top cpu" if name == "cpu" else "-auto-top"
    script = f"""
        read_verilog {circuits[name]}
        hierarchy {top}
        proc; flatten; opt -full
        opt_mem; memory_collect
        wreduce; peepopt; opt_clean; opt -full
        async2sync; dffunmap
        setundef -zero
        opt_expr -full; opt_muxtree; opt_merge; opt_clean -purge
        rename -top {name}
        write_json {out}
    """
    done = subprocess.run(["yosys", "-q", "-p", script], capture_output=True, text=True)
    if done.returncode:
        sys.exit(f"{name}: yosys failed\n{done.stdout[-2000:]}\n{done.stderr[-2000:]}")
    with open(out) as file:
        return json.load(file)["modules"][name]


def pins(name):
    """The names of the gates' inputs and outputs, in their netlist's order."""
    with open(os.path.join(CIRCUITS, name + ".aag")) as file:
        lines = file.read().split("\n")
    _, _, inputs, flops, outputs, gates = lines[0].split()
    names = {"i": [None] * int(inputs), "o": [None] * int(outputs)}
    for line in lines[1 + int(inputs) + int(flops) + int(outputs) + int(gates):]:
        if line == "c":
            break
        if line[:1] in names and line[1:2].isdigit():
            at, symbol = line.split(" ", 1)
            names[at[0]][int(at[1:])] = symbol
    return names["i"], names["o"]


def number(text):
    return int(text, 2) if isinstance(text, str) else int(text)


def mask(width):
    return f"0x{(1 << width) - 1:x}"


class Circuit:
    def __init__(self, name, module, each):
        self.name, self.each = name, each
        self.driver = {}      # a bit: the signal it is of, and which of its bits
        self.width = {}       # a signal: how many bits
        self.nodes = []       # (signal, the bits it reads, what writes its line)
        self.flops = []       # (signal, the bits it takes, its first value)
        self.memories = []
        self.ports = {}
        self.label = {}       # a signal: the system it is worked out in
        self.field = set()
        self.undriven = 0
        self.read(module)

    # --- reading -------------------------------------------------------

    def read(self, module):
        first = {}
        for net in module["netnames"].values():
            init = net.get("attributes", {}).get("init")
            if init:
                for bit, value in zip(net["bits"], reversed(init)):
                    if value == "1":
                        first[bit] = 1
        clocks = set()
        for name, port in module["ports"].items():
            if len(port["bits"]) > 64:
                sys.exit(f"{self.name}: the port {name} is wider than 64 bits")
            self.ports[name] = port
        for cell_name, cell in sorted(module["cells"].items()):
            kind, joins = cell["type"], cell["connections"]
            parameters = {key: value for key, value in cell.get("parameters", {}).items()}
            if kind == "$dff":
                if number(parameters["CLK_POLARITY"]) != 1:
                    sys.exit(f"{self.name}: a flip-flop at the clock's falling edge")
                clocks.add(joins["CLK"][0])
                signal = ("q", len(self.flops))
                value = sum(first.get(bit, 0) << at for at, bit in enumerate(joins["Q"]))
                self.own(signal, joins["Q"])
                self.flops.append((signal, joins["D"], value))
            elif kind == "$mem_v2":
                self.memory(cell_name, parameters, joins, clocks)
            elif "Y" in joins:
                signal = ("c", len(self.nodes))
                self.own(signal, joins["Y"])
                reads = [bit for port, bits in joins.items() if port != "Y" for bit in bits]
                self.nodes.append((signal, reads, (kind, parameters, joins)))
            else:
                sys.exit(f"{self.name}: a cell {kind}, which is not done")
        if len(clocks) > 1:
            sys.exit(f"{self.name}: more than one clock")
        self.clock = None
        for name, port in list(self.ports.items()):
            if port["direction"] == "input" and clocks and port["bits"] == list(clocks):
                self.clock = name
                del self.ports[name]
            elif port["direction"] == "input":
                self.own(("p", name), port["bits"])
        if clocks and not self.clock:
            sys.exit(f"{self.name}: the clock is no input")
        for signal, bits in list(self.width.items()):
            if bits > 64:
                sys.exit(f"{self.name}: a signal of {bits} bits, wider than 64")

    def own(self, signal, bits):
        self.width[signal] = len(bits)
        for at, bit in enumerate(bits):
            if not isinstance(bit, str):
                self.driver[bit] = (signal, at)

    def memory(self, cell_name, parameters, joins, clocks):
        size, wide = number(parameters["SIZE"]), number(parameters["WIDTH"])
        address, offset = number(parameters["ABITS"]), number(parameters["OFFSET"])
        if wide > 64:
            sys.exit(f"{self.name}: a memory of words wider than 64 bits")
        if "1" in parameters["RD_CLK_ENABLE"]:
            sys.exit(f"{self.name}: a memory read at the clock's edge")
        if "0" in parameters["WR_CLK_ENABLE"] or "0" in parameters["WR_CLK_POLARITY"]:
            sys.exit(f"{self.name}: a memory not written at the clock's rising edge")
        index = len(self.memories)
        init = parameters["INIT"]
        words = [init[len(init) - (at + 1) * wide:len(init) - at * wide] for at in range(size)]
        first = [int(word.replace("x", "0"), 2) if len(word) == wide else 0 for word in words]
        writes = []
        for port in range(number(parameters["WR_PORTS"])):
            clocks.add(joins["WR_CLK"][port])
            writes.append((joins["WR_ADDR"][port * address:(port + 1) * address],
                           joins["WR_DATA"][port * wide:(port + 1) * wide],
                           joins["WR_EN"][port * wide:(port + 1) * wide]))
        self.memories.append((index, size, wide, offset, address, first, writes))
        for port in range(number(parameters["RD_PORTS"])):
            signal = ("c", len(self.nodes))
            self.own(signal, joins["RD_DATA"][port * wide:(port + 1) * wide])
            at = joins["RD_ADDR"][port * address:(port + 1) * address]
            self.nodes.append((signal, at, ("read", index, at)))

    # --- the order, and which signal is a field ------------------------

    def signals(self, bits):
        return {self.driver[bit][0] for bit in bits
                if not isinstance(bit, str) and bit in self.driver}

    def order(self):
        """The operations, each after those it reads."""
        at = {signal: index for index, (signal, _, _) in enumerate(self.nodes)}
        needs = [sorted(at[s] for s in self.signals(reads) if s in at)
                 for _, reads, _ in self.nodes]
        done, ordered = [0] * len(self.nodes), []
        for start in range(len(self.nodes)):
            if done[start]:
                continue
            stack = [(start, 0)]
            done[start] = 1
            while stack:
                node, edge = stack.pop()
                if edge < len(needs[node]):
                    stack.append((node, edge + 1))
                    other = needs[node][edge]
                    if done[other] == 1:
                        sys.exit(f"{self.name}: its words go round in a ring")
                    if not done[other]:
                        done[other] = 1
                        stack.append((other, 0))
                else:
                    done[node] = 2
                    ordered.append(node)
        self.nodes = [self.nodes[index] for index in ordered]

    def place(self):
        """Which system works out each signal, and which signals another reads."""
        systems = max(1, -(-len(self.nodes) // self.each))
        self.fused = systems == 1
        for index, (signal, _, _) in enumerate(self.nodes):
            self.label[signal] = index // self.each
        def read_in(bits, where):
            for signal in self.signals(bits):
                if signal[0] == "c" and self.label[signal] != where:
                    self.field.add(signal)
        for signal, reads, _ in self.nodes:
            read_in(reads, self.label[signal])
        for name, port in self.ports.items():
            if port["direction"] == "output":
                read_in(port["bits"], 0 if self.fused else "outputs")
        for _, takes, _ in self.flops:
            read_in(takes, 0 if self.fused else "reads")
        for memory in self.memories:
            for write in memory[6]:
                for bits in write:
                    read_in(bits, 0 if self.fused else "reads")

    # --- a value as text -----------------------------------------------

    def named(self, signal):
        kind, which = signal
        if kind == "p":
            return f"Port.{port_name(which)}"
        if kind == "q":
            return f"Net.q{which}"
        return f"Net.c{which}" if signal in self.field else f"c{which}"

    def value(self, bits):
        """These bits as one word, the first the lowest."""
        constant, pieces, at = 0, [], 0
        while at < len(bits):
            bit = bits[at]
            if isinstance(bit, str) or bit not in self.driver:
                if bit == "1":
                    constant |= 1 << at
                elif not isinstance(bit, str):
                    self.undriven += 1
                at += 1
                continue
            signal, first = self.driver[bit]
            many = 1
            while at + many < len(bits) and not isinstance(bits[at + many], str) and \
                    self.driver.get(bits[at + many]) == (signal, first + many):
                many += 1
            text = self.named(signal)
            if first:
                text = f"({text} >> {first})"
            if first + many < self.width[signal]:
                text = f"({text} & {mask(many)})"
            if at:
                text = f"({text} << {at})"
            pieces.append(text)
            at += many
        if not pieces:
            return f"(0x{constant:x} as u64)"
        if constant:
            pieces.append(f"0x{constant:x}")
        return pieces[0] if len(pieces) == 1 else "(" + " | ".join(pieces) + ")"

    def taken(self, bits, signed, wide):
        """An operand for a result of so many bits: cut to them, or with its
        sign drawn out where it is shorter."""
        if len(bits) >= wide:
            return self.value(bits[:wide])
        if signed:
            return f"(sext({self.value(bits)}, {len(bits)}) as u64)"
        return self.value(bits)

    def line(self, what):
        """What stands right of the `=` for an operation."""
        if what[0] == "read":
            _, index, at = what
            _, size, _, offset, address, _, _ = self.memories[index]
            where = self.value(at)
            if offset == 0 and size >= 1 << address:
                return f"Mem{index}[{where} as i32].word"
            return (f"if {where} >= {offset} && {where} < {offset + size} "
                    f"{{ Mem{index}[({where} - {offset}) as i32].word }} else {{ 0 }}")
        kind, parameters, joins = what
        a = joins.get("A", [])
        b = joins.get("B", [])
        wide = len(joins["Y"])
        a_signed = number(parameters.get("A_SIGNED", "0")) == 1
        b_signed = number(parameters.get("B_SIGNED", "0")) == 1
        both = a_signed and b_signed
        cut = "" if wide >= 64 else f" & {mask(wide)}"

        def same(bits, other):
            """An operand of a comparison, as wide as the other."""
            if both and len(bits) != len(other):
                return f"sext({self.value(bits)}, {len(bits)})"
            return self.value(bits)

        if kind in ("$and", "$or", "$xor", "$xnor", "$add", "$sub", "$mul"):
            sign = {"$and": "&", "$or": "|", "$xor": "^", "$xnor": "^",
                    "$add": "+", "$sub": "-", "$mul": "*"}[kind]
            text = f"({self.taken(a, both, wide)} {sign} {self.taken(b, both, wide)})"
            if kind == "$xnor":
                text = f"(~{text})"
            plain = kind in ("$and", "$or", "$xor") and not (
                both and (len(a) < wide or len(b) < wide))
            return text if plain else text + cut
        if kind == "$not":
            return f"(~{self.taken(a, a_signed, wide)}){cut}"
        if kind == "$pos":
            return f"{self.taken(a, a_signed, wide)}{cut}"
        if kind == "$neg":
            return f"(0 - {self.taken(a, a_signed, wide)}){cut}"
        if kind in ("$lt", "$le", "$gt", "$ge", "$eq", "$ne", "$eqx", "$nex"):
            sign = {"$lt": "<", "$le": "<=", "$gt": ">", "$ge": ">=", "$eq": "==",
                    "$ne": "!=", "$eqx": "==", "$nex": "!="}[kind]
            if both and kind in ("$lt", "$le", "$gt", "$ge"):
                return (f"bit(sext({self.value(a)}, {len(a)}) {sign} "
                        f"sext({self.value(b)}, {len(b)}))")
            return f"bit({same(a, b)} {sign} {same(b, a)})"
        if kind == "$logic_not":
            return f"({self.value(a)} ^ 1)" if len(a) == 1 else f"bit({self.value(a)} == 0)"
        if kind in ("$reduce_or", "$reduce_bool"):
            return self.value(a) if len(a) == 1 else f"bit({self.value(a)} != 0)"
        if kind == "$reduce_and":
            return self.value(a) if len(a) == 1 else f"bit({self.value(a)} == {mask(len(a))})"
        if kind == "$reduce_xor":
            return f"odd({self.value(a)})"
        if kind == "$reduce_xnor":
            return f"(odd({self.value(a)}) ^ 1)"
        if kind in ("$logic_and", "$logic_or"):
            if len(a) == 1 and len(b) == 1:
                return f"({self.value(a)} {'&' if kind == '$logic_and' else '|'} {self.value(b)})"
            sign = "&&" if kind == "$logic_and" else "||"
            return f"bit({self.value(a)} != 0 {sign} {self.value(b)} != 0)"
        if kind in ("$shl", "$sshl"):
            if len(b) <= 6:
                return f"({self.taken(a, a_signed, wide)} << {self.value(b)}){cut}"
            return f"shl({self.taken(a, a_signed, wide)}, {self.value(b)}){cut}"
        if kind == "$sshr" and a_signed:
            return f"sar(sext({self.value(a)}, {len(a)}), {self.value(b)}){cut}"
        if kind in ("$shr", "$sshr", "$shift", "$shiftx"):
            most = max(wide, len(a))
            text = self.value(a)
            if a_signed and kind != "$shiftx" and len(a) < most:
                text = f"(sext({text}, {len(a)}) as u64)" + \
                    ("" if most >= 64 else f" & {mask(most)}")
                text = f"({text})"
            if kind in ("$shift", "$shiftx") and b_signed:
                return f"shift({text}, sext({self.value(b)}, {len(b)})){cut}"
            if len(b) <= 6:
                return f"({text} >> {self.value(b)}){cut}"
            return f"shr({text}, {self.value(b)}){cut}"
        if kind in ("$div", "$mod"):
            if both:
                return (f"{'sdiv' if kind == '$div' else 'smod'}(sext({self.value(a)}, {len(a)}), "
                        f"sext({self.value(b)}, {len(b)})){cut}")
            return f"{'udiv' if kind == '$div' else 'umod'}({self.value(a)}, {self.value(b)}){cut}"
        if kind == "$mux":
            return (f"if {self.value(joins['S'])} != 0 {{ {self.value(b)} }} "
                    f"else {{ {self.value(a)} }}")
        if kind == "$pmux":
            text = ""
            for at, select in enumerate(joins["S"]):
                text += (f"if {self.value([select])} != 0 "
                         f"{{ {self.value(b[at * wide:(at + 1) * wide])} }} else ")
            return text + f"{{ {self.value(a)} }}"
        sys.exit(f"{self.name}: a cell {kind}, which is not done")

    # --- the module ----------------------------------------------------

    def write(self):
        self.order()
        self.place()
        ins, outs = pins(self.name)
        text = [f"// The circuit {self.name} word by word: written by examples/logic/words.py.",
                "import circuit", "import logic", "import words", ""]

        def unique(name, fields, said):
            if not fields:
                return
            text.append(said)
            text.append(f"unique {name} {{")
            text.extend("  " + ", ".join(f"{one}: u64" for one in fields[i:i + 8]) + ","
                        for i in range(0, len(fields), 8))
            text[-1] = text[-1].rstrip(",")
            text.append("}")

        unique("Port", [port_name(name) for name in self.ports],
               "// The circuit's inputs and outputs, a word each.")
        unique("Net", [f"q{which}" for (_, which), _, _ in self.flops] +
               [f"c{which}" for _, which in sorted(self.field, key=lambda s: s[1])],
               "// The flip-flops, and what one system works out for another.")
        writes = [(memory[0], port) for memory in self.memories
                  for port in range(len(memory[6]))]
        if not self.fused:
            unique("Next", [f"q{which}" for (_, which), _, _ in self.flops] +
                   [f"m{index}_{port}_{part}" for index, port in writes
                    for part in ("at", "word", "bits")],
                   "// What the flip-flops and the memories take at the clock's edge.")
        for index, size, _, _, _, _, _ in self.memories:
            text.append(f"buffer Mem{index} {{ word: u64 }} capacity {max(size, 1)}")
        text.append("")
        systems = []

        def system(stem, body, whole=False):
            each = max(1, len(body)) if whole else self.each
            for part in range(0, max(1, len(body)), each):
                systems.append(f"{stem}{part // each}")
                text.append(f"system {systems[-1]}() {{")
                text.extend(body[part:part + each])
                text.append("}")
            text.append("")

        def kept(signal, what):
            target = self.named(signal)
            return f"  {target} = {self.line(what)}" if signal in self.field else \
                f"  let {target} = {self.line(what)}"

        def written(index, port, at, word, bits):
            _, size, wide, offset, _, _, _ = self.memories[index]
            row = f"Mem{index}[({at} - {offset}) as i32].word"
            inside = f"{bits} != 0 && {at} >= {offset} && {at} < {offset + size}"
            return (f"  if {inside} {{ {row} = ({row} & ~{bits}) | ({word} & {bits}) }}")

        work = [kept(signal, what) for signal, _, what in self.nodes]
        shown = [f"  Port.{port_name(name)} = {self.value(port['bits'])}"
                 for name, port in self.ports.items() if port["direction"] == "output"]
        if self.fused:
            # All in one: the new values are worked out before any is taken.
            edge = [f"  let d{which} = {self.value(takes)}" for (_, which), takes, _ in self.flops]
            for index, port in writes:
                at, word, bits = self.memories[index][6][port]
                edge += [f"  let m{index}_{port}_at = {self.value(at)}",
                         f"  let m{index}_{port}_word = {self.value(word)}",
                         f"  let m{index}_{port}_bits = {self.value(bits)}"]
            edge += [f"  Net.q{which} = d{which}" for (_, which), _, _ in self.flops]
            edge += [written(index, port, f"m{index}_{port}_at", f"m{index}_{port}_word",
                             f"m{index}_{port}_bits") for index, port in writes]
            system("work", work + shown + edge, True)
        else:
            system("work", work)
            system("shown", shown)
            reads = [f"  Next.q{which} = {self.value(takes)}" for (_, which), takes, _ in self.flops]
            for index, port in writes:
                at, word, bits = self.memories[index][6][port]
                reads += [f"  Next.m{index}_{port}_at = {self.value(at)}",
                          f"  Next.m{index}_{port}_word = {self.value(word)}",
                          f"  Next.m{index}_{port}_bits = {self.value(bits)}"]
            system("reads", reads)
            system("steps", [f"  Net.q{which} = Next.q{which}" for (_, which), _, _ in self.flops] +
                   [written(index, port, f"Next.m{index}_{port}_at", f"Next.m{index}_{port}_word",
                            f"Next.m{index}_{port}_bits") for index, port in writes])
        core = list(systems)

        # The ports bit by bit, in the order of the gates' netlist.
        where = {}
        for name, port in self.ports.items():
            many, low = len(port["bits"]), port.get("offset", 0)
            for at in range(many):
                index = low + (many - 1 - at if port.get("upto") else at)
                where[f"{name}[{index}]"] = (name, at)
            if many == 1:
                where[name] = (name, 0)
        packed = {}
        for index, symbol in enumerate(ins):
            if symbol not in where:
                sys.exit(f"{self.name}: the gates' input '{symbol}' is no port of the words")
            name, at = where[symbol]
            packed.setdefault(name, []).append(f"((Stim[{index}].bits & 1) << {at})")
        system("pack", [f"  Port.{port_name(name)} = " + " | ".join(parts)
                        for name, parts in packed.items()])
        unpacked = []
        for index, symbol in enumerate(outs):
            if symbol not in where:
                sys.exit(f"{self.name}: the gates' output '{symbol}' is no port of the words")
            name, at = where[symbol]
            unpacked.append(f"  Seen[{index}].bits = 0 - ((Port.{port_name(name)} >> {at}) & 1)")
        system("unpack", unpacked)

        text.append("// How many inputs and outputs the circuit has, bit by bit, and what the")
        text.append("// flip-flops and the memories start with.")
        text.append("system sized() {")
        text.append(f"  Circuit.inputs = {len(ins)}")
        text.append(f"  Circuit.outputs = {len(outs)}")
        text.extend(f"  Net.q{which} = 0x{value:x}" for (_, which), _, value in self.flops)
        for index, size, _, _, _, first, _ in self.memories:
            text.append(f"  Mem{index} = []")
            text.append(f"  for i in 0..{size} {{ Mem{index} += {{ word: 0 }} }}")
            text.extend(f"  Mem{index}[{at}].word = 0x{word:x}"
                        for at, word in enumerate(first) if word)
        text.append("}")
        text.append("")
        text.append("// A clock cycle from the ports' inputs to their outputs, and the")
        text.append("// flip-flops' step.")
        text.append("schedule core() {")
        text.extend(f"  {one}()" for one in core)
        text.append("}")
        text.append("")
        text.append("// The same from the inputs in `Stim` to the outputs in `Seen`.")
        text.append("schedule net() {")
        packs = [one for one in systems if one.startswith("pack")]
        unpacks = [one for one in systems if one.startswith("unpack")]
        if self.fused:
            text.extend(f"  {one}()" for one in packs + core + unpacks)
        else:
            # (The outputs are noted before the flip-flops step.)
            early = [one for one in core if not one.startswith(("reads", "steps"))]
            late = [one for one in core if one.startswith(("reads", "steps"))]
            text.extend(f"  {one}()" for one in packs + early + unpacks + late)
        text.append("}")
        folder = os.path.join(CODE, self.name)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "netcode.ent"), "w") as file:
            file.write("\n".join(text) + "\n")
        bits = sum(len(takes) for _, takes, _ in self.flops)
        print(f"{self.name}: {len(self.nodes)} operations, {len(self.flops)} flip-flop words "
              f"({bits} bits), {len(self.memories)} memories, {len(core)} systems" +
              (f", {self.undriven} bits that nothing gives" if self.undriven else ""))


def port_name(name):
    clean = re.sub(r"\W", "_", name)
    if clean[:1].isdigit() or clean in WORDS:
        clean = "p_" + clean
    return clean


def main():
    name = sys.argv[1]
    each = next((int(a) for a in sys.argv[2:] if a.isdigit()), 5000)
    Circuit(name, netlist(name), each).write()
    return 0


if __name__ == "__main__":
    sys.exit(main())
