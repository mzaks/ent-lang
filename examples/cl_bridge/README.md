# cl_bridge: an ent-lang dish behind the CL SDK

A spiking network written in ent-lang plays the "dish" behind the simulator
of Cortical Labs' public CL SDK (pip package `cl-sdk`, 1.0.0). A Python
program written against the CL API (`cl.open`, `neurons.loop`,
`neurons.stim`, `tick.analysis.spikes`, `neurons.record`) then runs against
neurons that answer a stim and whose synapses learn, where the SDK's own
simulator replays a recording that does neither.

**This is a simplified LIF model and not a validated model of a biological
culture.** Its parameters were picked so that it fires at about a spike a
second a neuron and answers a stim; nothing in it was fitted to, or checked
against, recordings of real neurons or of a CL1. Its spikes say what this
model does, not what a culture would.

## What is here

| file | what it is |
|---|---|
| `network.ent` | the dish: a module with no `main` (schedules `create`, `step`, `checksum`) |
| `run.ent` | the dish on its own: rate a second, spike hash, checksum |
| `bridge.c` | a flat C interface over the dish; built with it into `libdish.so` |
| `ent_dish.py` | the CL SDK data source over `libdish.so` (ctypes), and `use()` |
| `demo_loop.py` | a CL API program: 1000 ticks/s, a stim every 100 ms, spikes counted, HDF5 recorded |
| `check/dish_check.c` | determinism and stim response through the C interface |
| `check/plasticity.c` | does pairing two channels change how one answers the other? |

Tests: `test/Integration/cl_bridge_dish.test` (run.ent: same seed twice and
on 8 threads gives the same spikes and checksum, seed 2 others),
`cl_bridge_abi.test` (dish_check.c through `libdish.so`, Linux), and
`cl_bridge_demo.test` (demo_loop.py on the SDK; runs only where lit finds a
Python that imports `cl`, in `build/cl_bridge/venv` or `CL_SDK_PYTHON`:
the SDK is no dependency of the rest of the suite).

## Build and run

```sh
# the dish alone: 60 s of seed 1
tools/ent run examples/cl_bridge/run.ent 60 1

# the shared library (needs ent build --shared, added for this)
tools/ent build --shared examples/cl_bridge/network.ent \
    examples/cl_bridge/bridge.c -o build/cl_bridge/libdish.so

# the C checks
cd build/cl_bridge
cc -O2 ../../examples/cl_bridge/check/dish_check.c -L. -ldish -Wl,-rpath,. -o dish_check
./dish_check [channel [amount]]
cc -O2 ../../examples/cl_bridge/check/plasticity.c -L. -ldish -Wl,-rpath,. -lm -o plasticity
./plasticity [minutes [seed [A B C]]]

# the CL SDK (tried with Python 3.14)
python -m venv build/cl_bridge/venv
build/cl_bridge/venv/bin/pip install cl-sdk
PYTHONPATH=examples/cl_bridge build/cl_bridge/venv/bin/python \
    examples/cl_bridge/demo_loop.py [seconds [seed [channel]]]
```

`ent_dish.use()` builds `build/cl_bridge/libdish.so` if it is not there
(or takes `library=` or `ENT_DISH_LIBRARY`). A program of its own:

```python
import cl, ent_dish
attributes = ent_dish.use(seed=7, stdp=True)   # before cl.open()
with cl.open() as neurons:
    recording = neurons.record(attributes=attributes, include_raw_samples=False)
    for tick in neurons.loop(1000, stop_after_seconds=10):
        ...
```

`demo_loop.py` sets `CL_SDK_ACCELERATED_TIME=1` unless the environment says
otherwise: 5 s of the dish take 1.7 s.

## The model

1024 leaky integrate-and-fire neurons at random places on a unit square, a
fifth of them inhibitory (the sign fixed per neuron). Each pair is wired
with the chance `density * exp(-distance / reach)`, none longer than
`reach_cut` reaches (44,815 synapses for seed 1). Each neuron's are found
among those near it through a grid index of where they are (`index Near
of Spot grid (x, y)`, a `near` loop), not among all: the draw of a pair is
by the numbers of both, so the synapses are those a loop over every pair
gives, and at the defaults the cut (1.6) is longer than the square's
diagonal, so nothing is cut. Each step is a millisecond:

