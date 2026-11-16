# Training a spiking network with e-prop

A recurrent network of leaky integrate-and-fire neurons that learns, in
ent-lang alone: the network is entities and relations, the learning rule
is systems, and nothing runs backwards. `examples/snn.ent` and
`examples/snn_pull.ent` run a fixed network; this one changes its weights.

| file | what is in it |
|---|---|
| `network.ent` | the units, neurons, readouts, synapses; one step of the network |
| `learn.ent` | e-prop: eligibility traces, learning signals, the weights moved |
| `train.ent` | the network made, samples counted into epochs, a line for each, a checksum |
| `toy.ent` | a task made up for it (no download) |
| `check/toy.c` | the toy trained in plain C, to compare to the bit |

## The learning rule

e-prop (Bellec et al., Nature Communications 2020) replaces
backpropagation through time with two things there are at every step:

- each synapse's **eligibility trace**, `elig = kappa * elig + psi_j * trace_i`:
  the source's spikes filtered by the membrane's decay (`trace_i`, how
  the target's potential depends on the weight), times how near its
  threshold the target was (`psi_j`, the surrogate slope);
- each neuron's **learning signal**, the readouts' errors
  (`softmax(y) - target`) sent back along its own readout edges by their
  weights (symmetric e-prop; the paper's random fixed feedback weights
  are a variant, not built).

A synapse adds `learn_j * elig` to its gradient each step; at a sample's
end the weights move against the gradients (plain gradient descent, a
sample a batch; the paper used Adam and batches). It is an
approximation: a neuron's spike that changes other neurons later is not
followed. No autodiff, no stored history.

The order of one step, so that the trace a synapse's eligibility uses is
made of exactly the spikes that went into its target's potential:
encode, propagate (inputs of this step, hidden spikes of the step
before), traces, integrate, read out, softmax, error, then the learning.

Simplifications: weights may change sign (no Dale's law); there is no
refractory period; the readouts' biases learn, the neurons have none.

## The toy task

Two groups of ten input channels; in each sample one group fires (40% a
step) for ten steps, then the other for ten, then all are quiet for ten.
Class 0 is A then B, class 1 B then A: the same spikes in all, so what
tells them apart is the order. The error counts in the quiet window.
32 hidden neurons, half of the hidden pairs connected, 2 readouts.

```
tools/ent run examples/snn_train/toy.ent
```

prints, for each of 30 epochs, the loss and how many of the 200 new
training samples came out right as it learned them, then how many of 200
fixed test samples, which it does not learn from:

```
epoch 1 loss 0.565238 train 148/200 test 200/200
epoch 2 loss 0.074174 train 198/200 test 200/200
...
epoch 30 loss 0.000123 train 200/200 test 200/200
weights 100859224272
```

With the learning rate at nought it stays at chance (103 and 114 of 200).
The task is easier than it looks: without the hidden-to-hidden synapses
it is learned too, more slowly (test 200/200 in the fifth epoch), from
which group fired last, which the membranes and the readouts still hold
in the quiet window. It checks the learning, not memory.

## Checked

- `check/toy.c` trains the same network in C: the same numbers at random
  (wyhash), the same order of every sum (a neuron's synapses by source,
  the hidden neurons before the inputs, as ent's ids order them). Its
  output is the same as the program's, byte for byte, the checksum of the
  bits of every weight included.
- With `--parallel`, on 8 threads, the output is the same again.

`test/Integration/snn_train.test` runs all three and compares them.
