# 03 · From dialect to language

*1 to 3 October 2026, about 37 commits: a name, a syntax, relations, a
spiking neural network chased down to C speed, a second machine, and programs
that run without a C host.*

## A name

On 1 October ecs-lang became **ent-lang**: the dialect `ecs` became `ent`,
`ecs-opt` became `ent-opt`, and generated C now says `ent_world_create`. Prose
about the ECS pattern kept the term. `bench/RESULTS.md` kept the old names in
the entries measured before the rename, and says so at the top, because
editing old measurements to match a new name is how results files start lying.
All 44 tests passed, and no behaviour changed. A rename that's a commit and
not a migration is a nice perk of being four days old.

## A syntax, finally

`ent-translate --import-ent` parses `.ent` files. It's a hand-written lexer
and a recursive-descent parser that builds ent IR as it goes. Every op is
located at its source line and column, so the dialect's verifier reports
errors in the `.ent` file and not in IR nobody wrote.

The bar was that the syntax had to express what the dialect already did, and
nothing new. So v1 had:

- components, tags, uniques and archetypes;
- `for` with filters and reactive triggers;
- `let`, if-expressions, `+=`, `min=` and `max=`;
- `C(id).f` for another entity's field;
- `spawn`, `destroy`, `add` and `remove`;
- schedules.

A literal takes the type of the operand it meets, and there are no implicit
conversions. Everything else (`fn`, `proc`, relations, loops) was reported as
"not supported yet".

Here is the reactive health bar from [post 02](02-the-compiler-owns-the-world.md)
as it was written in the dialect:

```mlir
ecs.system @redraw() reads [@Hull] writes [@Bar] {
  ecs.query (%h: !ecs.ref<@Hull>, %b: !ecs.ref<@Bar, mut>)
      on [changed @Hull "hp"] {
    ...
```

The same system in ent-lang:

```
system redraw() {
  for h: Hull, b: mut Bar on changed Hull.hp {
    b.width = h.hp
    b.redraws += 1
  }
}
```

**The examples were translated, not rewritten.** Each integration test now
also imports the `.ent` version, links it with the same host and expects the
same output as the `.mlir` one. The translations keep the order of
floating-point operations, so the results agree to the bit. The syntax got
its own test, the old one.

## Filters, `has`, and conditional runs

Before this, filtering by a component meant binding a ref and never using it.
Now there's `for e, h: mut Hull with Enemy, any(Fire, Ice) without Shield`.
Where an archetype always or never has the component, the decision is made at
compile time. Where the component is optional, the test joins the
branch-free mask. A filter reads only presence, so filtering by `Shield`
doesn't conflict with a system writing Shield's fields.

`e.has(C)` answers **as of the query's start**. "Now" would depend on storage:
an optional component flips at once, while a move lands at the query's end.
Answering as of the start makes `if e.has(S) { e.remove(S) } else { e.add(S) }`
behave the same on both storages, which the filters example checks by running
once with presence bytes and once with moves.

`run_if` makes a run or a whole schedule conditional. The condition is
evaluated after the earlier runs have committed. The first version created the
column views inside the condition's branch, and the IR didn't verify. It was
caught at once, which is what verifiers are for.

## Relations: edges with data

A field of type `!ent.entity` links one entity to one other. A spiking network
has a thousand synapses per neuron, each with a weight. So relations became
their own thing:

```
relation Synapse { weight: f32 } capacity 65536

system propagate() {
  for e with Spiked {
    for s, post in e.out(Synapse) {
      Neuron(post).input += s.weight
    }
  }
}
```

Edges aren't entities. They're a table of source, target and one column per
field, kept **sorted by source** in compressed sparse rows, with offsets per
entity. Sorting by target and an index are added where the program visits
incoming edges.

Flecs makes each target a pair component. That would have put every distinct
set of targets in an archetype of its own, which, with a thousand random
targets per neuron, is a lot of archetypes.

Connecting appends and marks the relation unclean. A generated counting sort
runs where edges changed, and drops dead and disconnected edges along the way.
An apply along edges is combined in a fixed order (row, then edge), so pushing
values along edges gives the same result to the bit with parallel loops.

## The spiking network, and chasing C

`examples/snn.ent` is 1,024 leaky integrate-and-fire neurons. A neuron that
fires pushes its weights into its targets. `snn_pull.ent` instead gathers
along incoming edges, looking up whether the source fired. The host runs a
plain C simulation of the same network alongside, and both agree with it **to
the bit** after 100 steps. That's sequential, fused, in parallel stages and
with parallel entity loops, all compiled with `-ffp-contract=off` so that C
rounds like the generated code.

The first benchmark (M4 Max) wasn't kind:

- ent push took 1.55–1.73× the time of hand-written C push;
- **ent pull took 6.3–9.4×** C pull.

