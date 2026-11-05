# 11 · Other languages behind devices, and v0.1

*31 October – 4 November 2026.*

## The rule this post builds on

Since 5 October ([post 04](04-devices.md)), code in other languages never gets
the world. A device is ent systems calling `extern fn` and `extern proc`:

- an `extern fn` is pure, so it may be called from any number of threads at
  once;
- an `extern proc` does anything, but only where the schedule says.

Buffers cross the border as a count plus one pointer per field. So far all
device code was C. This post covers what happened when it wasn't, and what
had to be checked once the compiler stopped taking "pure" on trust.

## Mojo, on the CPU

I'm a Mojo person, so Mojo came first. The device is a `.mojo` file, a `.build`
file holding one shell command, and declarations on the ent side:

```
extern fn mix(x: u64) -> u64
extern fn square_sum(n: i32) -> i64
extern proc dot(them: Samples) -> f64
```

```mojo
@export("ent_mojodev_mix")
def mix(x: UInt64) abi("C") -> UInt64:
    var z = x + 0x9E3779B97F4A7C15
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB
    return z ^ (z >> 31)
```

The spike needed no change to ent. The only addition was `$ENT_LINK`, where a
`.build` command may write linker flags it only learns while building. Mojo
needs it for its runtime libraries.

The results:

- A million calls from a parallel `for` were exact against Python.
- Lists were allocated on ent's threads, and errors raised in Mojo were caught
  before the C border, since a C-ABI export may not raise.
- Pure functions compile to the same machine code as C.
- The costs are real, though. Loading Mojo's runtime takes about 7.5 ms at
  start-up. Growing a `List` took 481 ms per million against C's 84 (115 with
  the capacity given).

My verdict at the time, and still: a Mojo device is worth it for what only Mojo
has, not for kernels you could write in C.

## Mojo, on the GPU, with answers that come later

What only Mojo has, for us, is a GPU stack that's easy to call. So the second
Mojo device is a *computing GPU*. The program hands it rows, the ticks go on,
and the answers arrive as **entities, some ticks later**:

```
buffer Starts { n: u64 } capacity 4000000
buffer Finished { n: u64, steps: i32, peak: u64 } capacity 4000000
component Chain { n: u64, steps: i32, peak: u64 } capacity 4000000

// The device's whole interface: two procs written in Mojo.
extern proc submit(device: i64, them: Starts) -> i64
extern proc take(device: i64, into: mut Finished) -> i64
```

The program fills the `Starts` buffer with the Collatz chains it wants
worked out. The device's two procs carry them to the GPU and back. Like
every `extern proc` (post 04), they get a buffer as a count and one
pointer per field, and nothing else of the world:

- `submit` copies the rows and returns a ticket. It also starts a detached
  thread that does everything touching the GPU, even opening it, so no tick
  ever waits for a driver.
- `take` reads an atomic flag the thread sets when it's done. If the job has
  finished, `take` fills `Finished` and returns the ticket. If not, it
  returns −1.

Two small ent systems call them. `send` hands `Starts` to `submit` when no job
is out. `collect` runs every tick, asks `take`, and spawns a `Chain` entity
for each row of a finished job (trimmed; the real one also notes a failed
job):

```
system collect() {
  if Gpu.out {
    let ticket = take(Gpu.device, Finished)
    if ticket > 0 {
      Gpu.out = false
      for i in 0..len(Finished) {
        spawn { Chain { n: Finished[i].n, steps: Finished[i].steps,
                        peak: Finished[i].peak } }
      }
    }
  }
}
```

The rest of the program doesn't know a GPU exists. Its schedule is
`give() collect() use()`, and `use` finds `Chain` entities in the tick when
they arrive, like any others. Only one job is out at a time.

Mojo's API has no non-blocking way to ask whether work is done, hence the
thread. Errors are stored and reported by `take`, not by the worker:
printing from the worker raced CUDA's teardown at exit in 17 runs out of 40.

`bench/mojo/run.py` computes Collatz chains on an RTX 4050 against ent on a
Ryzen AI 9 HX 370. Times in ms, medians of 7 processes:

| Rows | GPU, first job | GPU, warm | CPU, 1 thread | CPU, all threads |
|---|---|---|---|---|
| 100,000 | 190–196 | 0.9 | 6.8 | 6.8 |
| 1,000,000 | 134 | 9.3 | 83 | 6.7 |
| 4,000,000 | 165–169 | 36 | 364 | 29 |

Warm, the GPU is 7–10× one thread. **All threads beat it from a million rows
up.** What the GPU buys is that the ticks keep going during the job: a free
tick costs 110–145 ns with a job out, against 23 ns without one.

The first version of that table said otherwise. The laptop's firmware had
quietly capped the CPU at 2.0 GHz under multi-core load. A restart cleared
it, the numbers were taken again and the README rewritten. That's the
"measured, not explained" rule from day one, catching a thermal ceiling.

## Rust: a crate, checked against the declarations

Rust was worth a look for its crates. The device in `examples/rust` is a
static library that cargo builds. Its `build.rs` runs bindgen on the header
ent generates, and a four-line macro turns any mismatch into a compile
error:

