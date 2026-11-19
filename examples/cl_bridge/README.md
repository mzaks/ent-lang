# cl_bridge: an ent-lang dish behind the CL SDK

A spiking network written in ent-lang plays the "dish" behind the simulator
of Cortical Labs' public CL SDK (pip package `cl-sdk`, 1.0.0). A Python
program written against the CL API (`cl.open`, `neurons.loop`,
`neurons.stim`, `tick.analysis.spikes`, `neurons.record`) then runs against
neurons that answer a stim and whose synapses learn, where the SDK's own
simulator replays a recording that does neither.

**This is a simplified LIF model and not a validated model of a biological
culture.** Its defaults were fitted so that twelve summary statistics of its
activity (firing rate, bursts, the answer to a stim; [below](#calibration))
fall near those of public recordings of cortical cultures on
multi-electrode arrays (Wagenaar, Pine and Potter 2006). Ten of the twelve
land in the cultures' middle half. That is a fit, not a validation. The
model has 16 fitted parameters for 12 statistics; beyond them, only that
it holds on seeds it was not fitted on and over 45 minutes was checked;
and no single recording is reproduced. It was never compared with a CL1. Its spikes say
what this model does, not what a culture would.

## What is here

| file | what it is |
|---|---|
| `network.ent` | the dish: a module with no `main` (schedules `create`, `step`, `checksum`) |
| `run.ent` | the dish on its own: rate a second, spike hash, checksum |
| `bridge.c` | a flat C interface over the dish; built with it into `libdish.so` |
| `ent_dish.py` | the CL SDK data source over `libdish.so` (ctypes), and `use()`; `Dish` alone without the SDK |
| `demo_loop.py` | a CL API program: 1000 ticks/s, a stim every 100 ms, spikes counted, HDF5 recorded |
| `pong.py` | Pong in closed loop through the CL API, after DishBrain |
| `pong_study.py` | Pong's conditions and controls on 10 seeds |
| `conditioning.py` | can reward-modulated STDP teach one answer to one stim (the dish alone)? |
| `learn_study.py` | Pong with a reward condition, by gain, as is and flipped |
| `reach.py` | how much of the motor regions' answer to a stim comes along the axons |
| `paths_probe.py` | conditioning's weights by path, minute by minute |
| `feedback_study.py` | DishBrain's feedback (`stim`) against its controls on the `columns` layout, plain STDP |
| `pacman/pacman_dish.ent` | `examples/pacman.ent` played by the dish, in a window, with its electrodes drawn beside the maze |
| `pacman/dishpilot.ent`, `.c` | the pilot: a device that loads `libdish.so` at run time and turns the game into stims and spikes into a way |
| `check/dish_check.c` | determinism and stim response through the C interface |
| `check/plasticity.c` | does pairing two channels change how one answers the other? |
| `culture/fetch.sh` | downloads the cultures' recordings (29 spontaneous, 21 with stims) |
| `culture/stats.py` | spontaneous statistics (rate, Wagenaar's bursts), of a culture or the dish |
| `culture/evoked.py` | the statistics of the answers to stims; the archive's stim protocol on a dish |
| `culture/dish_stats.py` | `stats.py` on the dish, a few seeds |
| `culture/calibrate.py` | the random search that gave the defaults |
| `culture/validate.py` | the dish's statistics against the cultures', seed by seed (the tables below) |

Tests: `test/Integration/cl_bridge_dish.test` (run.ent: same seed twice and
on 8 threads gives the same spikes and checksum, seed 2 others),
`cl_bridge_abi.test` (dish_check.c through `libdish.so`, Linux),
`cl_bridge_gpu.test` (the GPU library against the CPU's),
`cl_bridge_pong.test` (pong.py replays the same),
`cl_bridge_pacman.test` (the Pac-Man pilot compiles) and
`cl_bridge_demo.test` (demo_loop.py on the SDK; these two run only where lit finds a
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
./plasticity [minutes [seed [A [B C]]]]

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
otherwise.

## The model

1024 leaky integrate-and-fire neurons at random places on a unit square, a
fifth of them inhibitory (the sign fixed per neuron). Each pair is wired
with the chance `density * exp(-distance / reach)`, none longer than
`reach_cut` reaches (44,815 synapses for seed 1). Each neuron's are found
among those near it through a grid index of where they are (`index Near
of Spot grid (x, y)`, a `near` loop), not among all: the draw of a pair is
by the numbers of both, so the synapses are those a loop over every pair
gives, and at the defaults the cut (1.6) is longer than the square's
diagonal, so nothing is cut. Each neuron has an axon: a straight line of
length `axon` from it, in a direction drawn from the seed. Each step is a
millisecond:

- **synaptic input**: `sum`, the pull sum of `weight * out` over a
  neuron's incoming synapses. It acts twice. `syn = exp(-1/tau_syn) * syn +
  sum` is a slow current that holds what came in for about `tau_syn`
  steps. `fast * sum` acts in this step alone, as fast synapses beside the
  slow ones;
- **short-term depression**: what a spike carries (`out = sign * ready`)
  is the neuron's `ready` resources (0 to 1). A spike uses `depress` of
  them, and they recover with `tau_recover`, so a burst wears itself out;
- **stims**: an electrode's stim reaches the neurons whose axons pass near
  it, the four electrodes nearest each axon. Each gets `stim_gain *
  exp(-d / stim_reach)` times the stim, where `d` is the distance from the
  axon. It arrives at the soma `l / conduct` steps later, where `l` is how
  far along the axon that point is. One stim at a time is on its way; a
  second before the first arrives joins it, at the second's time;
- **noise**: a kick of `noise_amp` with the chance `noise_rate` a step;
- **membrane**: `v = exp(-1/tau) * v + syn + fast * sum + kick + stim -
  adapt`; a spike at `v >= thr`, then reset to 0 and `refractory` steps
  without integrating;
- **adaptation**: each spike adds `adapt_inc` to `adapt`, which fades with
  `tau_adapt`;
- **STDP** (trace, pair-based, soft bounds as van Rossum, Bi and
  Turrigiano 2000; pull form, so each neuron changes only the synapses
  into it), on excitatory synapses only: `dw = a_plus * pre.trace *
  post.fired * (w_max - w) - a_minus * post.trace * pre.fired * w`. The
  weights settle where the two balance, not at 0 or `w_max`, where
  additive STDP took them in a dish that bursts;
- **synaptic scaling** (Turrigiano), with STDP: each excitatory synapse
  into a neuron also changes by `home_gain * (home_rate - rate) * w`,
  where `rate` is the neuron's rate averaged over `tau_home`. With STDP
  alone, even soft-bounded, the calibrated dish went alight or quiet
  within minutes;
- **reward-modulated STDP** (Izhikevich 2007, Florian 2007), with
  `modulated` (off by default): the STDP term goes into the synapse's
  eligibility, `elig = exp(-1/tau_elig) * elig + dw_stdp`, and the weight
  changes by `reward_gain * dopamine * elig` (scaling as before). A stim
  on `reward_channel` adds its size to `dopamine`, one on
  `punish_channel` takes it away, and neither is a current into the dish;
  `dopamine` fades with `tau_reward`. No CL1 has such a channel: it is
  there to ask whether this model could learn a task at all were it told
  how it did ([Learning rules](#learning-rules)).
- **hard bounds**, with `hard` (off by default): STDP's steps no longer
  depend on the weight, `(w_max - w)` and `w` both become `w_max / 2`
  (the soft rule's steps at `w_max / 2`), and the weight is still kept in
  `[0, w_max]`.

`fired` and `trace` are f32 values, not tags. The noise is SplitMix64's mix
of the seed, the step and the neuron's number (wyrand's 128-bit multiply
would keep the neuron loop from vectorising), so a dish replays the same on
one thread and on many.

The seed says which culture (places, axons, wiring, noise); the parameters
which cell line. Defaults (`Dish` in `network.ent`, `dish_defaults()` in
C); "fitted" are those the [calibration](#calibration) chose, the rest were
set by hand:

| parameter | default | | meaning |
|---|---|---|---|
| `seed` | 1 | | everything random is drawn from it |
| `neurons` | 1024 | | at most 65,536 (`dish_most_neurons`) |
| `inhibitory` | 0.2 | | the share of inhibitory neurons |
| `tau` | 20 ms | | membrane time constant |
| `thr` | 1.0 | | threshold (rest and reset are 0) |
| `refractory` | 2 ms | | |
| `adapt_inc` | 0.0682 | fitted | what a spike adds to the adaptation |
| `tau_adapt` | 66.8 ms | fitted | the adaptation's time constant |
| `density` | 0.3 | | the chance of a synapse at no distance |
| `reach` | 0.2 | | the distance over which it falls by e (side = 1) |
| `reach_cut` | 8 | | no synapse is longer than this many reaches |
| `w_exc` | 0.00194 | 0.32 `w_max` | excitatory weight at the start |
| `w_inh` | 0.0295 | fitted | inhibitory weight (fixed) |
| `w_max` | 0.00605 | fitted | excitatory weights are kept in [0, w_max] |
| `tau_syn` | 270 ms | fitted | the slow synaptic current's time constant |
| `fast` | 47.8 | fitted | the fast part, times what came in this step |
| `depress` | 0.231 | fitted | the share of its resources a spike uses |
| `tau_recover` | 1070 ms | fitted | their recovery |
| `stdp` | true | | whether excitatory weights learn |
| `a_plus`, `a_minus` | 0.002, 0.002 | | STDP's potentiation and depression |
| `tau_plus`, `tau_minus` | 20 ms | | the pre- and postsynaptic traces |
| `home_rate` | 1.33 Hz | fitted | the rate scaling holds a neuron to |
| `tau_home` | 60 s | | what that rate is averaged over |
| `home_gain` | 0.0859 | fitted | how fast scaling works |
| `noise_rate` | 0.00532 | fitted | the chance of a noise kick a step |
| `noise_amp` | 0.51 | fitted | its size |
| `stim_gain` | 8.16 | fitted | a stim of 1 on a neuron whose axon passes through the electrode |
| `stim_reach` | 0.0646 | fitted | the distance (electrode to axon) over which it falls by e |
| `axon` | 0.274 | fitted | the axon's length |
| `conduct` | 0.0552 | fitted | how far a spike runs along an axon in a step (55 um/ms if the square is 1 mm) |
| `hear_reach` | 0.03 | | an electrode hears the neurons this near it |
| `modulated` | false | | reward-modulated STDP instead of STDP |
| `tau_elig` | 1000 ms | | the eligibility's time constant |
| `tau_reward` | 200 ms | | the dopamine's |
| `reward_gain` | 1 | | how much a unit of dopamine times the eligibility changes a weight |
| `reward_channel`, `punish_channel` | -1, -1 | | the channels whose stims are dopamine (-1: none) |
| `hard` | false | | hard-bounded (additive) STDP instead of soft bounds |

With these, a dish is quiet between bursts. Seed 1's first 3 s have 184
spikes (0.06 Hz a neuron); over minutes, the electrodes see about 4 bursts
a minute (below).

## Electrodes and channels

64 electrodes on an 8 x 8 grid over the same square: channel `c` is row
`c / 8`, column `c % 8`, at `((c % 8 + 0.5) / 8, (c / 8 + 0.5) / 8)`. Each
neuron belongs to the electrode of the grid cell it is in (about 16 a
channel). The electrode hears only those within `hear_reach` of it: about
3 neurons, and none on a few channels (seed 1: 61 channels fire at 0.1 Hz
or more),
as a culture's electrode records a few cells near it. A stim reaches
neurons by their axons (above), so its first answers are on whichever
channels those neurons are, often far from the stimulated electrode.

## Calibration

**Data.** Wagenaar, Pine and Potter's recordings of dense cultures of rat
cortex on 60-electrode arrays (Wagenaar DA, Pine J, Potter SM (2006). An
extremely rich repertoire of bursting patterns during the development of
cortical cultures. *BMC Neuroscience* 7:11,
https://doi.org/10.1186/1471-2202-7-11; data at
https://potterlab.bme.gatech.edu/development-data/, whose terms ask that
work using it cite the article). Of each of 29 cultures, the spontaneous
recording (half an hour) of the day nearest 28 days in vitro (25 to 31);
of 21 of them, the stimulus-evoked recording of the same day, a stim on
one electrode every 0.3 s, round the electrodes in turn. `culture/fetch.sh`
downloads them into `build/culture` (130 MB). The data are not in the
repository.

**Statistics.** Each the same for a culture and the dish (`stats.py`,
`evoked.py`). Spontaneous, of the channels firing at 0.1 Hz or more:
- the median channel's rate;
- its ISI CV;
- network bursts as Wagenaar et al. find them (on 4 or more channels): how
  many a minute, their median length, the share of spikes in them, the
  median share of active channels in one;
- the CV of the intervals between bursts.

Evoked, on the channels other than the stimulated one:
- spikes 3 to 20 ms after a stim (`early`; the first 3 ms are lost to the
  stim's artefact on an array);
- spikes 20 to 300 ms after (`late`);
- channels with a spike in 3 to 100 ms (`channels`);
- the share of stims answered by a burst (`big`);
- the median latency.

The cultures' quartiles of each are the target (`STATS_JSON=...
stats.py`, `EVOKED_JSON=... evoked.py` over the recordings).

**Search** (`calibrate.py`). Sixteen parameters, drawn log-uniformly
within ranges: `w_max`, `w_inh`, `depress`, `tau_recover`, `noise_rate`,
`noise_amp`, `adapt_inc`, `tau_adapt`, `tau_syn`, `home_rate`,
`home_gain`, `axon`, `conduct`, `stim_gain`, `stim_reach` and `fast`.
`w_exc` is 0.32 `w_max`: higher, and STDP sets the dish alight in its first
second. Each candidate is a dish of seed 1 with STDP and scaling on. It
runs 10 minutes to settle, then 5 minutes of which the spontaneous
statistics are taken, then the archive's stim protocol, 5 rounds of the 64
channels. Its score is the sum over the 12 statistics of `(log(dish /
culture median) / log(q75 / q25))^2`. A statistic one interquartile spread
(in logs) off the median adds 1. Rounds of 66 candidates, each round
around the five best so far, in 22 processes. It ran twice: 6 rounds
around an earlier fit of the spontaneous statistics alone (no axons, no
fast synapses), then 5 rounds around that search's best, 726 dishes in
all.

The best of seed 1 (2.14) fired at 5.3 Hz with 5.9 bursts a minute on
seeds 1 to 10, both above the cultures' 75th percentile. The defaults are
the second best (2.25), whose spontaneous medians over the ten seeds
fall in the cultures' middle half (the share of spikes in bursts at its
edge), rounded to 3 digits.

**Result** (`validate.py ... 10`). The defaults on seeds 1 to 10 (seed 1
the only one the search saw): 10 minutes to settle, 10 minutes of
spontaneous activity, then 10 rounds of the stim protocol:

| statistic | dish, median of seeds | seeds' range | cultures 25% | median | 75% | |
|---|---|---|---|---|---|---|
| rate (Hz, median active channel) | 3.86 | 3.65–3.98 | 2.27 | 3.32 | 4.75 | in |
| ISI CV | 3.95 | 3.83–4.12 | 1.36 | 2.26 | 4.63 | in |
| bursts a minute | 4.05 | 3.5–4.3 | 1.71 | 2.79 | 4.08 | in |
| burst length (ms) | 1590 | 1480–2000 | 434 | 1040 | 1700 | in |
| share of spikes in bursts | 0.895 | 0.888–0.911 | 0.298 | 0.759 | 0.898 | in |
| share of channels in a burst | 0.883 | 0.767–0.934 | 0.451 | 0.784 | 0.982 | in |
| IBI CV | 0.700 | 0.615–0.768 | 0.571 | 0.784 | 0.979 | in |
| early (spikes, 3–20 ms) | 25.5 | 23.4–26.8 | 17.6 | 62.4 | 77.2 | in |
| late (spikes, 20–300 ms) | 37.1 | 33.7–43.6 | 53.3 | 76.9 | 138 | low |
| channels (in 100 ms) | 16 | 14–18 | 21 | 30 | 47 | low |
| big (share of stims) | 0.137 | 0.105–0.156 | 0.119 | 0.278 | 0.598 | in |
| latency (ms) | 57 | 40–73 | 22.2 | 62.8 | 120 | in |

Scores 2.16 to 2.94 a seed (the median dish 2.38).

`early` and `latency` put the dish's first answer to a stim in the
cultures' range, if in its lower half for `early`. The number of bursts
a minute is near the cultures' 75th percentile. `late` and `channels`
fall short: the answer reaches fewer channels than a culture's and dies
sooner. The share of spikes in bursts is at the cultures' upper edge:
almost every spike of the dish is in a burst, as in the burstiest
cultures. The seeds differ much less than the cultures do. The dish's
seeds span rate 3.65 to 3.98 Hz, where the cultures' middle half alone
spans 2.3 to 4.8. A seed changes where the neurons are
and their wiring, not the cell line; the cultures differ by more than
that.

**It keeps so** (`validate.py ... --long 10`, no stims). The medians over
seeds 1 to 10 of minutes 0 to 10, 15 to 25 and 35 to 45:

| minutes | rate | ISI CV | bursts a minute | burst ms | in bursts | channels in a burst | IBI CV | score |
|---|---|---|---|---|---|---|---|---|
| 0–10 | 3.86 | 4.08 | 3.95 | 1660 | 0.901 | 0.851 | 0.825 | 0.60 |
| 15–25 | 3.83 | 3.95 | 4.05 | 1640 | 0.895 | 0.876 | 0.706 | 0.62 |
| 35–45 | 3.90 | 3.94 | 3.95 | 1670 | 0.900 | 0.877 | 0.706 | 0.62 |

(The score of the 7 spontaneous statistics alone.) Every median is in
the cultures' middle half but the share of spikes in bursts, which is on
its upper edge (0.895 to 0.901 against 0.898).

No seed goes silent or alight, and none bursts like a clock: the IBI CV
settles from 0.83 to 0.71 but stays a culture's.

What a fit of summary statistics cannot say: whether the bursts have a
culture's shape inside, whether a channel's answer is a culture's channel's
answer, how the dish develops (the cultures were fitted at one age), and
whether its plasticity is anything like a culture's (none of the
statistics measures learning).

## Bigger dishes, threads and the GPU

A dish of more neurons is as dense a culture over a bigger square: the
square stays 1 wide and `reach` shrinks as the root of the number grows
(`0.2 * sqrt(1024 / neurons)`; `run.ent [seconds [seed [neurons]]]` does
so), so a neuron has as many synapses, about 70 away from the square's
sides. Keep the default reach with many neurons and every neuron has
thousands: the synapses outgrow the relation's capacity (8,388,608) and
the program stops, saying so. The other lengths (`axon`, `stim_reach`,
`hear_reach`) are not scaled, so a bigger dish has more neurons an
electrode hears and a stim reaches; it was not calibrated.

Seed 1, native builds (`ENT_CFLAGS=-march=native`), on mains power; a
step is 1 ms of the dish, from the difference of an 11 s and a 1 s run:

| neurons | synapses | 1 core | 24 threads | GPU |
|---|---|---|---|---|
| 1,024 | 44,815 | 182 us | 30 us | 56 us |
| 16,384 | 1,103,714 | 4.9 ms | 519 us | 303 us |
| 65,536 | 4,726,319 | 20.0 ms | 2.6 ms | 2.0 ms |

Every column gives the same spikes and checksum. 24 threads is `tools/ent
build --parallel` with `ENT_PARALLEL_MIN=256` set when building (without
it a loop of fewer than a million rows stays on one thread). The GPU is a
laptop RTX 4050 (`--gpu`; the five systems of a step are `gpu system`s,
which a build without it runs on the CPU as before). Its clocks change
during a run, so a GPU step's wall time varies by up to 2x from run to
run; the kernels' own times (`ENT_GPU_TIMES=1`) do not. A step costs
about twice what it did before the calibration (1,024 neurons on one
core: 92 us; 65,536: 12.2 ms), which was not broken down: the step
has more per neuron now (two currents, depression, the axon's stims,
scaling) and more per synapse (soft bounds and scaling). Making the dish was not measured again; before the calibration it
was 11.6 s for 65,536 neurons on one core and 1.0 s on 24 threads (the
axons add a pass over the 64 electrodes a neuron). Wiring every pair, as
at first, took 47 s for 65,536 neurons (2.9 M synapses, `density` scaled
down instead of `reach`) on one core.

Through the CL SDK (`demo_loop.py`'s loop, 10 s of the dish in
accelerated time, `ent_dish.use(neurons=.., reach=..)`), measured before
the calibration, the same recording from every library:

| neurons | 1 core | 24 threads | GPU |
|---|---|---|---|
| 1,024 | 2.5 s | 1.8 s | 2.4 s |
| 16,384 | 34 s | 9.0 s | 9.0 s |

At 16,384 neurons the SDK and Python (168,181 spikes handed over one by
one) took about as long as the dish did on 24 threads or the GPU.
Build the library for threads with `ENT_PARALLEL_MIN=256 tools/ent build
--parallel --shared ..`. A library leaves its host's threads unbound: a
program built with `--parallel` binds its threads, the first to the CPU it
starts on, and put that in the environment; as a library in Python, that
pinned Python's own thread to one CPU, and the SDK's simulator, a process
it starts, got that one CPU and the settings: 16,384 neurons took 39 s on
24 threads. `cl_bridge_abi.test` holds a library's host to what it was.

Real time (a step in under a millisecond) is one core up to about 4,000
neurons, 24 threads or the GPU up to about 30,000 (from the sizes
measured, not at those sizes).

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

`dish_params` lists every parameter of the table above, in the order of
`DishParams` in `ent_dish.py`; a host built against an older one reads the
wrong fields.

A C host reaches an ent world's columns (`ent_world.h`) but not its
buffers, so a step's spikes are a count on each electrode
(`Electrode.count`), which `dish_step` turns into a list of
(channel, step). To build a shared library at all, `tools/ent` gained
`--shared` (everything compiled `-fPIC`, linked `-shared`, no `main`).

On a GPU build the world's columns are the GPU's between steps; the host
asks for the bytes it touches (`ent_gpu_range_to_host` before it reads or
writes them, `ent_gpu_range_wrote` after it wrote them, added to
devices/gpu.c for this): the 64 counts after each step, a stim's cell
around its write. `cl_bridge_gpu.test` holds the GPU library's spikes and
checksum against the CPU's, to the bit.

## What was measured

**Determinism.** The same seed gives the same spikes and checksum: run
again, on 8 threads (`--parallel`), on the GPU, and through the C
interface. Another seed gives others. (`cl_bridge_dish.test`,
`cl_bridge_abi.test`, `cl_bridge_gpu.test`.)

**Stim response.** Through C (`dish_check`, seed 1): 20 stims of 1 on
channel 27, every 100 ms: 60 spikes on it in the 20 ms after them, none in
the same windows of the same dish without them; the other channels 154
against 4. Through the CL SDK (`demo_loop.py 5`): channel 27 fires 0.150
spikes a ms in the 20 ms after a stim and none in the rest; 659 spikes in
all, most on channels 27, 44, 20, 34 and 35. The calibration's `early`, `late` and `channels` say the rest.

**Plasticity** (`check/plasticity.c`, 3 minutes of the dish's time). Two
dishes of one seed. Probe (STDP off): a stim on A, B's spikes counted 5 to
30 ms after it, 100 times; the same for C, a channel never paired with A.
Train: A, then B 10 ms later, every 200 ms (900 pairings), STDP on in one
dish and off in the other. Probe again. The effect is B's change in the
dish that learned less its change in the one that did not. A = 27; B and C
are chosen before any training, on a dish of the seed with STDP off. B is
the channel that answers A most in 5 to 30 ms. C is the channel other than
A whose stims B answers most, so B's answers to both start from something.

| seed | B, C | B's answer to A before | effect, paired A | effect, unpaired C |
|---|---|---|---|---|
| 1 | 41, 56 | 0.05 | −0.03 | −0.05 |
| 2 | 53, 60 | 0.02 | −0.03 | −0.07 |
| 3 | 19, 11 | 0.39 | −0.31 | −0.09 |
| 4 | 13, 16 | 0.05 | −0.01 | +0.00 |
| 5 | 21, 28 | 0.09 | −0.06 | −0.11 |
| 6 | 41, 24 | 0.03 | −0.04 | −0.24 |
| 7 | 20, 10 | 0.22 | −0.09 | −0.15 |
| 8 | 36, 49 | 0.12 | −0.11 | −0.01 |
| 9 | 19, 4 | 0.20 | −0.17 | −0.18 |
| 10 | 32, 5 | 0.02 | −0.04 | −0.04 |

Spikes on B a probe. Pairing does not make B answer A more. With STDP on,
B's answers fall to about nothing after training, to the paired channel and
the unpaired one alike (means −0.09 and −0.09). Most likely the 3 minutes of two stims every
200 ms raise the rates and scaling takes the weights down everywhere, but
that was not separated from STDP's own depression. Either way, there is no
pair-specific learning in this model at the defaults. That is no evidence about cultures one way or the
other. Wagenaar, Pine and Potter did not find a stimulation protocol
that reliably changed cultures' answers either ("Searching for plasticity
in dissociated cortical cultures on multi-electrode arrays", *Journal of
Negative Results in BioMedicine* 5:16, 2006).

Before the calibration, with hand-set parameters (electrodes hearing their
whole cell, no axons, additive STDP), A = 27, B = 28, C = 36 gave a paired
effect of +0.11 spikes a probe on all ten seeds and an unpaired one of
nought. Those channels were chosen after a first pair showed nothing, and
the dish answered a stim only on its own channel. The calibrated dish
answers far away, so neighbours mean nothing to it any more.

## Not verified

- Agreement with real CL1 recordings: never compared. Agreement with
  cultures: only the twelve summary statistics above, as a fit, at one age.
- Voltage waveforms and frames: none are produced.
- Stim pulse shape, polarity and timing within a millisecond: only the
  charge counts, at a millisecond's resolution.
- Seekable or non-accelerated reads were not exercised beyond what
  `demo_loop.py` does in accelerated time; a read that asks for a time
  before the last one gets no spikes from before it.
- macOS and Windows: `--shared` and the ABI test were run on Linux only.
- The GPU build behind the CL SDK: run by hand (the same HDF5 recording as
  the CPU's, spike by spike), not in a test (the test of the SDK builds the
  CPU library). More than 65,536 neurons (a pair's draw is keyed by
  `i * 65536 + j`).
- Bigger dishes against cultures (the calibration is of 1,024 neurons), and
  plasticity for other protocols, longer training or other parameters.
- Pong with other layouts, feedback, game speeds or longer sessions, or
  with learning rules other than STDP and scaling.

## Pong in closed loop

`pong.py` plays Pong with the dish through the CL API, after DishBrain
(Kagan et al. 2022, *Neuron* 110:3952): `tick.analysis.spikes` in,
`neurons.stim` out, 200 ticks a second, in accelerated time. The layout
and the game are this example's, not DishBrain's:

```
rows 0-2   motor "up"     channels 0-23
rows 3-4   sensory        26 27 28 29 / 34 35 36 37 (columns 2-5)
rows 5-7   motor "down"   channels 40-63
```

- **Ball to dish**: the sensory electrode stimulated says the ball's
  height (eighths of the court, in the order 26 27 28 29 37 36 35 34,
  top to bottom); how often says how near it is, from 4 Hz at the far
  wall to 40 Hz at the paddle. Each stim is a single pulse of the SDK's
  default 1 uA, scheduled by the game.
- **Dish to paddle**: the spikes on each motor region, averaged over
  100 ms; the paddle (0.3 of the court high) moves at one court a second
  towards the busier region, not at all within 10% of a tie.
- **The ball** crosses the court in 2 s. A serve, and a ball off the
  paddle, leave at a random angle up to 45 degrees. (With the ball only
  reflected, a paddle parked at a wall could return some paths forever:
  one 158-hit rally in a first study, now gone.)
- **Feedback**, by condition:
  - `stim`: a hit gets all 8 sensory electrodes at 100 Hz for 100 ms
    (predictable); a miss gets 4 s of stims on random electrodes at 5 Hz
    (unpredictable), then a new serve;
  - `silent`: none, but 4 s without sensory input after a miss;
  - `none`: none, and no pause;
  - `rest`: no sensory input at all, so the paddle moves with the dish's
    own activity.

`pong_study.py` plays 20 minutes of each condition on seeds 1 to 10, plus
three controls:
- `stim/off`: `stim` with STDP off (and so no scaling), so the dish can
  only do what its wiring does at the start;
- `stim/off/flip`: the same with the sensory map upside down;
- `still`: the game alone, its paddle in the middle all along.

The means over seeds (± standard error), DishBrain's measures: hits a
rally in the first 5 minutes and in the 15 after, and of the later
rallies the share that were aces (no hit) and long (3 hits or more):

| session | rallies | hits, first 5 min | hits, after | aces, after | long, after | paddle at a wall |
|---|---|---|---|---|---|---|
| stim | 107 | 1.49 ± 0.16 | 1.29 ± 0.11 | 0.45 | 0.18 | 65% |
| silent | 111 | 1.30 ± 0.12 | 1.20 ± 0.10 | 0.46 | 0.17 | 67% |
| none | 181 | 1.29 ± 0.09 | 1.19 ± 0.10 | 0.46 | 0.15 | 70% |
| rest | 324 | 0.43 ± 0.02 | 0.43 ± 0.01 | 0.70 | 0.03 | 37% |
| stim/off | 110 | 1.29 ± 0.19 | 1.28 ± 0.11 | 0.45 | 0.16 | 65% |
| stim/off/flip | 187 | 0.10 ± 0.03 | 0.11 ± 0.03 | 0.91 | 0.00 | 71% |
| still | 333 | 0.44 ± 0.02 | 0.39 ± 0.01 | 0.72 | 0.02 | — |

The differences, seed by seed (the same dish in each), in hits a rally
after the first 5 minutes:

| | mean ± se | |
|---|---|---|
| stim less rest | +0.87 ± 0.11 | 7.8 se |
| stim/off less rest | +0.86 ± 0.11 | 7.6 se |
| stim/off less stim/off/flip | +1.17 ± 0.14 | 8.5 se |
| stim less stim/off | +0.01 ± 0.08 | 0.1 se |
| stim: after less first 5 minutes | −0.20 ± 0.14 | −1.4 se |
| stim/off: after less first 5 minutes | −0.01 ± 0.18 | 0.0 se |

So the dish plays about three times as well as chance (a still paddle,
or its own activity at rest), from the first minutes, and that is its
wiring, not learning:
- with STDP off it plays as well;
- it does not get better over 20 minutes;
- feedback, silence and no feedback make no difference.

That the dish follows the ball, and not just any stim moving the
paddle, the flipped map shows: upside down, the dish plays a quarter as
well as chance. And it is not the synapses: a stim excites the neurons
whose axons pass the electrode, and those reach about two electrodes from
it, so the sensory electrodes of row 3 fire neurons the up region hears
and those of row 4 the down region's, directly. With every synapse at 0
(`w_exc=0 w_inh=0`), the spikes the up and down regions answer a sensory
stim with in 30 ms are the same as with them (for example, channel 26:
4.6 up and 2.1 down with the synapses, 4.6 and 2.1 without; seeds 1 to
4). No weight can change that, which is why the `columns` layout below
is there.

DishBrain's cultures, by its report, improved over a session under
`stim` feedback and not without it. This dish does not, which fits what
the pairing check found: its STDP and scaling change the dish as a whole,
not one pathway. Nothing here says anything about cultures; it says this
model, at these defaults, has nothing that learns the game.

```sh
PYTHONPATH=examples/cl_bridge build/cl_bridge/venv/bin/python \
    examples/cl_bridge/pong.py [minutes [seed [condition [flip] [columns] [name=value ...]]]]
PYTHONPATH=examples/cl_bridge build/cl_bridge/venv/bin/python \
    examples/cl_bridge/pong_study.py [minutes [seeds [out.json]]]
```

One session's line of JSON has its rallies (start, hits) and the summary;
the study of 60 sessions took about 45 minutes on 22 processes (a session
is the game's loop and the SDK's data producer, a core each).
`cl_bridge_pong.test` plays half a minute of `stim` twice and checks it
replays the same rallies.

## Pac-Man

`pacman/pacman_dish.ent` is `examples/pacman.ent` with the dish as its
pilot, in real time: a game an ent world, the dish another, loaded by a
device (`pacman/dishpilot.c`, with `dlopen`, its symbols kept to itself,
`RTLD_LOCAL | RTLD_DEEPBIND`). Each frame:

- the game tells the device where each pellet and each ghost is from
  Pac-Man (`sense`), and the device keeps the nearest pellet and the
  nearest ghost that is not frightened;
- the device runs the dish for the frame's milliseconds, stimulating the
  sensory electrode of the nearest pellet's way (four in the middle of
  the grid: right, down, left, up) and, within 10 tiles, that of the
  nearest ghost's way (four more), each at 4 Hz far to 40 Hz near, as
  DishBrain's ball;
- the way is that of the motor region (the grid's four edges, 8
  electrodes each, the corners left out) whose spikes, each averaged
  over 100 ms, lead the next by 10%; held arrow keys win over it.

The panel beside the maze is the 64 electrodes lit by their spikes (blue
motor, green food, red ghost). The dish has short axons (`DISH_AXON`,
0.11) so the stims do not drive the motor regions directly, and plain
STDP: no reward, nothing that would make it learn the game.

```sh
tools/ent build --shared examples/cl_bridge/network.ent \
    examples/cl_bridge/bridge.c -o build/cl_bridge/libdish.so
tools/ent build examples/cl_bridge/pacman/pacman_dish.ent -o build/pacman_dish
build/pacman_dish        # from the repository's root; DISH_SEED, DISH_STIM=0
```

One try, seed 1, 15 minutes of each, played unattended (each game over
starts the next): told where the food and the ghosts are, 21 games, a
mean score of 950 ± 75 (480 to 1830); with `DISH_STIM=0`, the dish's own
activity alone, 18 games, 693 ± 52 (350 to 1100). So what it is told
moves Pac-Man somewhat towards what it is told of, at
about 2.8 standard errors on one seed's games, which follow each other
in one dish and are not independent; no level was cleared. Not a
study: no seeds, no pairing, real time.

## Learning rules

Nothing in the default dish learns the game, so `modulated` swaps STDP
for reward-modulated STDP: the same pairs, but kept in each synapse's
eligibility (1 s), and turned into weight by dopamine (200 ms), which the
host gives with stims on a reward and a punish channel. A real CL1 has no
such channel, so this asks about the model, not about cultures: could
this dish learn the game at all, were it told how it did?

The rows layout cannot show it: there, the stims' direct answers along
the axons play the game (above). pong.py's `columns` layout spreads them
so that they do not:

```
columns 0-1   motor "up"       columns 6-7   motor "down"
columns 3-4   sensory, a row each: 3 12 20 27 35 44 52 59 (top to bottom)
              (the column alternates, so each half of the court has two
              electrodes that lean up and two that lean down)
28, 36        reward, punish
```

There a still paddle hits 0.39 a rally and the dish with nothing learnt
(gain 0) about 0.45.

**One stim, one answer, first.** `conditioning.py` asks the simplest
thing on that layout, on the dish alone: after a minute to settle, a stim
on a random sensory electrode every 0.5 s, the up and down regions'
spikes in the 100 ms after it, then a reward or a punish pulse. `map` wants
up for the top half's electrodes and down for the bottom's, as Pong
does; its score is the top half's share of right answers plus the bottom
half's less 1, so a bias towards one region scores 0. Plain reward (`map`,
a pulse of 1 either way) mostly taught a bias: at gain 100 on seeds 1 to
4, 0.65 right for the top half and 0.37 for the bottom, against 0.55 and
0.44 with none (2-minute windows from 2 to 20 minutes). The pulse as the
reward prediction error (`rpe`: 1 − p for a right answer, p for a wrong
one, p the electrode's running share of right ones; Frémaux, Sprekeler
and Gerstner 2010) works better. Scores over the session's second half,
seeds 1 to 8:

| | score | less gain 0, seed by seed | seeds above gain 0 |
|---|---|---|---|
| gain 0 | +0.00 ± 0.03 | | |
| `map`, gain 3 | +0.12 ± 0.04 | +0.11 ± 0.01 | 8 of 8 |
| `rpe`, gain 1 | +0.21 ± 0.03 | +0.20 ± 0.02 | 8 of 8 |
| `rpe`, gain 3 | +0.18 ± 0.03 | +0.18 ± 0.01 | 8 of 8 |

So the dish learns a mapping, a little: with `rpe` at gain 1, about 0.68
right for the top half and 0.52 for the bottom, from 0.56 and 0.44.
Most of it comes in the first two minutes, then nothing more.

**Then the game.** pong.py's `reward` condition sends the same kind of
pulse: 1 − p for a hit, p for a miss, p the running share of hits. 20
minutes on seeds 1 to 8, the columns layout, hits a rally by 5-minute
window:

| | 0-5 | 5-10 | 10-15 | 15-20 | last less gain 0 |
|---|---|---|---|---|---|
| gain 0 | 0.42 | 0.48 | 0.45 | 0.48 | |
| gain 1 | 0.45 | 0.56 | 0.50 | 0.52 | +0.04 ± 0.05 |
| gain 3 | 0.45 | 0.47 | 0.45 | 0.44 | −0.04 ± 0.05 |
| flipped, gain 0 | 0.40 | 0.41 | 0.38 | 0.39 | |
| flipped, gain 1 | 0.44 | 0.45 | 0.53 | 0.42 | +0.02 ± 0.06 |
| flipped, gain 3 | 0.43 | 0.45 | 0.43 | 0.43 | +0.04 ± 0.05 |

No learning that 8 seeds can see. An earlier version, with pulses of 1
for a hit and a miss alike, did no better: gains 0.3 to 100 on seeds 1 to
4, rows and columns, as is and flipped. Why was not taken apart; likely:
in `conditioning.py` the outcome comes 100 ms after the one stim that
caused it, in Pong seconds after a stream of stims at up to 40 Hz and a
paddle that answers to 100 ms of spikes, so the eligibility of the
synapses that mattered is a small part of all there is when the pulse
comes; and the stims' direct answers, which no weight changes, stay most
of what the motor regions hear (about 1 spike against 0.1-0.2 through
the synapses, 100 ms after a stim). A task with the reward right after
the answer, a closer reward, or a dish whose stims reach less of it
directly might do better.

**A reward for each move.** pong.py's `moves` condition brings the
reward closer: every 100 ms in which the paddle moved, a move towards
the ball's height counts 1 and one away 0, and the pulse is that less
the running share of moves towards it (the same prediction error, on
the same reward and punish channels); hits and misses get nothing.
`shuffled` is its control: the same pulses at the same times, but each
move's outcome a coin flip, so the reward says nothing about the move.
A screen on seeds 1 to 4 (as is and flipped) put eligibility at 200 ms
and gain 1 ahead of 1 s and of gain 3. Then 20 minutes on seeds 1 to 8,
the columns layout, `tau_elig=200`, hits a rally by 5-minute window:

| | 0-5 | 5-10 | 10-15 | 15-20 | moves towards the ball |
|---|---|---|---|---|---|
| gain 0 | 0.42 | 0.48 | 0.45 | 0.48 | 0.516 |
| `moves`, gain 1 | 0.52 | 0.58 | 0.58 | 0.50 | 0.548 |
| `shuffled`, gain 1 | 0.42 | 0.47 | 0.37 | 0.39 | 0.520 |
| flipped, gain 0 | 0.40 | 0.41 | 0.38 | 0.39 | 0.516 |
| flipped, `moves`, gain 1 | 0.46 | 0.45 | 0.58 | 0.54 | 0.547 |
| flipped, `shuffled`, gain 1 | 0.42 | 0.37 | 0.43 | 0.39 | 0.515 |

Seed by seed, the 16 pairs pooled over as is and flipped:

| | moves towards the ball | hits, last 5 minutes |
|---|---|---|
| `moves` less gain 0 | +0.032 ± 0.005, 16 of 16 above | +0.08 ± 0.05, 10 of 16 |
| `shuffled` less gain 0 | +0.001 ± 0.002, 11 of 16 | −0.05 ± 0.03, 5 of 16 |
| `moves` less `shuffled` | +0.030 ± 0.005, 16 of 16 | +0.13 ± 0.04, 12 of 16 |

So the dish learns what it is told: a small lean towards the ball, in
every pair, and not from the pulses alone, since the shuffled ones leave
it where it was. Whether that wins more hits is not settled: above
`shuffled`, yes (but `shuffled` itself may cost a little), above gain 0
not by more than the noise of 8 seeds. A lean of 3 moves in 100 is small
against a paddle that mostly follows the stims' direct answers.

**Shorter axons.** A stim excites the neurons whose axons pass near the
electrode, and the default axon is 0.274 of the dish long: longer than
the 2 to 3 electrode pitches between the sensory columns and the motor
ones. `reach.py` measures the motor regions' answer to a sensory stim
(spikes in the 100 ms after it, above those without one), with the
synapses and with all of them at 0, seeds 1 to 4, STDP off:

| axon | with synapses | without | direct share | all channels |
|---|---|---|---|---|
| 0.274 (default) | 1.09 | 0.96 | 0.88 | 11.8 |
| 0.2 | 0.64 | 0.65 | 1.01 | 11.5 |
| 0.15 | 0.38 | 0.26 | 0.67 | 11.3 |
| 0.11 | 0.27 | 0.09 | 0.33 | 11.6 |
| 0.08 | 0.25 | 0.09 | 0.35 | 11.0 |

From 0.11 down, the direct answer is gone and what is left reaches the
motor regions through synapses, while the dish's whole answer to a stim
stays about what it was. It costs some calibration: `validate.py ... 6
axon=0.11` keeps the same 10 of 12 statistics in the cultures' middle
half, but the early answer falls from 25.5 spikes to 18.1 (the middle
half starts at 17.6), the channels answering from 16 to 11 (further below
it) and the latency rises from 57 ms to 96; the score is 3.77 against
2.38. The defaults stay as they were; `axon=0.11` is a parameter of the
runs below.

`conditioning.py rpe 20 0,1 8 axon=0.11` learns about as before: +0.225
± 0.015 over gain 0, 8 of 8 seeds above. Pong, 20 minutes on seeds 1 to
8, columns, `axon=0.11` (and `tau_elig=200` for `moves` and `shuffled`):

| | 0-5 | 5-10 | 10-15 | 15-20 | moves towards the ball |
|---|---|---|---|---|---|
| gain 0 | 0.37 | 0.42 | 0.39 | 0.47 | 0.517 |
| `moves`, gain 1 | 0.59 | 0.65 | 0.70 | 0.60 | 0.558 |
| `shuffled`, gain 1 | 0.49 | 0.45 | 0.42 | 0.47 | 0.520 |
| `reward`, gain 1 | 0.42 | 0.47 | 0.51 | 0.48 | |
| flipped, gain 0 | 0.46 | 0.47 | 0.49 | 0.38 | 0.522 |
| flipped, `moves`, gain 1 | 0.62 | 0.68 | 0.60 | 0.64 | 0.571 |
| flipped, `shuffled`, gain 1 | 0.42 | 0.43 | 0.47 | 0.43 | 0.518 |
| flipped, `reward`, gain 1 | 0.47 | 0.58 | 0.51 | 0.50 | |

Seed by seed, the 16 pairs pooled over as is and flipped:

| | moves towards the ball | hits, last 5 minutes |
|---|---|---|
| `moves` less gain 0 | +0.045 ± 0.003, 16 of 16 above | +0.20 ± 0.05, 14 of 16 |
| `shuffled` less gain 0 | −0.000 ± 0.002, 10 of 16 | +0.03 ± 0.03, 8 of 16 |
| `moves` less `shuffled` | +0.045 ± 0.004, 16 of 16 | +0.17 ± 0.05, 15 of 16 |
| `reward` less gain 0 | | +0.07 ± 0.04, 11 of 16 |

With the direct answers gone, the reward for each move wins hits: about
0.62 a rally against 0.43 with nothing learnt (a still paddle: 0.39),
above both gain 0 and the shuffled pulses, as is and flipped alike. The
lean is larger too (4.5 moves in 100 against 3). Most of it is there in
the first 5 minutes and grows no further, as in `conditioning.py`. The
reward for hits and misses alone still shows nothing 8 seeds can tell
from noise. This is the model's answer, on a channel no CL1 has, with
axons shortened for it: not a claim about cultures.

**An hour.** The same, 60 minutes a session (`learn_study.py 60 8 0,1
... columns moves tau_elig=200 axon=0.11`, and `shuffled` at gain 1);
hits a rally, mean over the 16 sessions of each (as is and flipped):

| minutes | 0-5 | 5-20 | 20-40 | 40-60 | 50-60 |
|---|---|---|---|---|---|
| gain 0 | 0.41 | 0.43 | 0.41 | 0.42 | 0.41 |
| `moves`, gain 1 | 0.61 | 0.64 | 0.67 | 0.68 | 0.70 |
| `shuffled`, gain 1 | 0.46 | 0.44 | 0.45 | 0.43 | 0.41 |
| `moves` less gain 0, seed by seed | +0.19 ± 0.03 | +0.21 ± 0.03 | +0.26 ± 0.03 | +0.26 ± 0.03 | +0.29 ± 0.04 |
| pairs above | 14 of 16 | 16 of 16 | 16 of 16 | 16 of 16 | 16 of 16 |

`moves` stays above `shuffled` the same way (+0.26 ± 0.03 over minutes
40 to 60, 16 of 16), and leans towards the ball in 0.570 of its moves
against 0.519. What was learnt keeps: nothing fades over the hour. It
may grow a little after the first 5 minutes: the gap over gain 0 is
+0.05 ± 0.03 larger in minutes 40 to 60 than in 5 to 20, but up in only
9 of 16 pairs, so that is not settled. Most of it is still learnt in the
first 5 minutes. Part of the reason may be the rule itself: the pulse is
the outcome less the running share of moves towards the ball, so as the
lean grows, each pulse shrinks. The 48 sessions took about 4 hours on 22
processes.

**Hits and misses, 24 seeds.** The `reward` condition (a pulse for each
hit and miss, none for moves) on the same dish, 20 minutes on seeds 1 to
24 (seeds 1 to 8 give the same rallies as the run above), as is and
flipped: 48 pairs.

| minutes | 0-5 | 5-10 | 10-15 | 15-20 | 10-20 |
|---|---|---|---|---|---|
| gain 0 | 0.45 | 0.43 | 0.45 | 0.43 | 0.44 |
| `reward`, gain 1 | 0.48 | 0.51 | 0.53 | 0.51 | 0.52 |
| less gain 0, seed by seed | +0.03 ± 0.02 | +0.08 ± 0.02 | +0.08 ± 0.02 | +0.08 ± 0.02 | +0.08 ± 0.015 |
| pairs above | 30 of 48 | 35 of 48 | 31 of 48 | 29 of 48 | 39 of 48 |

So the reward for hits and misses teaches something too, now that 48
pairs can see it: +0.08 hits a rally (as is +0.09 ± 0.04, flipped +0.07
± 0.02), in place by minutes 5 to 10 and no more after. That is about a
third of what the reward for each move gives in the same time (+0.20,
above). 96 sessions took about 3 hours.

Its control, `hits_shuffled`: the same pulses at the same hits and
misses, but each outcome said is a coin that comes up a hit with the
running share of real hits, so the pulses are alike in size and number
and say nothing. Gain 1 on the same 24 seeds, as is and flipped, against
the same gain 0 and `reward` sessions:

| minutes | 0-5 | 5-10 | 10-15 | 15-20 | 10-20 |
|---|---|---|---|---|---|
| `hits_shuffled`, gain 1 | 0.42 | 0.41 | 0.46 | 0.42 | 0.44 |
| `hits_shuffled` less gain 0 | −0.03 ± 0.02 | −0.02 ± 0.02 | +0.01 ± 0.02 | −0.01 ± 0.02 | +0.00 ± 0.02 |
| `reward` less `hits_shuffled` | +0.06 ± 0.02 | +0.10 ± 0.02 | +0.07 ± 0.02 | +0.08 ± 0.03 | +0.07 ± 0.02 |
| pairs above | 33 of 48 | 39 of 48 | 33 of 48 | 35 of 48 | 35 of 48 |

The shuffled pulses play as gain 0 does; `reward` stays above them by
what it is above gain 0. So what it learns comes from the pulses saying
hit or miss, not from there being pulses.

**Why it stops.** In every run here most of the learning is in the first
minutes. `conditioning.py rpe 10 0,1 8 axon=0.11` (score less gain 0,
seeds 1 to 8) with one thing changed at a time:

| changed | score less gain 0 |
|---|---|
| nothing | +0.255 ± 0.020 |
| `w_max=0.012` (twice) | +0.266 ± 0.036 |
| `w_max=0.024` | +0.149 ± 0.031 |
| `home_gain=0` (no scaling) | +0.504 ± 0.061 |
| `home_gain=0 w_max=0.024` | +0.031 ± 0.048 (a bias: up for nearly every stim) |
| `home_gain=0 w_exc=0.0027` | +0.255 ± 0.055 |
| `noise_rate=0.00266` (half) | +0.229 ± 0.039 |
| `noise_rate=0.00133` (a quarter) | −0.05 ± 0.19 (weights run to 0 on some seeds) |

Scaling off looks like the answer, but is not: without it the weights
stay at w_exc (0.00194), where the dish is nearly silent (`validate.py`
with `stdp=0`: 0.17 Hz, no bursts; Pong's motor regions 16 Hz against
118), and a quiet dish's answers are its stims'. Started at what scaling
settles to (`w_exc=0.0027`), or settled for 5 minutes with scaling and
then without it (`paths_probe.py 10 axon=0.11 settle=5 freeze=1`), it
learns as much as with scaling, and stops as soon. `paths_probe.py`
follows the weights (`Dish.paths`, the sums by electrode pair): from
each sensory electrode's neurons to the region it should drive and to
the other, at gain 1 after 5 minutes of settling, 0.555 and 0.547 of
w_max at the start, 0.645 and 0.621 after 3 minutes, and so to the end;
nowhere near w_max, and the prediction error is still 0.2 a trial. Nor
is it bursts: split by the dish's spikes in the 100 ms before the stim,
the quietest quarter of the trials scores +0.28 over gain 0, the
busiest +0.18. What is left: the weights stop diverging at a small
difference, likely where STDP's soft bounds (a strengthening by
`w_max - w`, a weakening by `w`) balance the reward's pull, each path at
a fixed point of its own. (That the answers are too few spikes, read
from the quiet before most stims, was a guess; counted, an answer is
about 26 motor spikes, see below.)

**Hard bounds.** With `hard=1` the steps no longer shrink near the
bounds. The plateau is partly the soft bounds', not all theirs:

| `conditioning.py rpe`, `axon=0.11`, seeds 1 to 8 | soft bounds | `hard=1` |
|---|---|---|
| 10 minutes, score less gain 0 | +0.255 ± 0.020 | +0.313 ± 0.016 (8 of 8 higher) |
| 20 minutes, score less gain 0 (minutes 10 to 20) | +0.225 ± 0.015 | +0.365 ± 0.021 (8 of 8 higher) |
| 10 minutes, `home_gain=0 w_exc=0.0027` | +0.255 ± 0.055 | +0.323 ± 0.039 |

The right paths pull further ahead (`paths_probe.py 10 axon=0.11 hard=1`:
0.513 against 0.455 of w_max at minute 10, against 0.564 and 0.529 with
soft bounds), but the right share still rises mostly in the first 3
minutes and then creeps (top half/bottom half right 0.70/0.57 in the
first 2 minutes, 0.76/0.62 in the last 2 of 20; soft bounds 0.66/0.52
and 0.65/0.56). No path meets 0 or w_max. Settled 5 minutes and then
without scaling (`settle=5 freeze=1`) both paths first grow (0.73 and
0.71 of w_max) and then sink together as the dish quiets (5.5 Hz a
channel to 2.4), the right share staying near 0.69: additive STDP here
weakens more than it strengthens, and without scaling nothing holds the
dish. So scaling is not the brake with hard bounds either.

Nor is the size of the answer. `conditioning.py` now counts it (motor
spikes in the answer window) and takes `stim=` (the sensory stim's
size) and `answer=` (the window, ms); 10 minutes, `axon=0.11 hard=1`,
seeds 1 to 8:

| | motor spikes an answer (gain 0) | score less gain 0 |
|---|---|---|
| as is | 25.6 | +0.313 ± 0.016 |
| `stim=2` | 25.8 | +0.329 ± 0.040 |
| `stim=3` | 25.9 | +0.377 ± 0.019 |
| `answer=200` | 36.8 | +0.310 ± 0.042 |

An answer is some 26 spikes, not a few; a stronger stim hardly adds to
it (with axons of 0.11 it reaches few motor neurons) and a longer window
adds spikes but not learning. `stim=3` may help a little (+0.06, about
two standard errors). Where the learning stops stays open.

With plain STDP (`modulated` off, as the calibration ran), `hard=1`
keeps the dish where the calibration put it: `validate.py ... 6
axon=0.11 hard=1` has 9 of the 12 statistics in the cultures' middle
half (bursts a minute 4.2 and the share of spikes in bursts 0.903, both
just above it; soft bounds 10 of 12), score 3.72 against 3.77, and over
45 minutes (`--long 6`) the rate stays 3.83 to 3.84 Hz, burst lengths
growing a little (1.66 to 1.74 s).

Pong with the reward for each move (`learn_study.py 20 8 0,1 out.json
columns moves tau_elig=200 axon=0.11 hard=1`, as is and flipped, 16
pairs), minutes 10 to 20:

| hits a rally | soft bounds | `hard=1` |
|---|---|---|
| gain 1, mean | 0.63 | 0.80 |
| less gain 0 (0.43) | +0.20 ± 0.03 (16 of 16) | +0.37 ± 0.03 (16 of 16) |
| hard less soft | | +0.17 ± 0.04 (14 of 16) |
| moves towards the ball | 0.564 | 0.590 |

Its control, `shuffled` with `hard=1`: 0.43 hits a rally, −0.00 ± 0.03
over gain 0, and `moves` +0.37 ± 0.04 above it (16 of 16): what is
learnt comes from what the pulses say, as with soft bounds.

The reward for hits and misses (`learn_study.py 20 24 0,1 out.json
columns axon=0.11 hard=1`, and `hits_shuffled` for its control; seeds 1
to 24, as is and flipped, 48 pairs), minutes 10 to 20:

| hits a rally | soft bounds | `hard=1` |
|---|---|---|
| gain 1, mean | 0.51 | 0.61 |
| less gain 0 (0.43) | +0.08 ± 0.01 (39 of 48) | +0.18 ± 0.02 (46 of 48) |
| hard less soft | | +0.11 ± 0.02 (39 of 48) |
| `hits_shuffled` less gain 0 | +0.00 ± 0.02 | +0.02 ± 0.02 |
| less `hits_shuffled` | +0.07 ± 0.02 | +0.17 ± 0.02 (42 of 48) |

Hard bounds more than double what either reward teaches; the reward
for hits and misses still gets about half of what the one for each
move does (+0.18 against +0.37).

Is it the credit, a hit or miss coming seconds after the moves that made
it? A longer eligibility should then help; it does not (seeds 1 to 8, as
is and flipped, 16 pairs, `hard=1`, minutes 10 to 20):

| `tau_elig` | hits a rally | less gain 0 | less `tau_elig` 1000 |
|---|---|---|---|
| 1000 ms (default) | 0.63 | +0.20 ± 0.04 | |
| 2000 ms | 0.59 | +0.16 ± 0.03 | −0.04 ± 0.03 |
| 5000 ms | 0.50 | +0.07 ± 0.02 | −0.13 ± 0.04 (12 of 16 lower) |

A longer trace carries more of the moves that did not matter, and the
reward for each move did best with a shorter one (200 ms).

As every parameter here, `hard` is a choice of the model's, not a fact
of a culture's: the dish was calibrated with soft bounds (and its
calibration is not checked again here). With `modulated` at gain 0
only scaling changes the weights, so `hard` changes nothing there (the
gain 0 rows are the same with and without it); with plain STDP
(`modulated` off, as the calibration ran) it does, and its calibration
check is above.

**DishBrain's feedback, with no reward channel.** `stim`'s feedback
(a hit: the 8 sensory electrodes at 100 Hz for 100 ms; a miss: 4 s of
stims on random electrodes at 5 Hz) needs nothing a CL1 lacks. On the
`rows` layout it could not show (the stims played the game along the
axons; see [Pong in closed loop](#pong-in-closed-loop)); `feedback_study.py`
plays it on `columns` with short axons, plain STDP (no `modulated`), as
DishBrain compared it: against `silent`, `none` and `stim/off` (weights
fixed, `stdp=0 w_exc=0.0027`). 20 minutes, seeds 1 to 8 as is and
flipped (16 pairs), hits a rally:

| | soft bounds, first 5 min / after | `hard=1`, first 5 min / after |
|---|---|---|
| `stim` | 0.45 / 0.42 | 0.45 / 0.41 |
| `silent` | 0.39 / 0.43 | 0.38 / 0.39 |
| `none` | 0.46 / 0.42 | 0.39 / 0.41 |
| `stim/off` | 0.45 / 0.45 | 0.45 / 0.45 |
| `stim`: after less first 5 min | −0.03 ± 0.03 | −0.05 ± 0.05 |
| `stim` less `silent`, after | −0.01 ± 0.03 | +0.02 ± 0.02 |
| `stim` less `none`, after | +0.00 ± 0.02 | +0.00 ± 0.02 |

Nothing learnt, with either bound: every session plays at about what
the gain 0 sessions above do (0.43), `stim` does not get better and does
no better than its controls. (`stim/off` is not matched in activity:
its motor regions fire at 21 Hz against 118, the weights fixed below
where STDP and scaling hold them in play; the comparisons with `silent`
and `none` do not have that problem.) DishBrain reported its cultures
improving under `stim` and not under the controls; this dish, whose
STDP and scaling have no term the feedback's predictability could act
on, does not. That is a statement about the model, not about cultures,
and is the comparison a real dish could settle.

```sh
build/cl_bridge/venv/bin/python examples/cl_bridge/conditioning.py \
    bias|map|rpe [minutes [gains [seeds [name=value ...]]]]
build/cl_bridge/venv/bin/python examples/cl_bridge/learn_study.py \
    [minutes [seeds [gains [out.json [columns] [moves|shuffled|hits_shuffled] [name=value ...]]]]]
build/cl_bridge/venv/bin/python examples/cl_bridge/reach.py [axons [name=value ...]]
build/cl_bridge/venv/bin/python examples/cl_bridge/feedback_study.py \
    [minutes [seeds [out.json [name=value ...]]]]
```

A `conditioning.py` run of 3 gains on 8 seeds (20 minutes each) takes
about 25 minutes on 22 processes, with the dish alone; a `learn_study.py`
run of 48 sessions, through the SDK, about 85.
