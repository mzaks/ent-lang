# Spec: parking a vehicle in a tight spot, in ent-lang

*For `examples/parking/`. The habits of `examples/traffic/` and
`examples/analog/`: a model module without a window, a window view that
imports it, numbered checks that the check scripts name.*

## 1. Goal

A live demo, in a window, of one vehicle being parked in a tight spot,
by the keys or by an automatic parker, in which the viewer sees **two
worlds side by side**: where things are (the truth) and where the
vehicle believes they are (the belief, made from its sensors and its
odometry). The automatic parker sees only the belief; collisions and
clearances are judged against the truth. The gap between the two is
what the demo is about.

The viewer switches the vehicle (a table of real cars, §4), its sensor
suite (§6) and the scene (§9) while it runs.

The sensor model that is worked out in detail is **Tesla Vision** on a
Model 3: cameras only, a blind zone close to the bumpers, and obstacles
there remembered from when they were last seen and carried along by the
odometry (§7). The older car with 12 ultrasonic sensors is the
counterpart on the same body.

## 2. Scope

### In

- Flat ground, two dimensions, with a height class for every obstacle
  (§5.3): what the body, the mirrors and the tyres can touch, and what
  each sensor can see, depend on it.
- A kinematic single-track ("bicycle") model with a steered rear axle
  where the vehicle has one: no tyre slip, which holds at parking speeds
  (below 10 km/h).
- Steering and speed that take time: a steering rate, an acceleration
  and a braking limit, later a delay.
- `f32`, but for the pose, which is `f64` (ten thousand steps of a few
  millimetres are added to it; in an `f32` a vehicle is millimetres off
  its circle after 80 m). A step of 1/120 s.

### Not here (for now)

- Slopes, kerb climbing, suspension, tyre deformation.
- Camera images. A camera is modelled by what it would let the
  occupancy network know (§7), not by pixels.
- Radar (it plays no part in parking but for cross traffic, §9, later).
- A trailer's own weight and tyres: it is kinematic, as the vehicle
  is (§5.5).

## 3. Layout

| File | Contents |
|---|---|
| `README.md` | What is here, how to run it, what is checked. |
| `vehicles.ent` | The table of vehicles (§4) and of sensor suites (§6). |
| `park.ent` | The model: the vehicle's motion, the obstacles, clearance and collision (§5). No window. |
| `scenes.ent` | The scenes as tables of boxes (§9). |
| `sense.ent` | The sensors: what each reports of the truth (§6). |
| `believe.ent` | The belief: odometry, the occupancy grid, remembering (§7). |
| `parker.ent` | The automatic parker: plan and follow (§8). |
| `lot.ent` | The car park of many vehicles (§9, scene 13). |
| `tow.ent` | Backing with a trailer: the knob and the way into a slot (§5.5). |
| `show.ent` | The program: the window, the keys, both worlds drawn. |
| `check/*.py`, `check/*.ent` | The checks of §10. |

## 4. Vehicle data

A row of `vehicles` is everything the model needs of a vehicle:

| Field | Meaning |
|---|---|
| `length`, `width`, `mirrors` | Overall length, width of the body, width with the mirrors out (m). |
| `wheelbase`, `front`, `rear` | Wheelbase, front and rear overhang (m); they add up to `length`. |
| `track` | Track width (m), taken the same front and rear. |
| `circle` | Turning circle kerb to kerb (m): the diameter of the outer front wheel's path at full lock. |
| `rear_steer` | Most the rear axle steers, against the front, at parking speed (degrees; 0: none). |
| `turns` | Turns of the steering wheel lock to lock. |
| `corner` | Radius of the body's corners seen from above (m). |
| `tyre`, `tyre_width` | Tyre diameter and width (m). |
| `clearance` | Ground clearance (m). |
| `ticks` | Wheel encoder edges per wheel turn (§7.1). |

The most the front wheels steer is not in the table: it is worked out
from `circle`, `wheelbase`, `track` and `rear_steer` (§5.1), so a
vehicle driven at full lock turns in its published circle. Check 1.2
holds that.

### The vehicles