```rust
#[no_mangle]
pub extern "C" fn ent_rustdev_mix(x: u64) -> u64 { /* … */ }
same_signature!(ent::ent_rustdev_mix, ent_rustdev_mix);
```

Declare `mix` as taking `u32` in ent, and Rust refuses to build. That's the
first device language where a signature mismatch can't reach run time.

`pngs.ent` reads 31 kinds of PNG through the `png` crate and through stb, and
gets the same colours from both. In the spike, the crate was 1.6–2.4× faster
than stb (7.5 against 12.3 ms per megapixel). There's no per-thread setup and
no start-up cost. Cross-language LTO doesn't work yet: rustc is on LLVM 23 and
ent on LLVM 22, and the bitcode won't read across.

## Purity, checked instead of trusted

Every parallel `for` that calls an `extern fn` relies on the word "pure".
Until 4 November that word was only a promise. Now it's checked twice:

- **At every build, from the IR.** `tools/ent` compiles the device's C to
  LLVM IR, optimises it, and reads what LLVM concludes about each function's
  memory effects:

  ```
  warning: extern fn 'counted' is no fn: its C (ent_counted in externs.c)
    writes calls. A fn may be called at another time, fewer times or on many
    threads at once: declare it 'extern proc'
  warning: extern fn 'scaled' may give another result for the same
    arguments: its C (ent_scaled in externs.c) reads scale.
  ```

  Allocating and freeing memory doesn't count against a function, and neither
  does a path that ends in an abort or a panic. Where a function really is pure
  but LLVM can't see why (a table filled once, an opaque library),
  `ENT_SAME_RESULT` in C vouches for it. An exported `ent_same_result_<name>`
  does the same in Rust or Mojo.
- **At run time, with `--check-externs`.** Every call is made twice and
  compared, and also compared with what the same arguments gave before. A
  mismatch aborts at the declaration.

Rust came next: cargo emits the crate's IR, and the check reads it like C's.
Then Mojo: a module's `.emit` file is a shell command that writes LLVM IR, and
the check optimises and reads it like C's. Mojo's IR quotes its names
(`@"std::…"`), which the first version missed for calls and globals.

The check found real problems right away. The window device's text-measuring
functions read fonts that other procedures load lazily, which is a race on
many threads. They're procs now, and that change is what v0.1.1 is.

## Three systems

- **macOS.** Threads can't be pinned on a Mac, and with one thread per core of
  two kinds, each waiting 5 ms before sleeping, a parallel build could be
  slower than a serial one. Now there's a thread per performance core, and
  idle threads sleep at once. Pixalorien's landing check went from 121–128 s
  (63 s on one core) to 32–41 s.
- **Windows.** `ent build --windows` makes a 64-bit `.exe` on Linux with
  llvm-mingw, and `ent run --windows` starts it under Wine. The runtime got
  `VirtualAlloc`, the performance counter and `_aligned_malloc`. Pixalorien
  under Wine gives the same digests as on Linux.
- **Linux** keeps doing the benchmarks, pinned, native and on mains power.

## Pictures as files

`devices/pictures.ent` reads PNG and JPEG into a buffer of colours and writes
them back out, with no window needed. `Window.dropped` reports a file dropped
on the window. Both exist because Pixalorien's postcards needed them
([post 10](10-pixalorien.md)). Both are also small, general and immediately
useful elsewhere: the Rust device compares itself against exactly this
module.

## v0.1

`v0.1.0` went on 4 November on the commit that added pictures as files, as
"the version Pixalorien builds against". `v0.1.1` followed the same day with
the purity checks, because they changed the window's procs. A version number
arrived not from a roadmap but because a game needed something stable to
build against. That fits.

## What's open

An honest selection from the list we keep:

- **Journaled buffers.** Pixalorien's undo log, made a language feature:
  record old values where they're written, roll back a tick, an HPC superstep,
  or a simulator.
- **The rough edges a real game found:** text types that depend on a literal's
  length, names colliding across modules, a buffer capacity that can't be a
  constant, and an unexplained 15-minute clang compile.
- **Language basics still missing:** early `return`, `match` over enums
  checked for every case, and a named group of buffers to pass as one
  parameter. Some of Doom's procs take about 25 parameters, and the lists were
  generated by a script, which is a smell.
- **Debug info.** No `.ent` locations survive lowering yet, so a crash points
  at generated code.
- **Bounds proofs.** Port my Mojo bounds-check prover to buffer accesses and
  spawn capacities.
- **Hot reload,** and layout migration so old saves load into a changed world.
- **Distribution.** Ghost entities fetched on request and placement by blocks,
  so a world can be larger than one machine.
- **GPU.** Spawning and destroying on the device, and compacting at run time.
- **Syntax.** Several syntax styles for one semantics, as promised in
  [post 00](00-why.md). It still sounds crazy. So did Doom.

Five weeks, 631 commits to the first tag. The loop runs: input, transform,
output, and the output becomes the next input. It's the same for a world of
entities and for building a language.

Back to the [index](README.md).
