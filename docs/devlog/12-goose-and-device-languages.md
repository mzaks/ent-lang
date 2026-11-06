# 12 · A device in Goose, and which language a device function wants

*4 – 6 November 2026.*

## Why another language

Since [post 04](04-devices.md), code in other languages gets values and
columns, never the world. [Post 11](11-other-languages-and-v0-1.md) put Mojo
and Rust behind that rule and started checking that an `extern fn` really is
pure. Then the rule got a stress test from a language that compiles to C.

[Goose](https://github.com/aardappel/goose), by Wouter van Oortmerssen, is
memory-safe, has no heap and turns into one C file. That makes it a good fit
for the functions a device hands to ent: a UDF written with bounds checked,
no allocator to get wrong and no pointer arithmetic, in code that reads
cleaner than the C it replaces. The first candidates are the C we still
write by hand, the parsers and file formats (WAD lumps, saves,
`GameConfig.bytes`). Where a device is a thin wrapper over a C library, the
library stays C. So we built a device, and it now lives in `examples/goose`.

## The device

The shape is the usual one plus a shim. `goose` makes `goosedev.goose` into
C, and `goosedev_shim.c` includes that C, maps ent's "count and one pointer
per field" onto Goose's slices, and calls `goose_init` from a constructor.
One function all the way through, the median of a buffer. ent declares it:

```
buffer Values { value: i64 } capacity 4096
extern proc median(values: Values) -> i64
```

Goose implements it, sorting a copy on its data stack:

```goose
export "goose_median" fn median(values: const i64[:]) -> i64 {
    if values.len == 0 { return 0; }
    var copy: i64[>..] = [];
    for v in values { copy.push(v); }
    sort(copy[..]);
    return copy[(copy.len - 1) / 2];
}
```

And the shim connects the two, turning the buffer's count and pointer into a
Goose slice:

```c
int64_t ent_goosedev_median(int32_t count, const int64_t *values) {
  sl_i64 them = {(int64_t *)values, count};
  return goose_median(them);
}
```

The shim includes `ent_extern.h`, so a signature that drifts from ent's
declaration is a compile error, not a wrong call. One surprise: `goose` reads
a `goosedev.build` next to its source as *its own* project file, and ent's
`goosedev.build` is one line of shell, so the build script compiles a copy of
the source in a folder of its own. Two languages, one file extension, zero
coordination.

In the spike the results were exact, and the speed was C's: parsing 10 MB
took 2.93 ms against C's 2.94. Then the parallel `for` gave a different wrong
total on every run, without a crash.

## The race nobody declared

The `i64[>..]` in `median` lives on Goose's *data stack*, and the data stacks
belong to the program instance. The spike also had `square_sum`, which
pushes the squares below `n` into such an array and adds them up, declared
an `extern fn` and called from a parallel `for`. A Goose program that starts
no threads is built without thread-local storage, so every ent worker
calling `square_sum` shared one stack. Each function was "pure" by the look of its source and racing by the
look of its C. Attaching each thread to its own stack fixed it in the spike.
I asked Wouter about it, and his answer was clear: threads managed in C talk
to Goose only on the main thread. The question of attaching threads is open
for discussion as [goose#232](https://github.com/aardappel/goose/issues/232).

That splits the device the way ent already splits devices:

- `mix` uses no data stack. It's an `extern fn`, and a `for` over a million
  entities calls it on all cores.
- `square_sum`, `parse` and `median` keep arrays on the data stack. They're
  `extern proc`s.

A failed check in an exported Goose function ends the program, and nothing on
the C side can catch it. So the parser checks what it's given and answers
-1 for a stray byte and -2 for too many numbers. A bad file is an answer, not
a crash.

## Does ent call procs on the main thread?

A question I hadn't asked before. It does, and now a test pins it. A proc is
called on the thread that runs the schedule, also from a `for` and also in a
`--parallel` build, because a `for` that calls a proc runs on that one
thread.

On many ranks it's different. Every partition runs on a thread of its own,
even a lone one, and main only waits. A proc in a `for` over distributed
entities is called by every partition for its own rows, several at once when
they share a process. So `ent build --ranks` now warns at such a call when the
proc's code keeps state. That matters for any device whose state belongs to
one thread, not just Goose's.

## What it taught the purity check, for every language

The check already read the IR of C, Rust and Mojo devices. Declared a fn,
`square_sum` passed anyway. The check looked for writes to globals, and Goose
writes *through a pointer it reads out of one*, `gs_stks`. Making the check
see that improved it for every language, not only Goose:

- **Writes through a global's pointer,** plain or thread-local, now count:

  ```
  warning: extern fn 'goosedev.square_sum' is no fn: its Goose
    (ent_goosedev_square_sum in goosedev.goose) writes what gs_stks points
    to. A fn may be called at another time, fewer times or on many threads
    at once: declare it 'extern proc'
  ```

  The Rust test's function over a `HashMap`, already caught by its call into
  the random keys, now also names the thread-local `KEYS` it writes.
- **Reads that only decide whether to stop don't count.** Every Goose export
  first checks that the runtime is started and panics if not. Read naively,
  that's a global read in every function, and `mix` would have been flagged
  as giving "another result for the same arguments". The same pattern shows
  up wherever a library asserts its own initialisation.
- **Smaller gaps closed:** atomic operations whose result is unused, and
  globals picked by a `select` or `phi`, used to slip past.
- **And one false alarm gone,** found by the benchmark below. A Rust fn that
  sorts an array of its own was flagged "writes memory other than its own".
  LLVM's attributes said `memory(write)`, because the sort stores through a
  `phi` of the array's parts. Now a function whose every write stays in its
  own stack is a fn, whatever the attributes say.

`test/Integration/goose.test` builds the example serially and `--parallel`,
compares the output byte for byte, and then flips `square_sum` to an
`extern fn` to see the warning come back. It runs where
`examples/goose/setup.sh` has built the Goose compiler.

## Which language for a device function?

Goose's own benchmarks put it 1.3–2.3× ahead of Rust, and I'd hoped arrays on
a data stack would beat a heap here too. So `bench/devices` runs one program
against a module `dev` written four times, in ent, C, Rust and Goose: the same
declarations, the same algorithm, written the way each language's programmer
would (a stack array in C, a `Vec` in Rust, the data stack in Goose, no array
at all in ent, whose fns have none). Every language gives the same sums.

The first finding was embarrassing. Device code was compiled for any x86-64
even when the program was built for this CPU, so every comparison so far had
pitted a native ent program against baseline C. Now device code follows the
program: C and C++ get the processor flags of `ENT_CFLAGS`, and a module's
`.build` gets them as `ENT_CPU`. The default stays "any x86-64", because a
game you give away has to run on someone else's machine.

Built for this CPU, ms, best of ten (one core / all cores):

| | ent | C | Rust | Goose |
|---|---:|---:|---:|---:|
| `mix`, per entity, 1M | **0.24** / 0.25 | 0.89 / 0.33 | 0.89 / 0.62 | 1.00 / 0.32 |
| `square_sum(n % 200)`, an array, per entity | **2.0** / 0.46 | 15.4 / 1.9 | 15.1 / 2.6 | 14.6 / 14.5 |
| `median16`, 16 numbers sorted on the stack | — | 111 / 8.9 | **96** / 8.2 | 110 / 9.5 |
| parse 1M numbers, 5.9 MB | 4.8 | 2.7 | **2.5** | 2.6 |
| median of 1M by quickselect | 12.2 | 6.9 | **6.8** | 7.0 |
| blur 2048 × 2048 bytes, 3 by 3 | 14.6 | **6.7** | 7.8 | 8.0 |

- **Per entity, ent's own fns win** by 4–7×. They're inlined into the `for`
  and vectorised, while an extern fn is a call LLVM can't see into. Where ent
  has no array, LLVM sums the squares without one.
- **Over a buffer, ent's procs lose,** about 2× behind all three. A proc's
  loop over rows is plain scalar code, and something in each row read or
  `+=` costs more than it should. That's now on my list.
- **C, Rust and Goose run at one speed,** within 20% of each other. Goose's
  wins in its own suite are in programs that build structures out of many
  small parts (records, an interpreter, word lists). Device code mostly reads
  one buffer and writes another, and there the data stack ties `Vec` and C's
  stack array.
- **Goose's real cost is threads.** `square_sum` keeps its array on the data
  stack, so it's a proc and stays on one core: 14.5 ms where C takes 1.9. A
  fixed array (`median16`) lives on the native stack, so that one is a fn and
  uses all cores like the others.
- **Built for any x86-64,** ent's `mix` takes 0.80 ms (no 64-bit vector
  multiply), and `square_sum` takes 54.7 in C, 54.1 in Goose and 29.1 in
  Rust. Everything else stays within a few percent.

Goose also taught me that it doesn't promote integers: `u8 + u8` is an 8-bit
add. The first Goose blur wrapped and gave a different sum from the other
three, which is exactly what the sums are for. One `as u32` mended it.

## What this settled

- **Write it in ent first.** A function that works on values belongs in ent,
  where it's inlined, vectorised and checked like the rest.
- **A device language is about safety and what's already written,** not
  speed. C, Rust and Goose are equally fast at device work. Goose buys
  memory safety without a borrow checker, and Rust buys its crates.
- **Arrays decide fn or proc in Goose.** Fixed arrays keep a function a fn;
  growing ones make it a proc, on one core, until goose#232 says otherwise.
- **The checks got better from a language that broke them.** Writes through
  pointers, way-out reads, thread-locals, own-stack arrays and procs on ranks
  now hold for every device language.
- **Measure native against native,** including the code you didn't write.

Back to the [index](README.md).