| | length | width | mirrors | wheelbase | front | rear | track | circle | rear steer | turns |
|---|---|---|---|---|---|---|---|---|---|---|
| Tesla Model 3 (2017–2023) | 4.694 | 1.849 | 2.088 | 2.875 | 0.841 | 0.978 | 1.580 | 11.6 | 0 | 2.00 |
| Tesla Model 3 (2024, "Highland") | 4.720 | 1.850 | 2.089 | 2.875 | 0.868 | 0.977 | 1.584 | 11.7 | 0 | 2.14 |
| smart fortwo (C453) | 2.695 | 1.663 | 1.893 | 1.873 | 0.424 | 0.398 | 1.45* | 6.95 | 0 | 3.4* |
| Mercedes EQS 450+ (rear-axle steering 10°) | 5.223 | 1.926 | 2.125 | 3.210 | 0.89* | 1.123* | 1.67* | 10.9 | 10 | 2.1* |
| Mercedes Sprinter L3 panel van | 6.967 | 2.020 | 2.345* | 4.325 | 1.021 | 1.621 | 1.72* | 14.4 | 0 | 3.9* |

Sources: the Model 3 rows are the "Dimensions and Weights" tables of
Tesla's owner's manuals (the 2017–2023 one and the current one), as
quoted by search results; the manual pages themselves refused an
automatic fetch, so **these are to be held against the manual of the
car once by hand**. The others are from spec sites that agree with each
other (auto-data.net, automoli.com, ev-database.org, vandimensions.com);
the Sprinter's circle is its kerb-to-kerb 47.2 ft, its front overhang
what the others leave of its length. A star marks an estimate of mine
where no source gave the number; none of the starred ones changes where
the body goes but the EQS's split of its overhangs.

`corner`, `tyre`, `clearance` and `ticks` are estimates for all (corner
0.35 m for the saloons, 0.25 the smart, 0.15 the van; 96 encoder edges
a wheel turn, which is 2.1 cm of a Model 3's tyre).

## 5. The truth

### 5.1 Motion

The vehicle's pose is where the middle of its rear axle is, `(x, y)` in
metres, and its heading `θ` (radians, 0 along x, growing
anticlockwise). `δ` is the angle of the front wheels of the single-track
model, `ρ = -k δ` that of the rear ones (`k` = `rear_steer` over the
most `δ`; 0 without), `L` the wheelbase. In a step the vehicle goes
`ds = v dt` along a circle:

```
dθ = ds cos ρ (tan δ - tan ρ) / L
x += ds cos(θ + ρ + dθ/2) (1 - dθ²/24)
y += ds sin(θ + ρ + dθ/2) (1 - dθ²/24)
θ += dθ
```

(the chord of the arc: its direction exactly, its length to the fourth
order of `dθ`, which is below 0.01 in a step), so a vehicle held at one
steering angle stays on its circle however long it drives.

The most `δ`: the outer front wheel at full lock goes round a circle of
diameter `circle`. With the turning centre `R` beside the vehicle's
axis and `a` ahead of the rear axle (`a = R tan ρ`; 0 without rear
steering), `(R + track/2)² + (L - a)² = (circle/2)²` gives `R`, and
`tan δ = (L - a) / R`.

Steering: the wheel is turned towards what is asked at no more than
`steer_rate` (lock to lock in 1.6 s, about what an electric rack does
when the car parks itself, and a quick driver). It stays where it is
left, as a steering wheel does in a car that stands.

Speed: what is asked is a speed between -`creep` and `creep` (2.8 m/s,
10 km/h); the vehicle comes to it at no more than 1.5 m/s² speeding up
and 4 m/s² braking.

### 5.2 Shape

Seen from above, in the vehicle's own frame (x ahead from the rear
axle, y to the left):

- the **body**: the rectangle from `-rear` to `wheelbase + front` and
  `±width/2`, its corners rounded with the radius `corner` (kept as the
  rectangle smaller by `corner` all round, and `corner` taken off every
  distance to it);
- two **mirrors**: boxes that stand out to `±mirrors/2`, 0.2 m long, at
  0.75 of the wheelbase;
- four **tyres**: boxes `tyre` by `tyre_width` at the ends of the
  axles, the steered ones turned by their angle.

### 5.3 Obstacles

An obstacle is a list of edges (segments) with a height class:

