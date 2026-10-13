#!/usr/bin/env python3
"""A processor runs a program as Icarus Verilog runs it (the spec's
fourth check).

  examples/logic/fetch.sh
  examples/logic/prepare.py cpu
  examples/logic/check/cpu.py [cycles] [--no-icarus]

The circuit is PicoRV32 (examples/logic/cpu.v), 30,000 ANDs and 1,900
flip-flops; its memory is outside the circuit, and the program in it is
PicoRV32's Dhrystone, built here with clang for RISC-V (the compiler's
own LLVM, or the one on the PATH). The model runs it four ways (gates as
entities, all of them or only those whose inputs changed; as rows; and
the circuit written out as code), and Icarus
runs the same gates with a memory that answers alike, in a testbench
made here. Every output of every cycle is to be the same in all five,
up to the cycle the processor stops in, and so is what the program
says; that is shown. With a number, no more than so many cycles.
"""

import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
LOGIC = os.path.join(ROOT, "examples", "logic")
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
SOURCES = os.path.join(ROOT, "build", "logic", "src", "picorv32", "dhrystone")
RUN = os.path.join(ROOT, "build", "logic", "cpu")
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import equal  # the netlist's names

INPUTS = ["resetn", "mem_ready"] + [f"mem_rdata[{bit}]" for bit in range(32)]
OUTPUTS = ["trap", "mem_valid", "mem_instr"] + [f"mem_addr[{bit}]" for bit in range(32)] + \
    [f"mem_wdata[{bit}]" for bit in range(32)] + [f"mem_wstrb[{bit}]" for bit in range(4)]


def tool(name):
    own = os.path.join(ROOT, "build", "toolchain", ".pixi", "envs", "default", "bin", name)
    prefix = os.environ.get("LLVM_PREFIX")
    for path in ([os.path.join(prefix, "bin", name)] if prefix else []) + [own]:
        if os.path.isfile(path):
            return path
    return shutil.which(name) or sys.exit(f"no {name}: it comes with LLVM")


def firmware():
    """Dhrystone for the processor: the bytes that lie from 0x10000 on."""
    folder = os.path.join(RUN, "firmware")
    os.makedirs(folder, exist_ok=True)
    objects = []
    for source in ("start.S", "dhry_1.c", "dhry_2.c", "stdlib.c"):
        objects.append(os.path.splitext(source)[0] + ".o")
        subprocess.run(
            [tool("clang"), "--target=riscv32", "-march=rv32im", "-mabi=ilp32", "-O3",
             "-std=gnu89", "-ffreestanding", "-nostdlib", "-fno-builtin", "-DTIME", "-DRISCV",
             "-DUSE_MYSTDLIB", "-w", "-c", os.path.join(SOURCES, source), "-o", objects[-1]],
            check=True, cwd=folder)
    subprocess.run([tool("ld.lld"), "-T", os.path.join(LOGIC, "firmware.lds"), "-o", "dhry.elf"]
                   + objects, check=True, cwd=folder)
    subprocess.run([tool("llvm-objcopy"), "-O", "binary", "dhry.elf", "dhry.bin"],
                   check=True, cwd=folder)
    return os.path.join(folder, "dhry.bin")


def build(source, built, more=()):
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", "-I", LOGIC,
                    *more, os.path.join(LOGIC, source), "-o", built],
                   check=True, env=environment)
    return built


def model(program, image, cycles, way, name):
    folder = os.path.join(RUN, name)
    os.makedirs(folder, exist_ok=True)
    shutil.copy(os.path.join(CIRCUITS, "cpu.aag"), os.path.join(folder, "circuit.aag"))
    shutil.copy(image, os.path.join(folder, "memory.bin"))
    inputs, outputs = equal.netlist("cpu")
    with open(os.path.join(folder, "pins.txt"), "w") as file:
        file.write(" ".join(str(inputs.index(pin)) for pin in INPUTS) + "\n" +
                   " ".join(str(outputs.index(pin)) for pin in OUTPUTS) + "\n")
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write(f"{cycles} 1 1 {way}\n")
    said = subprocess.run([program], check=True, capture_output=True, text=True,
                          cwd=folder).stdout.strip().split("\n")
    with open(os.path.join(folder, "outputs.txt")) as file:
        lines = [line for line in file.read().split("\n") if line]
    with open(os.path.join(folder, "console.txt"), errors="replace") as file:
        console = file.read()
    return lines, console, said[-1]


