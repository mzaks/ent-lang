# 08 · The GPU

*17 to 26 October 2026. The GPU work proper (G0, G1, G2) was done between 17
and 18 October. The indexes followed it there in the week after.*

## The bet

If the compiler knows who touches what, as day one promised, then *where* a
system runs is just one more execution freedom. A system that only touches
its own entity can run on 12 threads, or on two thousand.

All numbers in this post are from an RTX 4050 Laptop GPU next to an AMD Ryzen
AI 9 HX 370: a laptop's GPU, not a data centre's.

## G0: does upstream MLIR already do it?

Before designing anything, G0 asked a cheap question. We already had
entity-parallel loops. Would MLIR's own passes turn them into correct CUDA
kernels without any help from us?

They did. The toy from day one, at 10 million entities, ran a frame in
**3.75 ms with the world on the GPU against 14.5 ms on one core**, with the same
checksum. With the world left in host memory and read over PCIe each frame, it
took **29.5 ms, slower than one core**.

So the language part was obvious: the columns must *live* on the GPU between
frames, and the program must never have to say so.

## G1: one word

```
gpu system integrate(dt: f32) {
  for p: mut Position, v: Velocity {
    p.x += v.dx * dt
    p.y += v.dy * dt
  }
}
```

`gpu` is a contract, like `reads` and `writes` used to be:

- The compiler checks it **whether or not you build for a GPU**. A program
  can't silently lose its ability to run there.
- If a marked system can't run on the GPU, the compiler says so, with the
  reason.
- `ent build --gpu` puts the marked systems on the GPU. Without the flag, or
  without a GPU, the same program runs on the CPU.
- Results are the same **bit for bit** either way. That's the determinism rule
  every other kind of parallelism in ent-lang follows, and the GPU didn't get
  an exception.

Why on the system and not on the `for` or on the call? A system whose loops
run in two places waits for the GPU between them anyway. One marker per system
keeps that wait visible. A system that needs both places is two systems.

## Managed memory: the obvious idea, measured

The first version kept the world in CUDA managed memory and let the driver
move pages. As long as the CPU stayed away from what the GPU touched, it was
fine: 3.7 ms at 1e7.

When the CPU spawned into a column the GPU wrote, or read it back, managed
memory took 1.2 to 15.5 ms a frame. The CPU alone took 0.27 to 0.96 ms. Driver
hints and prefetching didn't fix it.

So we replaced it with **copies the compiler places itself**, derived from the
same access analysis that day one built:

- A column that `gpu` systems touch gets a second home on the GPU.
- After a GPU run, only what the CPU needs next is copied back, and only the
  rows in use.
- Rows the CPU spawned are copied as rows.

The results:

- the floor went from 11 µs a frame to 8 µs;
- a thousand spawns a frame into a million moving entities cost 0.04 ms, where
  managed memory took 1.2 ms;
- a fused sequence of `gpu` systems became one kernel over all its archetypes,
  for a floor of **6.3 µs**. One kernel launch plus one synchronise alone costs
  about 5.

## HMM, or: the bug that wasn't in our copies

This machine's NVIDIA driver runs with HMM (heterogeneous memory management).
Under HMM, a kernel that touches a *CPU* pointer doesn't fault. It reads
something, and the page may move to the GPU and back. A bug that should crash
instead gives a slightly wrong count, sometimes.

We found one of those in lookups: they read the CPU's entity counts inside a
kernel. Three lessons came out of it:

- The lowering now checks that nothing in a GPU loop uses the CPU's world
  (`checkGpuLoops`, an internal compiler error), and there's an
  `ENT_GPU_PROTECT=1` mode. Check those before suspecting the copies.
- Intermittent GPU failures hid under every probe that printed or waited. An
  in-memory ring buffer, dumped at exit, finally caught them.
- "Fixed" means 20 to 40 clean repetitions, not one.

## What it takes to stay bit-identical