The results file listed three suspects, unmeasured. So we measured. Three
diagnostic C variants each added one behaviour of the generated loop to C's
pull: reading through the index, storing the input on every edge, and
checking every id. Together they landed within 2–5% of ent pull. The index
cost about 7× out of cache, and the per-edge store up to 2.5×. One suspect we
had been sure of, the reloaded archetype count, was wrong: LLVM hoists it.

Then the fixes, one gap at a time. Each step is a commit with a number:

| step | ent pull / C pull |
|---|---|
| start | 6.3–9.4× |
| a relation visited only by incoming edges is **stored sorted by target**; the visited neuron's input is **carried in a register** through the edge loop | 1.09–1.27× |
| `relation Synapse ... from Neuron to Neuron`: ends with a known component, and nothing removes it, so **no id checks** | 1.01–1.05× |

LLVM couldn't keep the sum in a register by itself. Every column is a view of
the same arena, so a store might change any load. The *language* rules that
out: inside an edge loop, only the entity's own `get` and `set` reach its
fields. Knowing that is the compiler's job, not LLVM's. The id checks were
"free" in the first diagnosis, because then the loop was waiting on scattered
loads. Once those were gone, the checks were the whole remaining gap: 14
instructions against 7.

Push had its own gap. It turned out to be the second pass: the deferred
apply's buffer and combine. But a loop that never runs in parallel visits
entities in exactly the combine order anyway. So when nothing else in the
query can see the field, the apply is now combined in place, and the buffers
stay only for parallel loops. Push went to 1.04–1.06× at 1e4 and 1e5.

Two more lowering fixes came out of reading the assembly:

- An accumulate's resource cell is carried through the loop instead of being
  stored on every combine. That had cost the integration loop 3.9× against C.
- When both branches of an `if` store to the same column, the stores are
  merged into one store after the `if`, so the `if` becomes a select. Clang
  does this for C, but our lowering hid it from LLVM.

And one example fix: firing had been recorded twice, as a value and as an
optional tag. The presence byte, written for every neuron every step, made
LLVM vectorise 2 wide instead of 4. Now it's a value only.

## Then we looked at the C

The like-for-like review was humbling, for the C side:

- C push computed each neuron's edge range as `i * K`, with `K` a compile-time
  constant. No program over a real relation can do that, and clang had
  unrolled the loop for it. With offsets read from an array, as every
  compressed-rows code needs, C push slowed down: the old variant took
  0.71–0.81× the time of the honest one at 1e6. Most of the push gap had been
  C cheating.
- C's parallel pull ran only the propagation in parallel, while ent's ran
  every loop. Now both do.

With like-for-like C, from 1e4 to 1e6 neurons:

- **ent push took 0.92–1.02×** the time of C push;
- **ent pull took 0.97–1.04×** C pull.

The integration loop alone still took 1.63× at 1e6 with the same loop shape
and vector width. Not explained, and the file says so.

## A second machine

The same day everything ran on Linux too: a Ryzen AI 9 HX 370 laptop with
conda-forge's LLVM 22.1.8. Two portability bugs showed up:

- `clock_gettime_nsec_np` exists only on macOS.
- glibc doesn't flush a redirected stdout on `abort()`, so the capacity test's
  error message never reached FileCheck.

The machine is faster in the caches and slower out of them. One thread moves
53–64 GB/s through DRAM against the Mac's 121–141. So entity parallelism gains
1.13× at 1e7 instead of 2.5×. That isn't a compiler problem, it's a memory bus.

The first Linux numbers spread by 33–60%. The cause was two kinds of cores: a
Zen 5c core takes 1.5× as long as a Zen 5 one. Pinned runs fixed the spread.
Then `-march=native` made the fused frame match C within 2% at every size. The
15% gap at 1e3 had been baseline SSE2, not us. Since then, benchmarks on this
machine are pinned and native, a rule that's still in force.

## Programs without a C host

The last commit of 3 October made a program with `main` an executable of its
own. Abridged from `examples/hello.ent`:

```
import console
import clock

system greet() {
  spawn { Print { line: "hello from frame {Time.frame}" } }
}

main {
  loop { frame() } until Time.frame == 5
}
```

It came with `world { ... }` for initial state, modules with `import`, inline
`text[N]` strings with interpolation, and `devices/console` and `devices/clock`.
`tools/ent` builds and runs a program together with the C of the modules it
imports.

The C host, which had owned the memory on day one and the `main` until now,
was down to devices. How much of the world those devices may see is the
subject of the next post.

## What these days settled

- **The syntax follows the semantics.** It's tested by giving the same bits as
  the IR it replaced.
- **Relations are tables, not archetypes**, sorted the way the program reads
  them.
- **Facts the language knows**, such as edge ends and no aliasing between
  columns, are worth more than any LLVM flag.
- **Check the C before celebrating or despairing.** The reference can be wrong
  too.
- **Pin, build native, and name the machine.**

Next: [04 · Devices: keeping the world away from C](04-devices.md)
