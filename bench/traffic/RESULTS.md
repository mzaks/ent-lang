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