G2 brought cross-entity work to the GPU: lookups, sends, accumulates, edge loops
and trees of one level. Every one of them collided with the same rule, because
floats added in another order give another sum. So:

- **The epidemic's hazard became whole numbers.** It counts units of 2⁻³²
  instead of fractions, so a day's sum is the same in any order, whether on
  one core, all of them, or the GPU. That costs 3% a day on one core and 11%
  on four threads. The epidemic curves still match the reference model.
- **`math` was rewritten in ent.** exp, log, pow, sin, cos and the rest now
  follow fdlibm's algorithms, built on what IEEE 754 rounds exactly. A
  `gpu system` gets the same bits as the CPU, where calling the GPU's own
  `libm` wouldn't.

## What ran there

**The city epidemic** has a million people, households, workplaces and chance
meetings. 100 days, ms per day:

| | a day | `workplace` |
|---|---|---|
| one core | 158.1 | 41.8 |
| four threads | 60.5 | 26.1 |
| one core and the GPU | **3.9** | 0.24 |

857,886 people are infected in every row. Before `math` moved into ent, the
GPU day was 18.8 ms.

**beecs** ran 100 honeybee colonies side by side, every forager its own
squadron, which is 2.5 million foraging places, for a year:

| | a year of 100 colonies |
|---|---|
| one core | 186.4 s |
| four threads | 184.2 s |
| the GPU, first version | 11.5 s |
| the GPU, after tuning | about 7 s |

The output was the same to the byte on the CPU, on threads and on the GPU.
All six scenarios still agree with beecs. The interesting part was a walk that
can't be parallel: each colony goes through its dancers in random order, one
GPU thread per colony. It turned out to be bound by compute once the loads were
batched. Four dancers at a time beat eight or sixteen.

## Indexes on the GPU

When the indexes of [post 09](09-indexes.md) arrived, they were made on the
CPU and copied over. For moving that onto the GPU I picked the Morton index
first. Claude had recommended the grid; I wanted the harder one first.

The rule we settled on needs no new syntax. **An index is made on the GPU when
the fields it is keyed on are written there and only `gpu` systems read it.**
Placement follows from access, again.

A million 2D dots moving every frame, the queries on the GPU, ms per frame
(`bench/moving`):

| | index made on the CPU | made on the GPU |
|---|---|---|
| grid | 28.0 | 11.5, then **9.5** |
| Morton | 38.7 | 31.1, then **23.3** |

The "then" column is a radix sort that works in tiles of 4,096 keys in the
GPU's shared memory. It cut a sort pass from 1.6 ms to 0.4 ms. The tiles needed
`gpu.launch` with workgroup memory and NVVM's `match.sync`, which is as close
to the metal as this project has gone.

Not everything paid off:

- **Partitioned stages** (sets of the ill, the recovered and so on) cost the
  epidemic 4.4 ms a day on the GPU, against 3.9 without them. Going over a
  million rows on the GPU is nearly free, so the index saved less than making
  it cost.
- **Morton *queries*** on the GPU are still about 2× slower than on the CPU.
  The walks aren't clustered.
- **k-d tree queries** on the GPU are bound by f64. In f32 the same query
  takes 90 ms instead of 147.

## Where this leaves the GPU

It's super early, and it isn't that powerful yet. Many things are still CPU-only:
spawning and destroying, text, sorted trees, reactive queries, and fractions sent
in the CPU's order. And the program has to say `gpu`.

But look at what it took. The epidemic went from 158 ms to 3.9 ms a day. On the
program's side that meant the word `gpu` in front of systems, a fraction made a
whole number, and a few loops reshaped so nothing walks a relation backwards.
No kernels, no copies, no streams. Everything else came
from the compiler knowing who touches what. That's the potential of the paradigm
I wanted to see.

A different kind of GPU work, a Mojo device that computes on the GPU and hands
results back as entities a few ticks later, is in
[post 11](11-other-languages-and-v0-1.md).

Next: [09 · Indexes as a language feature, and a SAT solver](09-indexes.md).