def icarus(image, cycles):
    folder = os.path.join(RUN, "icarus")
    os.makedirs(folder, exist_ok=True)
    data = open(image, "rb").read()
    with open(os.path.join(folder, "memory.hex"), "w") as file:
        file.write("\n".join(f"{byte:02x}" for byte in data) + "\n")
    _, outputs = equal.netlist("cpu")
    bench = f"""`timescale 1ns/1ns
module bench;
  reg clk = 0;
  reg resetn = 0;
  reg mem_ready = 0;
  reg [31:0] mem_rdata = 0;
  wire trap, mem_valid, mem_instr;
  wire [31:0] mem_addr, mem_wdata;
  wire [3:0] mem_wstrb;
  cpu dut(.clk(clk), .resetn(resetn), .trap(trap), .mem_valid(mem_valid),
          .mem_instr(mem_instr), .mem_ready(mem_ready), .mem_addr(mem_addr),
          .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb), .mem_rdata(mem_rdata));
  reg [31:0] memory [0:65535];
  reg [7:0] image [0:{len(data) - 1}];
  integer cycle, i, said;
  reg stop, next_ready;
  reg [31:0] next_rdata;
  initial begin
    for (i = 0; i < 65536; i = i + 1) memory[i] = 0;
    $readmemh("memory.hex", image);
    for (i = 0; i < {len(data)}; i = i + 1)
      memory[(65536 + i) >> 2] = memory[(65536 + i) >> 2] | (image[i] << ((i % 4) * 8));
    said = $fopen("console.txt", "w");
    stop = 0;
    for (cycle = 0; cycle < {cycles} && !stop; cycle = cycle + 1) begin
      #1;
      $display("%b", {{{", ".join(outputs)}}});
      next_ready = 0;
      next_rdata = mem_rdata;
      if (mem_valid && !mem_ready) begin
        next_ready = 1;
        if (mem_wstrb == 0)
          next_rdata = mem_addr[31:2] < 65536 ? memory[mem_addr[31:2]] : 0;
        else if (mem_addr == 32'h10000000)
          $fwrite(said, "%c", mem_wdata[7:0]);
        else if (mem_addr[31:2] < 65536) begin
          if (mem_wstrb[0]) memory[mem_addr[31:2]][7:0] = mem_wdata[7:0];
          if (mem_wstrb[1]) memory[mem_addr[31:2]][15:8] = mem_wdata[15:8];
          if (mem_wstrb[2]) memory[mem_addr[31:2]][23:16] = mem_wdata[23:16];
          if (mem_wstrb[3]) memory[mem_addr[31:2]][31:24] = mem_wdata[31:24];
        end
      end
      if (resetn && trap) stop = 1;
      clk = 1;
      #1;
      mem_ready = next_ready;
      mem_rdata = next_rdata;
      resetn = cycle + 1 >= 10;
      clk = 0;
    end
    $fclose(said);
    $finish;
  end
endmodule
"""
    with open(os.path.join(folder, "bench.v"), "w") as file:
        file.write(bench)
    subprocess.run(["iverilog", "-g2005", "-o", "bench.vvp", "bench.v",
                    os.path.join(CIRCUITS, "cpu.v")], check=True, cwd=folder)
    out = subprocess.run(["vvp", "-n", "bench.vvp"], check=True, capture_output=True,
                         text=True, cwd=folder).stdout
    with open(os.path.join(folder, "console.txt"), errors="replace") as file:
        console = file.read()
    return [line for line in out.split("\n") if line and set(line) <= set("01xz")], console


def main():
    arguments = sys.argv[1:]
    cycles = next((int(a) for a in arguments if a.isdigit()), 5000000)
    os.makedirs(RUN, exist_ok=True)
    image = firmware()
    program = build("cpu.ent", os.path.join(RUN, "cpu"))
    subprocess.run([sys.executable, os.path.join(LOGIC, "compile.py"), "cpu"], check=True,
                   stdout=subprocess.DEVNULL)
    coded = build("cpu_coded.ent", os.path.join(RUN, "cpu_coded"),
                  ["-I", os.path.join(ROOT, "build", "logic", "code", "cpu")])
    runs = [("as code", model(coded, image, cycles, 0, "coded")),
            ("as rows", model(program, image, cycles, 1, "rows")),
            ("as entities", model(program, image, cycles, 0, "entities")),
            ("as entities, by what changed", model(program, image, cycles, 2, "events"))]
    lines, console, said = runs[0][1]
    print(console.rstrip())
    print(f"({said})")
    failed = 0
    for name, (other, other_console, _) in runs[1:]:
        same = other == lines and other_console == console
        failed += 0 if same else 1
        print(f"{name}: {'the same' if same else 'NOT the same'} {len(other)} cycles")
    if "--no-icarus" not in arguments:
        wanted, wanted_console = icarus(image, cycles)
        differ = next((c for c, (a, b) in enumerate(zip(wanted, lines)) if a != b), None)
        if differ is None and len(wanted) == len(lines) and wanted_console == console:
            print(f"Icarus Verilog: the same {len(wanted)} cycles, and the same said")
        else:
            failed += 1
            print(f"Icarus Verilog: NOT the same: {len(wanted)} cycles, the model {len(lines)}"
                  + (f", first apart in cycle {differ}" if differ is not None else "")
                  + ("" if wanted_console == console else "; what is said differs"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
