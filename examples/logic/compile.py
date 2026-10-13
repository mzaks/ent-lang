#!/usr/bin/env python3
"""Writes a circuit as ent-lang code: every AND a line.

  examples/logic/prepare.py c6288
  examples/logic/compile.py c6288 [ANDs a system]

Reads build/logic/circuits/<name>.aag and writes the module `netcode`
into build/logic/code/<name>/netcode.ent, which examples/logic/coded.ent
runs: the compiler then has the whole circuit before it, as Verilator
has, where the model otherwise goes through a table of gates.

Every signal that is read far from where it is worked out is a field of
one unique, `Net`, at a place the compiler knows: an input, a flip-flop,
an output, and an AND that an AND of another system reads. Every other
AND is a `let` in its system. The ANDs are shared out among systems of
so many each (2,000), in the order of the file; the inputs (from
logic.ent's `Stim`), the outputs (to its `Seen`) and the flip-flops are
systems too, and the module's `schedule net()` is the circuit's part
of a clock cycle.
"""


import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
CODE = os.path.join(ROOT, "build", "logic", "code")


def as_fields(name, each, lines, inputs, flops, outputs, gates, limit=0):
    """The module's text, written to its file."""
    at = 1
    ins = [int(line) // 2 for line in lines[at:at + inputs]]
    at += inputs
    latches = [tuple(int(word) for word in line.split()) for line in lines[at:at + flops]]
    at += flops
    outs = [int(line) for line in lines[at:at + outputs]]
    at += outputs
    ands = [tuple(int(word) for word in line.split()) for line in lines[at:at + gates]]
    fanout = {}
    for _, a, b in ands:
        for literal in (a, b):
            fanout[literal // 2] = fanout.get(literal // 2, 0) + 1

    # An AND that is read only by ANDs of its own system (so many ANDs
    # each) is a `let` there and no field: nothing else needs its place.
    system_of = {out // 2: index // each for index, (out, _, _) in enumerate(ands)}
    field = set(ins) | {out // 2 for out, _ in latches} | \
        {source // 2 for _, source in latches} | {literal // 2 for literal in outs}
    readers = {}
    for index, (_, a, b) in enumerate(ands):
        for literal in (a, b):
            readers.setdefault(literal // 2, set()).add(index // each)
    for number, where in system_of.items():
        if readers.get(number, set()) - {where} or (limit and
                                                    len(readers.get(number, ())) and
                                                    fanout[number] > limit):
            field.add(number)

    def taken(literal):
        if literal < 2:
            return "~(0 as u64)" if literal else "(0 as u64)"
        value = f"Net.s{literal // 2}" if literal // 2 in field else f"g{literal // 2}"
        return f"~{value}" if literal & 1 else value

    numbers = ins + [out // 2 for out, _ in latches] + \
        [out // 2 for out, _, _ in ands if out // 2 in field]
    text = [f"// The circuit {name} as code: written by examples/logic/compile.py.",
            "import circuit", "import logic", "",
            "// Every signal's value, by its number.",
            "unique Net {"]
    text += ["  " + ", ".join(f"s{number}: u64" for number in numbers[i:i + 8]) + ","
             for i in range(0, len(numbers), 8)]
    text[-1] = text[-1].rstrip(",")
    text.append("}")
    if latches:
        text.append("// What each flip-flop takes at the clock's edge.")
        text.append("unique Next {")
        text += ["  " + ", ".join(f"s{out // 2}: u64" for out, _ in latches[i:i + 8]) + ","
                 for i in range(0, len(latches), 8)]
        text[-1] = text[-1].rstrip(",")
        text.append("}")
    text.append("")
    systems = []

    def system(stem, body):
        for part in range(0, max(1, len(body)), each):
            systems.append(f"{stem}{part // each}")
            text.append(f"system {systems[-1]}() {{")
            text.extend(body[part:part + each])
            text.append("}")
        text.append("")

    system("inputs", [f"  Net.s{number} = Stim[{index}].bits" for index, number in enumerate(ins)])
    system("ands", [f"  Net.s{out // 2} = {taken(a)} & {taken(b)}" if out // 2 in field else
                    f"  let g{out // 2} = {taken(a)} & {taken(b)}" for out, a, b in ands])
    system("outputs", [f"  Seen[{index}].bits = {taken(literal)}"
                       for index, literal in enumerate(outs)])
    if latches:
        system("reads", [f"  Next.s{out // 2} = {taken(source)}" for out, source in latches])
        system("steps", [f"  Net.s{out // 2} = Next.s{out // 2}" for out, _ in latches])
    text.append("// How many inputs and outputs the circuit has, for whoever is around it.")
    text.append("system sized() {")
    text.append(f"  Circuit.inputs = {len(ins)}")
    text.append(f"  Circuit.outputs = {len(outs)}")
    text.append("}")
    text.append("")
    text.append("// The circuit's part of a clock cycle: from the inputs in `Stim` to")
    text.append("// the outputs in `Seen`, and the flip-flops' step.")
    text.append("schedule net() {")
    text += [f"  {one}()" for one in systems]
    text.append("}")
    folder = os.path.join(CODE, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "netcode.ent"), "w") as file:
        file.write("\n".join(text) + "\n")
    print(f"{name}: {len(ands)} ANDs ({sum(1 for out, _, _ in ands if out // 2 in field)} "
          f"of them fields) and {len(latches)} flip-flops, {len(systems)} systems")
    return 0


def main():
    name = sys.argv[1]
    each = next((int(a) for a in sys.argv[2:] if a.isdigit()), 2000)
    with open(os.path.join(CIRCUITS, name + ".aag")) as file:
        lines = file.read().split("\n")
    _, _, inputs, flops, outputs, gates = lines[0].split()
    return as_fields(name, each, lines, int(inputs), int(flops), int(outputs), int(gates))


if __name__ == "__main__":
    sys.exit(main())
