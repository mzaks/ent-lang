# Training a spiking network: e-prop, and BPTT to compare

A recurrent network of leaky integrate-and-fire neurons that learns, in
ent-lang alone: the network is entities and relations, the learning
rules are systems. `examples/snn.ent` and `examples/snn_pull.ent` run a
fixed network; this one changes its weights, two ways:

- **e-prop** (Bellec et al., Nature Communications 2020), which runs
  forward only, with no history kept;
- **backpropagation through time** (BPTT), written by hand: the forward
  pass writes down what the way back needs, then the steps are gone
  through backwards. The ceiling e-prop is compared with.

| file | what is in it |
|---|---|
| `network.ent` | units, neurons, readouts, synapses; one step of the network |
| `learn.ent` | e-prop (eligibility traces, learning signals), softmax and error, Adam, the score |
| `bptt.ent` | BPTT: the tape of the forward pass, the adjoints backwards, the gradients |
| `train.ent` | the network made, samples counted into epochs, a line for each, a checksum |
| `toy_data.ent`, `toy.ent`, `toy_bptt.ent` | a task made up for it (no download), trained both ways |
| `shd_data.ent`, `shd.ent`, `shd_bptt.ent` | the Spiking Heidelberg Digits, trained both ways |
| `fetch.sh`, `prepare.py` | SHD downloaded, and made into one flat file |
| `check/toy.c` | the toy trained with e-prop in plain C, to compare to the bit |
| `check/same_gradient.ent` | BPTT without its recurrent term against e-prop, gradient by gradient |

## The network

Input units (an `Input` channel each), hidden neurons and readouts are
entities; `Synapse` edges go from any unit to a hidden neuron, `Out`
edges from the hidden neurons to the readouts. A step, in this order:

| system | what it does |
|---|---|
| `encode` | the data sets each input's `fired` (0 or 1) |
| `propagate` | each neuron sums its synapses' weights times their sources' `fired`: the inputs of this step, the hidden spikes of the step before |
| `traces` | each unit's `trace = alpha * trace + fired` |
| `integrate` | `v = alpha * v + input`; the surrogate slope `psi = gamma * max(0, 1 - abs(v - thr) / thr) / thr`; at the threshold the neuron fires and `v` drops by it |
| `read_out` | `y = kappa * y + (Out weights of the neurons that fired) + bias` |
| `soft_max`, `error` | `err = (softmax(y) - target) / window` in the steps where the error counts |