- **input**: the pull sum of `weight * out` over a neuron's incoming
  synapses (`out` = fired times sign), a noise kick of `noise_amp` with the
  chance `noise_rate`, and its electrode's stim times `stim_gain * gain`;
- **membrane**: `v = exp(-1/tau) * v + input - adapt`; a spike at `v >= thr`,
  then reset to 0 and `refractory` steps without integrating;
- **adaptation**: each spike adds `adapt_inc` to `adapt`, which fades with
  `tau_adapt`. Without it a stim set some seeds alight (seed 3: about 95
  spikes on a channel in 25 ms after a stim two electrodes away, against 2
  on seed 1); with it none of seeds 1 to 10 does;
- **STDP** (trace, pair-based, pull form: each neuron changes only the
  synapses into it): `dw = a_plus * pre.trace * post.fired - a_minus *
  post.trace * pre.fired`, on excitatory synapses only, kept in
  `[0, w_max]`.

`fired` and `trace` are f32 values, not tags. The noise is SplitMix64's mix
of the seed, the step and the neuron's number (wyrand's 128-bit multiply
would keep the neuron loop from vectorising), so a dish replays the same on
one thread and on many.

The seed says which culture (places, wiring, noise); the parameters which
cell line. Defaults (`Dish` in `network.ent`, `dish_defaults()` in C):

| parameter | default | meaning |
|---|---|---|
| `seed` | 1 | everything random is drawn from it |
| `neurons` | 1024 | at most 65,536 (`dish_most_neurons`) |
| `inhibitory` | 0.2 | the share of inhibitory neurons |
| `tau` | 20 ms | membrane time constant |
| `thr` | 1.0 | threshold (rest and reset are 0) |
| `refractory` | 2 ms | |
| `adapt_inc` | 0.1 | what a spike adds to the adaptation |
| `tau_adapt` | 200 ms | the adaptation's time constant |
| `density` | 0.3 | the chance of a synapse at no distance |
| `reach` | 0.2 | the distance over which it falls by e (side = 1) |
| `reach_cut` | 8 | no synapse is longer than this many reaches |
| `w_exc` | 0.09 | excitatory weight at the start |
| `w_inh` | 0.35 | inhibitory weight (fixed) |
| `w_max` | 0.2 | excitatory weights are kept in [0, w_max] |
| `stdp` | true | whether excitatory weights learn |
| `a_plus`, `a_minus` | 0.005, 0.006 | STDP's potentiation and depression |
| `tau_plus`, `tau_minus` | 20 ms | the pre- and postsynaptic traces |
| `noise_rate` | 0.02 | the chance of a noise kick a step |
| `noise_amp` | 0.5 | its size |
| `stim_gain` | 2.0 | a stim of 1 on a neuron at its electrode |
| `stim_reach` | 0.05 | the distance over which a stim falls by e |

With these: 0.87 to 0.90 Hz a neuron over 20 s for seeds 1 to 8; 60 s of
seed 1 in 8.2 s on one core (`tools/ent run`, a portable build).

## Bigger dishes, threads and the GPU

A dish of more neurons is as dense a culture over a bigger square: the
square stays 1 wide and `reach` shrinks as the root of the number grows
(`0.2 * sqrt(1024 / neurons)`; `run.ent [seconds [seed [neurons]]]` does
so), so a neuron has as many synapses, about 70 away from the square's
sides. Keep the default reach with many neurons and every neuron has
thousands: the synapses outgrow the relation's capacity (8,388,608) and
the program stops, saying so.

Seed 1, native builds (`ENT_CFLAGS=-march=native`), on mains power;
setup is making the dish (places and wiring), a step 1 ms of it, from a
3 s run:

| neurons | synapses | setup: 1 core | 24 threads | step: 1 core | 24 threads | GPU |
|---|---|---|---|---|---|---|
| 1,024 | 44,815 | 0.03 s | 0.01 s | 92 us | 24 us | 67 us |
| 4,096 | 243,015 | 0.35 s | 0.04 s | 496 us | 78 us | 98 us |
| 16,384 | 1,103,714 | 2.3 s | 0.26 s | 2.4 ms | 331 us | 406 us |
| 65,536 | 4,726,319 | 11.6 s | 1.0 s | 12.2 ms | 1.8 ms | 2.0 ms |