| Class | For example | Touches | Seen by (§6) |
|---|---|---|---|
| `Kerb` | kerbs, wheel stops (below the ground clearance) | tyres | cameras, poorly; no ultrasonic |
| `Low` | bollards, low walls, tow bars (below the mirrors) | body | ultrasonic if wide enough; cameras, unless in the blind zone |
| `Tall` | walls, pillars, other vehicles | body and mirrors | all |
| `Over` | a lorry's tail lift, a barrier's arm (above the bumper) | body and mirrors | cameras; no ultrasonic |

`Edges` is a buffer; a scene's boxes (§9) give four each.

### 5.4 Clearance

Every step, for the vehicle: the least distance from each of its parts
to the edges that part can touch (`gap`), which of the edges is the
nearest, and whether it touches (`gap <= 0`). A vehicle that touches is
stopped where it is and counts a hit; a tyre at a kerb counts a kerb
touch and is stopped too.

An edge is brought into the vehicle's frame and held against boxes that
lie along the axes there: the distance from a segment to a box is 0
where it crosses the box, else the least of its ends' distances to the
box and the box's corners' distances to it.

### 5.5 A trailer (P8)

One axle, on a ball `hitch` behind the vehicle's rear axle (its rear
overhang and 8 cm), its own axle `drawbar` behind the ball; the body a
rectangle. Two in `vehicles.ent`'s `trailers`, sizes estimated: a box
trailer of 750 kg (drawbar 2.45 m, body 2.6 x 1.5 m) and a caravan
(3.9 m, 5.2 x 2.3 m).

The trailer's axle does not slip sideways: when the ball goes by
(dx, dy), the trailer's heading turns by what of that is across it,
over the drawbar (at the heading it has halfway through the step). So
with the vehicle's rear axle going v and turning at w, the trailer
standing `across` the vehicle (the hitch angle), that angle changes at

    w (1 + hitch cos(across) / drawbar) - v sin(across) / drawbar

Forwards it settles: on a circle of radius R at
atan(hitch / R) + asin(drawbar / sqrt(R² + hitch²)), the trailer's axle
on a circle of radius sqrt(R² + hitch² - drawbar²). Backwards it does
not: any angle grows, until the trailer is jackknifed (60 degrees
across, where its corner is at the vehicle's). A step that would
jackknife it further is not taken, as one that would touch is not; the
trailer's body touches what a body touches and its tyres kerbs. The
trailer is nothing to other vehicles' sensors or bodies (it is in
places for one vehicle only), and with one hitched what looks straight
back from the vehicle (the rear ultrasonic sensors, the rear camera)
is switched off, as the car does it.

`tow.ent` has what cars that tow have. The knob: the driver says how
far across the trailer is to stand, the car steers so that it comes to
that at 2.0 of what is missing a metre (the steer is found from the
formula above by halving, which also holds where the rear wheels
steer). The most the knob asks for is four fifths of what the vehicle
can still straighten at full lock. And backing into a slot: the way is
the line the trailer's axle is on, a quarter circle, and the slot's
middle line from a metre before its mouth; the trailer's axle is
steered at the point of the way 2 m on (the angle for that curve is
what the knob is set to); it stops with the trailer's tail 40 cm from
the slot's end. Where the trailer then stands more than 4 degrees
askew or within 15 cm of the slot's side, the car pulls forwards along
the slot's line for up to 8 m and backs in again along the line alone,
twice at most. It goes by where the car believes it is (§7.1) and the
hitch angle, taken as measured without error. It does not look where
it goes: the driver does. The parker of §8 does not start with a
trailer (`Why.Hitched`).

## 6. Sensors

A sensor is a row of the suite the vehicle carries: its kind, where it
is mounted in the vehicle's frame, which way it looks, how wide, from
how near to how far, how often. What it reports is worked out from the
truth, with errors drawn from the seed, the sensor and the step (so a
run is the same on one core and on all).

### 6.1 Ultrasonic (the Model 3 until October 2022: 12 of them)

Six in each bumper. A sensor reports **one range**: that of the nearest
edge inside its cone that it can hear, not where in the cone it is.

- cone ±35° across; 0.25 m to 2.5 m (the two at the corners of each
  bumper, which look to the side and measure a gap while driving past
  it: to 4.5 m); 15 Hz;
- nearer than 0.25 m it reports "stop" and no range (the chime's last
  band);
- range in steps of 1 cm with a noise of 1 cm;
- hears `Low` and `Tall`, not `Kerb` (below the cone) nor `Over` (above
  it);