The answer is the readout whose `y` summed most over those steps; the
loss is the cross entropy there. Simplifications: weights may change
sign (no Dale's law), no refractory period, no adaptive thresholds, the
hidden neurons have no bias, and the reset is not differentiated.

## e-prop

Each synapse keeps an eligibility trace, `elig = kappa * elig + psi_j *
trace_i`: its source's spikes filtered by the membrane's decay (how its
target's potential depends on the weight), times how near its threshold
the target was. Each neuron gets a learning signal, the readouts' errors
sent back along its own `Out` edges by their weights (symmetric e-prop;
the paper's random fixed feedback weights are not built). A synapse adds
`learn_j * elig` to its gradient each step. The readouts' weights take
`err` times the neuron's spikes filtered by `kappa`, and their biases
`err` times 1 filtered by `kappa` (a bias goes into `y` every step). What
e-prop leaves out is how a spike changes other neurons later.

The order of a step is what makes this exact: the trace a synapse's
eligibility uses is made of the very spikes that went into its target's
potential that step.

## BPTT, and the journal it needs

Forward, each hidden neuron writes its spike and slope of every step into
rows of its own of the buffer `Tape` (a window of `Run.steps` rows), each
readout its error into `ReadTape`. Backwards, step by step from the last:

```
ybar_k(t) = err_k(t) + kappa * ybar_k(t+1)
zbar_j(t) = sum_k w_out_kj * ybar_k(t)  +  through * sum_m w_mj * vbar_m(t+1)
vbar_j(t) = psi_j(t) * zbar_j(t) + alpha * vbar_j(t+1)
```

and the gradients `vbar_j(t) * s_i(t)` (`s_i`: what the source sent into
`j` at `t`, the inputs' spikes encoded again from the data, the hidden
ones' of `t - 1` read back from the tape), `ybar_k(t) * z_j(t)` and
`ybar_k(t)`. The second sum is what e-prop leaves out.

With it left out (`Backward.through = 0`) the gradient is e-prop's, the
same sums added in another order: `check/same_gradient.ent` computes both
from the same weights on the same sample, and they agree within 0.3
millionths of the largest, for every synapse, readout weight and bias.
With it in, the synapses' gradients differ by about a third.

The tape is a journal of the forward pass written by hand: per entity,
two fields, a row per step, read back in reverse. For SHD it is 256 × 100
rows of 8 bytes, 200 KB, against the 2.5 KB a tick Pixalorien's undo log
keeps; what a language-level journal would have to give for this is
named fields of a component kept for a run of steps (not the whole
world, not every write: e-prop's eligibility traces, written every step
on every synapse, would be 20 million entries a sample), and a way to
read a step back by its number.

## Learning

The gradients are summed over a batch and the weights move with Adam
(Kingma and Ba, 2015: b1 0.9, b2 0.999, eps 1e-8). Plain gradient
descent, a sample at a time, learned the toy but not SHD reliably: the
gradients of the inputs, the recurrent weights and the biases differ by
orders of magnitude, and one rate was too much for some and too little
for others (with the rate that suited the weights, the hidden neurons
went quiet within an epoch).

## The toy task

```
tools/ent run examples/snn_train/toy.ent
tools/ent run examples/snn_train/toy_bptt.ent
```

Two groups of ten input channels; in each sample one group fires (40% a
step) for ten steps, then the other for ten, then all are quiet for ten.
Class 0 is A then B, class 1 B then A: the same spikes in all, so what
tells them apart is the order. The error counts in the quiet window. 32
hidden neurons, half the hidden pairs connected, 2 readouts, Adam at
0.01 on batches of 10. Each epoch learns from 200 new samples, then
tests 200 fixed ones it never learns from:

| | epoch 1 loss | epoch 3 | epoch 30 | train, test from |
|---|---|---|---|---|
| e-prop | 0.363 | 0.0052 | 0.000053 | epoch 2, epoch 1 |
| BPTT | 0.344 | 0.0013 | 0.000016 | epoch 2, epoch 1 |

The task is easier than it looks: without the hidden-to-hidden synapses
e-prop learns it as fast (loss 0.33 in the first epoch, every test
sample right from it on), from which group fired last, which the membranes
and the readouts still hold in the quiet window. It checks the learning,
not memory.

## The Spiking Heidelberg Digits

Spoken digits, 0 to 9 in English and German, 20 classes, through a model
of the cochlea into 700 channels (Cramer et al., 2020, CC BY 4.0): 8156
samples to train on, 2264 to test, the test speakers partly not among
the training ones.

```
examples/snn_train/fetch.sh
python3 -m venv build/snn_train/venv
build/snn_train/venv/bin/pip install h5py numpy
build/snn_train/venv/bin/python examples/snn_train/prepare.py
cd build/snn_train/data
ENT_PARALLEL_MIN=256 ../../../tools/ent build --parallel ../../../examples/snn_train/shd.ent -o shd
./shd [epochs [samples [tests]]]
```

`prepare.py` puts each sample into 100 steps of 14 ms (the longest is
1.37 s) and a channel on in a step where it spiked in it (a third of the
spikes share a step with another of their channel); 92 MB, read whole.
700 inputs, 256 hidden neurons, every input to every neuron, a fifth of
the hidden pairs connected, 20 readouts; alpha = kappa = 0.9, thr 1,
gamma 0.3; the error counts in every step; Adam at 0.001 on batches of 32.
At the end the network is saved (`shd.sav`, `shd_bptt.sav`, by
devices/world.ent).

20 epochs each, all threads, the same seed, network and samples (chance
is 1 in 20, 113 of 2264):

| epoch | e-prop train | e-prop test | BPTT train | BPTT test |
|---:|---:|---:|---:|---:|
| 1 | 34.4% | 892 (39.4%) | 33.4% | 795 (35.1%) |
| 3 | 66.5% | 1318 (58.2%) | 54.1% | 1211 (53.5%) |
| 6 | 79.6% | 1460 (64.5%) | 67.9% | 1415 (62.5%) |
| 11 | 89.7% | **1559 (68.9%)** | 78.2% | 1431 (63.2%) |
| 17 | 94.3% | 1457 (64.4%) | 85.4% | **1552 (68.6%)** |
| 20 | 95.6% | 1533 (67.7%) | 89.6% | 1495 (66.0%) |
| time | | 26 min | | 52 min |

Both learn well above chance and end near each other, about two thirds of
the test samples. e-prop fits the training samples faster and overfits
from about epoch 10 (its training accuracy climbs, its test accuracy
does not); a test accuracy moves by 50 to 150 samples from one epoch to
the next, so the epochs' best are not to be read closely. That BPTT, the
exact gradient of the same network, does no better is the result as it
came, not explained: the rate, batch and initial weights were picked on
e-prop and the toy and used for both unchanged; BPTT's gradient through
the recurrent synapses is not clipped; the reset is differentiated by
neither; and plain LIF neurons hold little over 1.4 s (the e-prop paper's
best results use adaptive thresholds, which are not built). These
numbers are not compared with published ones here: they have not been
checked against the papers.

`ENT_PARALLEL_MIN=256`: the loops have 256 neurons and some 750 synapses
each, far below the million entities from which tools/ent runs a loop on
many threads by default. Timings are in bench/RESULTS.md (2026-11-16):
e-prop's learning costs 0.61 ns a synapse a step on one thread, as much
as the network's own step; an epoch on 24 threads is 78 s, BPTT's 156 s.

## Checked

- `check/toy.c` trains the toy with e-prop in C: the same numbers at
  random (wyhash), the same order of every sum (a neuron's synapses by
  source, the hidden neurons before the inputs, as ent's ids order them),
  the same Adam. Its output is the program's, byte for byte, the
  checksum of the bits of every weight included.
- The toy with `--parallel` on 8 threads, and SHD on 24 (`2 300 200`),
  both ways: the same output as on one thread.
- `check/same_gradient.ent`: BPTT without the recurrent term against
  e-prop, as above.

`test/Integration/snn_train.test` runs all of them but SHD.