Every column gives the same spikes and checksum. 24 threads is `tools/ent
build --parallel` with `ENT_PARALLEL_MIN=256` set when building (without
it a loop of fewer than a million rows stays on one thread). The GPU is a
laptop RTX 4050 (`--gpu`; the five systems of a step are `gpu system`s,
which a build without it runs on the CPU as before). Its step is its
kernels: going along the synapses (`stdp` 0.27 ns an edge, `gather`
0.14), about what 24 threads take; from 4,096 neurons up neither is far
ahead. Its clocks change during a run, so a GPU step's wall time varies
by up to 2x from run to run; the kernels' own times (`ENT_GPU_TIMES=1`)
do not. Wiring every pair, as at first, took 47 s for 65,536 neurons
(2.9 M synapses, `density` scaled down instead of `reach`) on one core.

Real time (a step in under a millisecond) is one core up to about 6,000
neurons, 24 threads or the GPU up to about 35,000 (from the cost an edge,
not measured at those sizes).

## Electrodes and channels

64 electrodes on an 8 x 8 grid over the same square: channel `c` is row
`c / 8`, column `c % 8`, at `((c % 8 + 0.5) / 8, (c / 8 + 0.5) / 8)`. Each
neuron belongs to the electrode of the grid cell it is in; its spikes are
that channel's, and that electrode's stim reaches it scaled by
`exp(-distance / stim_reach)`. About 16 neurons a channel.

## Through the CL SDK

- **Time**: one step of the dish is 1 ms, 25 frames at the SDK's 25 kHz. A
  spike of step `s` is given at frame `25 s + 12`, the middle of its
  millisecond, so it is never before a stim it answers.
- **Stims**: a stim reaches the dish at the first step that starts at or
  after its timestamp. Its size is its charge (the sum of each phase's
  |current| times its width) in units of 0.32 nC: the SDK's default stim
  (`neurons.stim(channel, 1.0)`: 1 uA, 160 us a phase, two phases) is 1.
  The phases' shape is not modelled, only the charge.
- **Spikes** have no waveform and **frames** are none: the SDK fills in
  zeros for both, so record with `include_raw_samples=False`.
- **Metadata**: 64 channels, not seekable, accelerated time supported.
- **Attributes**: `use()` gives `data_source` and every parameter as
  `dish_<name>`; pass them to `neurons.record(attributes=...)` and the HDF5
  file says which dish it is.

## The C interface

`bridge.c`, built into the same library as the dish. It differs from the
task's sketch in one way: every function takes the dish it is about
(`dish *`), so a process may hold several (the plasticity check runs a dish
with STDP and one without, side by side). Built for a GPU (`tools/ent
build --gpu --shared`), a process has one dish (the GPU keeps one world;
`dish_create` gives NULL for a second), which the CL SDK's source, a dish
in a subprocess of its own, is.

```c
void     dish_defaults(dish_params *p);
dish    *dish_create(const dish_params *p);         // NULL on bad parameters
void     dish_destroy(dish *d);
void     dish_stimulate(dish *d, int32_t channel, float amount);  // at the next step
int64_t  dish_step(dish *d, int32_t n);              // n ms; spikes, or -1
int64_t  dish_spikes(dish *d, int32_t *channel, int32_t *step, int64_t max);
void     dish_set_stdp(dish *d, int32_t on);
int32_t  dish_tick(dish *d);
int64_t  dish_spike_total(dish *d);
uint64_t dish_checksum(dish *d);                     // every potential and weight
```

A C host reaches an ent world's columns (`ent_world.h`) but not its
buffers, so a step's spikes are a count on each electrode
(`Electrode.count`), which `dish_step` turns into a list of
(channel, step). To build a shared library at all, `tools/ent` gained
`--shared` (everything compiled `-fPIC`, linked `-shared`, no `main`).

On a GPU build the world's columns are the GPU's between steps; the host
asks for the bytes it touches (`ent_gpu_range_to_host` before it reads or
writes them, `ent_gpu_range_wrote` after it wrote them, added to
devices/gpu.c for this): the 64 counts after each step, a stim's cell
around its write. 1,024 neurons through bridge.c: 64 us a step, where the
whole world back each step (`ent_gpu_world_to_host`) took 232 (one core:
103). `cl_bridge_gpu.test` holds the GPU library's spikes and checksum
against the CPU's, to the bit.