- a flat face sends the echo back where the sensor stands straight
  before it: the foot of the perpendicular from the sensor is on the
  edge and in the cone. A face seen aslant sends it elsewhere and is
  not heard, but for its ends (where they are in the cone);
- an edge shorter than 7.5 cm (a thin post's) is heard only below 1 m.

The belief places an arc at the range (the car's display draws just
that).

### 6.2 Cameras (Tesla Vision)

| Camera | Where | Looks | Across | To |
|---|---|---|---|---|
| front main, front wide | top of the windscreen | ahead | 50°, 120° | 150 m, 60 m |
| B-pillar, left and right | the pillars | sideways and ahead | 90° | 80 m |
| repeater, left and right | the front wings | back along the side | 75° | 100 m |
| rear | above the number plate | back | 140° | 50 m |
| bumper (cars from late 2025) | front bumper | ahead and down | 120° | 10 m |

(Across and to: the numbers Tesla published for the cameras of
Autopilot hardware 2 to 3; hardware 4's are wider. Where exactly they
sit is measured from pictures and marked as an estimate in
`vehicles.ent`.)

What matters for parking is what they do **not** see:

- **The blind zone.** The windscreen cameras sit 1.4 m up and 1.9 m
  behind the front bumper, and look over the bonnet: the ground nearer
  than about 2 m ahead of the bumper is hidden, and a thing of height
  `h` is hidden when it is nearer than where the line from the camera
  over the bonnet's edge comes down to `h`: with the camera `up` high
  and the edge `hide` away and `lip` high, nearer than
  `hide (up - h) / (up - lip)`. With 1.4, 1.9 and 0.7 m a `Low` thing
  (taken as 0.5 m high) vanishes 0.54 m ahead of the bumper and a `Kerb`
  (0.12 m) 1.6 m ahead of it.
  The same for the sides close to the doors below the B-pillar and
  repeater cameras, and for the rear corners beside the rear camera.
  With the bumper camera the front zone is seen.
- **Depth.** The range of a seen point has an error that grows with
  the range (3 % of it, at least 3 cm), more in the dark (a setting of
  the scene), and a bias that stays for a while (it does not average
  out over frames).
- **Plain surfaces.** An edge marked `plain` (a white wall, glass, a
  dark garage's back wall) is seen only at its ends and where something
  else meets it, with three times the error.
- **Things hidden behind things**: a ray reports the nearest thing it
  meets and the nearest `Tall` one, and nothing behind that.

A camera is a ray every 2°, 30 times a second; what it reports are
points in the vehicle's frame.

## 7. The belief

### 7.1 Odometry

The vehicle does not know its pose; it starts from the true one and
adds up what it measures. How far it went: the edges its rear wheels'
encoders count (`ticks` a turn, 2 cm each), each wheel's rolling
circumference off by a ratio drawn per run (±0.5 %, tyre pressure and
wear). How far it turned: a gyroscope whose scale is off by a ratio
drawn per run (±1 %) and that drifts while the vehicle drives (up to
0.05°/s). So the believed pose goes off the true one by a part of how
far it drove and turned. Without errors it is the true pose, to the
bit. (Built so; the steering angle's offset is not modelled, as the
heading is the gyroscope's.)

### 7.2 The grid

An occupancy grid of 5 cm cells, 40 m square, fixed in the believed
world frame. A cell holds how sure the vehicle is that something is
there, how high it is thought to be, and **when it was last seen**.

- A sensor reading raises the cells where it puts something and lowers
  those the ray went through before.
- A cell that no sensor can see now **keeps what it held**: that is
  Tesla Vision's memory of what is in the blind zone. It was placed
  with the believed pose of then, so it is off by the odometry's drift
  since, and by the depth error of the sighting it came from (the last
  good one was usually from far, where the error is largest).
- A thing that moved or came after it was last seen (§9, moving things)
  is not in the grid at all.

The distance shown to the driver, and the one the parker brakes for, is
the vehicle's shape against the grid.

### 7.3 What is drawn

One pane, and V chooses what is in it (two panes side by side are too
small in a narrow window):

- the truth, and what the sensors report now (arcs and points);
- the truth, and over it the grid's cells the vehicle is sure of, in
  the colour of the height it took the thing to have, pink where it has
  not seen them for two seconds and goes by what it remembers; the
  believed pose as a yellow outline round the true vehicle; how far the
  believed pose is off;
- the belief alone, the vehicle drawn where it believes it is: to park
  by, as by the car's display.

A number each: the true clearance and what the car reads (its body, by
the believed pose, against the grid), and the difference.

## 8. The parker

- By hand: the arrow keys (§11).
- Automatic, from the belief only: finds the slot (a gap long and deep
  enough in the grid), plans a path of arcs and straights (two arcs for
  parallel, one for a bay; Reeds–Shepp where that fails; hybrid A* over
  the grid for the tight scenes), follows it with the steering rate in
  mind, stops for anything nearer than a margin, and plans again when
  the belief changes. Gives up and says so, rather than touch.

Built (P5, `parker.ent`): the slot is given (the scene's; in the car
the driver taps it), and the way is hybrid A* from the start, with no
arcs or Reeds–Shepp before it: over the grid's distance field, the body
as discs, arcs of 30 cm at full lock or straight, poses kept apart by
15 cm and 5 degrees, 90,000 poses a search at most (a second). It ends
in a corridor (the poses along the slot's axis that touch nothing), in
which a steering law settles the vehicle, to and fro where the gap is
short. It searches again at every change of direction and when
something is nearer than 7 cm and coming nearer. It keeps 6 cm from
what it believes is there. It gives up where it finds no way, where it
is stopped by what it did not see (so it has touched it: the plain
walls of the dark garage, which no camera sees), and at once without
cameras: ultrasonic sensors hear nothing beyond 2.5 m, so the slot is
unknown until the vehicle is at it, and nothing nearer than 25 cm.

Built (P6): what moves is rows of `movers` in `scenes.ent` (someone
walking, a trolley, a door, cars), each a box that goes its way from
when the vehicle first moves and stands while the vehicle is close
before it. The sensors see them as they see everything; the grid is
bad at them: a thing that moves is in no cell for long, and where it
was the cells stay until a ray goes through them. So

- the grid lets go of cells that held something for less than 2 s and
  have not been seen for 2 s, also between two rays (`believe.ent`);
- the parker looks ten times a second at the next 3 m of its way
  (`watch`): it stands while the cameras see something within 2.5 m of
  it where the grid has held nothing for 2 s (it moves, or has just
  come), for 4 s at most, and searches again where the grid has
  something in the way for 2 s;
- where a search finds no way it waits 2 s and searches again, six
  times, before it gives up.

Places 10 and 11 of the demo are the spec's 11 and 12.

## 9. Scenes

A scene is a table of boxes (middle, heading, length, width, height
class, plain or not), a start pose, a slot (the pose to end in and how
far off it may be), and whether it is dark.

| # | Scene | What it shows |
|---|---|---|
| 1 | Parallel, between two cars at a kerb, gap 6.2 m | the basic manoeuvre; the smart drives in forwards, the van cannot fit |
| 2 | Bay, between two cars, a wall behind, aisle 6 m | forwards and backwards; the swing of the front when backing in |
| 3 | Garage, door 2.4 m, a pillar inside | the mirrors decide; the van's do not pass |
| 4 | Dead end, 7 m wide | turning round in so many moves |
| 5 | Parallel, gap sweeping from length + 1.2 m down to + 0.5 m | how tight is too tight, per vehicle and sensor suite |
| 6 | Bollard: a `Low` post ahead in a bay | in the blind zone at the end; remembered or not |
| 7 | Kerb and wheel stop | tyres touch, bumper passes over; cameras place it poorly |
| 8 | Tail lift: an `Over` edge behind | ultrasonic misses it |
| 9 | White wall in a dark garage | `plain`: depth is poor |
| 10 | Charger: back up to a post so the port lines up | the last half metre is blind at the rear corner |
| 11 | Someone walks behind; a trolley rolls in; a door opens | things that were not there when the cells were last seen |
| 12 | Backing out of a bay with cross traffic | what is hidden by the neighbours |
| 13 | A car park of many vehicles parking at once | scale (many cores) |
| 14 | A yard: a trailer to back into a bay 3.4 m wide (P8) | the hitch angle; why backing with a trailer is hard |

Built (P7): every vehicle has its own. Until P6 the belief, the grid,
the search and the parker were one of each, the first vehicle's; now
`Belief` and `Parker` are components of the vehicle, and the grid, the
field, the search's nodes, heap and costs, the way and the rays are
buffers of which every vehicle has rows of its own, by windows (`cells:
mut Cells[c.number * GRID, GRID]`): 58 MB a vehicle. What a vehicle
must not touch and what it senses are the edges of its place (`Place`:
rows of `Edges`), and a vehicle that drives among others is four edges
to them (`bodies()`), which its own sensors and its own clearance leave
out. Where it is to stand is its `Goal`. A vehicle that is done, or not
in the place, sleeps (`Eyes.asleep`): it senses nothing more.

- Scene 13 is `lot.ent`: aisles of 6 m with bays of 2.5 m on both
  sides, every bay taken but those the vehicles are to park in; in each
  aisle three vehicles (Model 3s of both kinds and a smart) 12 m one
  behind another, each to park in a bay 7 m ahead of where it starts,
  on one side and the other in turn. An aisle is a place: its vehicles
  see each other and wait for each other as they do for what moves in
  scenes 11 and 12. It is place 13 of the demo, with two aisles: A
  starts all six, Tab chooses whose belief is shown.
- Scene 5 is `check/sweep.ent`: not a place of the demo but sixty
  streets at once, a vehicle and a gap each.

How tight is too tight (the sweep, seed 2026, errors on; the gap is so
much longer than the vehicle):

| Vehicle | Parks in gaps down to | Shorter than that |
|---|---|---|
| Tesla Model 3 (2017–2023), cameras | +1.1 m | finds no way in |
| Tesla Model 3 (2024), cameras | +1.2 m | finds no way in |
| Tesla Model 3 (2024), with the bumper camera | +1.4 m | at +1.2 and +1.1 m it is in the gap, askew, with no room to straighten |
| smart fortwo | +1.0 m | finds no way in |
| Mercedes EQS | none | finds no way in, in any gap up to +1.6 m |
| Mercedes Sprinter | none | at +1.6 m it believes itself parked and is over the slot's line; else no way, or no room (the slot is 18 cm wider than it is) |

At +0.6 and +0.5 m every parker says the grid leaves no room in the
slot: what the cameras put there of the two cars, and the 6 cm it
keeps to each, are more than what is left. None of the sixty touches
anything. These are this parker's limits, not the vehicles': someone
who drives well parks a Model 3 in its length and 0.8 m, to and fro.
The search goes in arcs of 30 cm and gives up after 90,000 poses; the
bumper camera does not make it better (the three Model 3 rows differ
by what the errors happened to be).

## 10. Checks

Each a script or test that names its number.

1. **Motion.**
   1. A vehicle held at a steering angle and a speed stays on the
      circle the formula gives, for every vehicle, forwards and
      backwards: within 0.01 mm of it after 80 m, and within 0.00001
      rad of where on it it should be.
   2. At full lock the outer front wheel's path has the diameter
      `circle`, within 1 mm.
   3. On one core and on all, the same bits.
2. **Clearance.** Against boxes placed where the distance is known by
   hand: beside, ahead, at a corner (the rounding), at a mirror, under
   it (`Low` passes a mirror), a kerb at a tyre. A drive into a wall
   stops within one step's travel of it.
3. **Sensors.** Without errors, against things at known ranges: a wall
   in an ultrasonic cone, too near, a thin post far and near, a kerb,
   a thing that hangs over; for the cameras a wall, a low wall and a
   kerb in and out of the blind zone, with and without the bumper
   camera, a thing behind a wall, a plain wall. The same on one core
   and on all.
4. **Belief.** A drive up to 30 cm from a low wall that no camera sees
   at the end: the grid still holds it. With every error set to 0 the
   believed pose is the true one to the bit and the distance read off
   the grid is within a centimetre of the true one. With errors the
   pose is off by less than 1.5 percent of the way and the reading by
   less than 15 cm. The same on one core and on all.
5. **Parker.** Every scene it should manage, for every vehicle and
   suite, ends in the slot with no touch against the truth; those it
   cannot, it gives up on. Same result on one core and on all.
   Built: `check/autopark.ent`, a Model 3 (2024) with Tesla's cameras
   and their errors in the eleven places before the yard (since P8 also the yard, without a trailer: parked): parked in ten with no touch
   in 44 to 77 s; in the dark garage it finds no way it believes in,
   or drives into a plain wall, and gives up. By hand, for eight
   seeds over both camera suites: 77 of the 80 runs outside the dark
   garage parked; it gave up twice in the garage (once after its
   mirror touched the door's post: the believed place was 13 cm off
   after a minute of to and fro) and once in place 10. The smart
   parks in all eleven. The Model 3 of 2017 gives up in place 10, the
   EQS and the Sprinter where they do not fit and in some places
   where they would. Not kept: "rather than touch" for the Sprinter
   and, now and then, the Model 3 at the garage's door, and wherever
   a plain wall stands in the dark.
   Built (P7): `check/carpark.ent`, six vehicles at once in two aisles:
   all park, none touches; on one core (39 s) and on six threads
   (20 s) the same lines. With 24 vehicles in eight aisles 23 park and
   one, a Model 3, touches once and gives up; one core 169 s, all
   cores (24 threads) 36 s, the same lines. `check/sweep.ent`, sixty
   at once: 3 minutes on all cores.
6. **Trailer** (P8). Forwards at half lock for three minutes, three
   vehicles with each trailer: the hitch angle is the formula's within
   10 millionths of a radian and the trailer's axle on its circle
   within 0.1 mm. Backwards with the wheels straight from one degree
   across: jackknifed at 60 degrees after 10.2 m (box trailer) and
   16.3 m (caravan), stopped; forwards again it comes straight. With
   the knob at its middle from 20 degrees across, and halfway: the
   angle is what was asked for within a hundredth of a degree after
   90 m backwards (also the EQS, whose rear wheels steer). Backed at a
   wall, the trailer stops within a millimetre of it. The same lines
   on one core and on six threads. `check/trailer.ent`.
7. **Backing in** (P8). Each of the five vehicles with each trailer
   backs it into the yard's bay, by the believed pose with its errors:
   all ten trailers stand in the slot, nothing is touched, in 25 to
   30 s; the EQS with the box trailer (a long vehicle before a short
   trailer) ends askew, pulls forwards once and is in after 60 s. By
   hand, six more seeds: ten of ten each. The parker asked to park with
   a trailer gives up at once. `check/backin.ent`.

## 11. The demo's keys

Up/down: the speed asked for, forwards and backwards. Left/right: turn
the steering wheel. Space: brake. 1–5: the vehicle. S: the sensor
suite. N/P: the scene. R: start the scene again. A: the automatic
parker on and off. T: the truth pane on and off (off: park by the
display alone, as in the car). E: every error on and off.

(Built: T is the swept path; V goes through the truth with the
sensors' reports, the truth with the belief, and the belief alone; S
also has "none". A starts the parker and stops it; so does any key
that drives. Its way is drawn as dots, green forwards and orange
backwards. In the car park A starts and stops all the vehicles, Tab
chooses the one whose belief, sensors, way and wheel are shown and
which the arrows drive. H hitches a trailer (box trailer, caravan,
none). B: the backing assist's knob, which left, right and C then
turn; a blue line shows where the trailer is to come to. With a
trailer A backs it into the slot.)

## 12. Milestones

Built: P0 to P8 (2026-11-11). The places of P4 are 5 to 9 in
`scenes.ent` (the table above counts the sweep as 5, which is P7's).

| | What | Checks |
|---|---|---|
| P0 | `vehicles.ent`, `park.ent`: motion, shape, edges, clearance | 1, 2 |
| P1 | `scenes.ent` 1–4, `show.ent`: drive by hand, switch vehicle and scene | |
| P2 | `sense.ent`: ultrasonic and cameras, drawn as what they report | 3 |
| P3 | `believe.ent`: odometry, grid, remembering; both panes | 4 |
| P4 | Scenes 6–10 (the sensor traps) | |
| P5 | `parker.ent`: arcs, then Reeds–Shepp, then hybrid A* | 5 |
| P6 | Scenes 11–12: things that move (built: places 10 and 11) | 5 |
| P7 | Scene 5 as a sweep, scene 13 (built: `check/sweep.ent`, `lot.ent`, place 13) | 5 |
| P8 | A trailer (built: §5.5, `tow.ent`, place 12, `check/trailer.ent`, `check/backin.ent`) | 6, 7 |
