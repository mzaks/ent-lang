# Traffic on roads of several lanes: timings

`examples/traffic/lanes.ent` at scale (`bench/traffic/highway.ent`):
1,000 ring roads of three lanes and 3 km, 120 cars and trucks each,
120,000 in all, 1,000 steps of 0.1 s. The cars are put in order by lane
and place twice a step, by hand: a `for ... by` the key appends them to
the buffer `Order`, a proc tells each car its row, and a car finds the
car ahead and behind in its lane next to its row, and those in the next
lane by halving. That is the baseline for an index of the language.

```sh
ENT_CFLAGS=-march=native tools/ent run [--parallel] --trace \
  -I examples/traffic bench/traffic/highway.ent
```

## 2026-10-22, the order by hand

The Ryzen AI 9 HX 370 (24 threads), on mains power; 162,849 lane
changes and no car standing on the one ahead, on one core and on all.

| 120,000 cars, a step | one core ms | all cores ms |
|---|---|---|
| `sort` (twice: the `by` sort and the rows) | 2.34 | 4.34 |
| `think` (twice: IDM behind the car ahead) | 0.86 | 0.30 |
| `decide` (MOBIL: halving in the next lane) | 4.91 | 0.55 |
| `drive` | 0.19 | 0.27 |
| the step | 8.34 | 5.52 |

- The order is most of the step on all cores: the `by` sort runs on one
  core and took 2.6 ms there where it takes 1.1 on one core alone
  (telling the cars their rows, 0.06 to 0.13 ms, apart from it). What is
  sorted hardly changes from one step to the next: a car that changed
  lanes, or went round the ring, moves; all others keep their order.
- Before the cars were told their rows, every car found the car ahead in
  its lane by halving too, in 31 rounds: `think` 4.15 ms a call and
  `decide` 10.3, 21.0 ms a step on one core.

## 2026-10-22, why the sort is slower on all cores

It is the thread beside it. The sort runs on thread 0, between parallel
loops, while the other threads wait for the next loop spinning (for
5 ms, `KMP_BLOCKTIME` as tools/ent sets it); one of them is on CPU 12,
the other hardware thread of CPU 0's core, and takes half of the core
from the sort. Smaller, the same: 120,000 entities, a `for ... by` a key
into a buffer after a `for` that writes the keys
(`ENT_PARALLEL_MIN=1`, so that it runs on all threads), 1,000 times:

| the sort, ms a call | |
|---|---|
| built without `--parallel` | 0.82 |
| 24 threads, the writing loop on one | 0.84 |
| 24 threads, the writing loop on all, spinning | 1.83 |
| the same, `KMP_BLOCKTIME=0` | 1.01 |
| the same, `OMP_WAIT_POLICY=passive` | 0.96 |
| 12 threads, one a core (`OMP_PLACES=cores`), spinning | 0.86 |
| 4 threads, one a core | 0.84 |

What is left beside the spinning (0.96 where 0.82) is reading what the
other cores have just written. In `highway.ent` (a sort a call, all
cores):

| | sort ms | step ms |
|---|---|---|
| 24 threads (as above) | 2.0-2.4 | 5.3-6.1 |
| 24 threads, `OMP_PLACES=threads` or `cores` and bound | 2.0-2.1 | 5.3-5.4 |
| 24 threads, `OMP_WAIT_POLICY=passive` | 1.62 | 4.54 |
| 12 threads, `OMP_PLACES=cores OMP_PROC_BIND=close` | 1.39 | 3.92 |
| the same, passive | 1.46 | 4.29 |
| 12 threads, not bound | 1.96 | 5.11 |

With a thread a core the parallel loops lose a little (`decide` 0.70 ms
where 0.59) and the step gains a third.

The others gain too. The city's epidemic, a million people for 100
days (`BENCH_CPUS=0-23 bench/epidemic/run.py 1000000`, the median of
three, 857,886 infected each way), and beecs's colonies, 100 at once for
a year (`examples/beecs/many.ent`, every forager a squadron of its own,
the same lines to the byte):

| | 24 threads | 12, one a core (`OMP_PLACES=cores OMP_PROC_BIND=close`) |
|---|---|---|
| epidemic, a day | 13.5 ms | 12.2 ms |
| epidemic `workplace`, `chance` | 4.85, 4.43 ms | 4.40, 3.90 ms |
| beecs, 100 colonies a year (twice) | 155.5, 156.7 s | 129.0, 128.6 s |

So tools/ent now has a program built with `--parallel` start a thread
for each core it may run on, bound to it (`OMP_NUM_THREADS` the cores of
its CPUs, `OMP_PLACES=cores`, `OMP_PROC_BIND=close`), unless the
environment says how many or where, and not with `--ranks`. `highway.ent`
so: 12 threads, 3.9 ms a step; under `taskset -c 0-3`, 4 threads.