## What was measured

**Determinism.** The same seed gives the same spikes and checksum: run
again, on 8 threads (`--parallel`), and through the C interface. Another
seed gives others. (`cl_bridge_dish.test`, `cl_bridge_abi.test`.)

**Stim response.** Through C (`dish_check`, seed 1): 20 stims of 1 on
channel 27, every 100 ms: 63 spikes on it in the 20 ms after them, 9 in the
same windows of the same dish without them; the other channels 328 against
331. Through the CL SDK (`demo_loop.py 5`): channel 27 fires 0.138 spikes a
ms in the 20 ms after a stim and 0.005 in the rest.

**Plasticity** (`check/plasticity.c`, 3 minutes of the dish's time). Two
dishes of one seed. Probe (STDP off): a stim on A, B's spikes counted 5 to
30 ms after it, 100 times; the same for C, a channel never paired. Train:
A, then B 10 ms later, every 200 ms (900 pairings), STDP on in one dish and
off in the other. Probe again. The effect is B's change in the dish that
learned less its change in the one that did not. A = 27, B = 28, C = 36: A
and C both next to B.

| seed | B's answer to A before | effect, paired A | effect, unpaired C |
|---|---|---|---|
| 1 | 0.33 | +0.19 | +0.02 |
| 2 | 0.38 | +0.09 | −0.01 |
| 3 | 0.41 | +0.08 | −0.05 |
| 4 | 0.79 | +0.18 | +0.08 |
| 5 | 0.16 | +0.05 | −0.03 |
| 6 | 0.53 | +0.10 | −0.01 |
| 7 | 0.35 | +0.12 | −0.03 |
| 8 | 0.31 | +0.06 | +0.01 |
| 9 | 0.31 | +0.07 | −0.01 |
| 10 | 0.31 | +0.12 | +0.00 |

Spikes on B a probe. The paired effect is positive on all ten seeds, mean
+0.11 (sd 0.05 over seeds, so about 7 standard errors from nought); the
unpaired one is nought (mean −0.003). It is small: a tenth of a spike a
probe, where B's answer to A before training is mostly B's own background
(about 0.3 spikes in 25 ms), and within one seed it never reaches 2
standard errors (100 probes). So pairing does make B answer A a little more
in this model, and only A; it is not a large or a robust change of the
dish's response, and nothing here says a culture would do the same.

What came before, and is not the result: with A = 27, B = 29 (two
electrodes apart) and no adaptation, seeds disagreed in sign (seed 1
+0.64, seeds 2 and 4 −0.18 and −0.37), seed 3 and 5 ran away under
stimulation, and STDP's main effect was a dish-wide depression
(`a_minus > a_plus`). With adaptation and those channels the effect was
nought (seeds 1 to 5: −0.05 to +0.07, all under 1 se): A two electrodes
away does not reach B at all. The neighbouring channels were chosen after
that, which the reader should weigh.

## Not verified

- Anything about agreement with real CL1 recordings or cultures: not
  attempted (out of scope).
- Voltage waveforms and frames: none are produced.
- Stim pulse shape, polarity and timing within a millisecond: only the
  charge counts, at a millisecond's resolution.
- Seekable or non-accelerated reads were not exercised beyond what
  `demo_loop.py` does in accelerated time; a read that asks for a time
  before the last one gets no spikes from before it.
- macOS and Windows: `--shared` and the ABI test were run on Linux only.
- The GPU build behind the CL SDK (demo_loop.py): only through C
  (`cl_bridge_gpu.test`). More than 65,536 neurons (a pair's draw is keyed
  by `i * 65536 + j`).
- The plasticity result for other channel pairs, longer training, other
  pairing intervals or parameters.

## DishBrain's Pong, as it would plug in

Not built here. In DishBrain (Kagan et al., 2022) the ball's position is
given to a sensory region by which electrodes are stimulated and how often,
the paddle moves by comparing spike counts in two motor regions, and a miss
is followed by unpredictable stimulation, a hit by none (or predictable).
All of that is a CL API loop: `tick.analysis.spikes` read by channel into
the motor regions, `neurons.stim` on the sensory channels each tick, a
random burst on a miss. `ent_dish.use()` before `cl.open()` is all it would
take to run such a loop against this dish; whether this model would learn
the game is another question, and STDP as it is here is the only thing in
it that could.
