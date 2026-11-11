# Parking a vehicle in a tight spot, in ent-lang

One vehicle, driven slowly among things it must not touch: a kinematic
model of how a car goes where its wheels point, real vehicles' numbers,
and how near the body, the mirrors and the tyres are to the nearest
thing. The plan is in `parking-SPEC.md`: next come the vehicle's own
sensors (ultrasonic, and Tesla Vision's cameras with their blind zone),
what it believes of the place from them, shown beside the truth, and a
parker that drives by that belief alone. Built: all its milestones, P0
to P8.

    tools/ent run examples/parking/show.ent

- `vehicles.ent`: the vehicles, a row each: a Tesla Model 3 of 2017 to
  2023 and of 2024, a smart fortwo, a Mercedes EQS with rear-axle
  steering and a long Mercedes Sprinter. Length, width with and without
  mirrors, wheelbase, overhangs, track and turning circle, from the
  owner's manuals and specification sites; the file says which numbers
  are estimates.
- `park.ent`: the model. A vehicle's pose is its rear axle's middle and
  its heading; a step is an arc by the angle of its front wheels (and of
  its rear ones, where they steer). How far the front wheels steer is
  worked out from the turning circle. What is asked of it is a speed and
  the steering wheel's place, which it comes to as fast as a car can.
  `Edges` are what it must not touch, each of a height: a kerb touches
  only tyres, a low thing the body, a tall one the mirrors too. A step
  that would touch is not taken.
- `scenes.ent`: twelve places for one vehicle, as tables of boxes: a gap of 6.2 m at a
  kerb, a bay between two cars, a garage with a door of 2.4 m, a dead
  end to turn round in; and five made to fool the sensors: a bollard in
  the corner of a bay (under the bonnet before the car is at it), a
  wheel stop (too low for ultrasonic, out of the cameras' sight 1.6 m
  ahead), a lorry's tail lift over the end of a gap (above what
  ultrasonic hears), white walls in a dark garage (nothing for a camera
  to hold on to but their ends) and a charger's post to back up to; and
  two where things move (`movers`): a bay where someone walks behind
  the vehicle as it backs in, a trolley rolls across and a neighbour's
  door opens, and a bay to back out of between two vans, with cars
  passing in both lanes; and a yard with a bay to back a trailer
  into. And whether the vehicle (or its trailer) is parked in the
  slot.
- `sense.ent`: the sensors, a row each, in three suites: the twelve
  ultrasonic sensors of a Model 3 until 2022, Tesla's cameras, and those
  with the bumper camera of late 2025. An ultrasonic sensor reports one
  range, of what sends its echo back (a face straight before it, an
  end; not a kerb, not what hangs over, a thin post only from near); a
  camera is rays that report points, and does not see over the car's
  own body what is low and near: a kerb vanishes 1.6 m ahead of the
  bumper, a bollard half a metre. With errors drawn from a seed.
- `believe.ent`: what the vehicle believes. Where it is: from the
  edges its wheel encoders count and a gyroscope, each a little off, so
  the believed pose drifts from the true one. What is around it: a grid
  of 5 cm cells that all the sensors' reports are put into by the pose
  believed then; a ray makes the cells it passes less sure, but not
  where it could not have seen what they hold, so what has gone into
  the blind zone stays as it was last seen. How near the body is to the
  grid is what the car reads.
- `parker.ent`: the car parks itself, by what it believes alone (the
  believed pose and the grid, never the truth). It finds the corridor
  of the slot, searches for a way into it (hybrid A* over arcs of 30 cm
  at full lock or straight), follows it, searching again at every
  change of direction with what it has seen since, and settles in the
  slot, to and fro where the gap is short. It gives up where it finds
  no way, where it is stopped by something it did not see, and without
  cameras. It stands while the cameras see something near its way
  that the grid did not hold 2 s ago (someone walking, a car), and
  waits and searches again where something is in its way.
- `lot.ent`: a car park of many vehicles that park at once: aisles
  with bays on both sides, all taken but those the vehicles are to
  park in, three vehicles driving in each aisle. Every vehicle has a
  belief, a grid, a search and a parker of its own (rows of its own
  of the buffers, by windows: 58 MB a vehicle), and the vehicles of an
  aisle are things to each other. Place 13 of the window, with six.
- `tow.ent`: a trailer (a box trailer or a caravan, in `vehicles.ent`;
  how it follows the hitch ball, jackknifes and touches things is in
  `park.ent`). Backwards a trailer does not stay behind the vehicle;
  two helps, as cars that tow have them: a knob that says how far
  across the trailer is to stand, the car steering so that it does;
  and backing the trailer into a slot along a way of a line, a quarter
  circle and the slot's line, pulling forwards to straighten it where
  it ends askew. It goes by the believed pose and does not look where
  it goes. The parker does not park with a trailer.
- `show.ent`: the window. A: the car parks itself (its way is drawn
  as dots), until A again or a key that drives. V chooses what is
  shown: the truth with the sensors' reports, the truth with the
  belief over it, or the belief alone. What moves starts when the
  vehicle first does. In the car park Tab chooses the vehicle whose
  belief, sensors and wheel are shown. H hitches a trailer, B turns
  the backing assist's knob on (left and right then turn it), and
  with a trailer A backs it into the slot (place 12 is a yard for
  that). The arrows drive and steer, 1
  to 5 choose the
  vehicle, N and P the place, S the sensors, whose reports are drawn
  over the place, with the least distance they make of it beside the
  true one. The steering wheel and the wheels from above are drawn as
  they are turned.
- `check/circle.ent`, `check/circle.py`: every vehicle held at a
  steering angle and a speed stays on the circle the formula gives,
  within 0.01 mm after 80 m, and at full lock its outer front wheel goes
  round the turning circle its maker states, within 1 mm; the same bits
  on one core and on all.
- `check/clear.ent`: a Model 3 beside walls, at a point off its rounded
  corner, under a low wall with its mirror and with its tyres at a kerb,
  at distances known by hand, to a tenth of a millimetre; and one that
  drives at a wall and stops short of it.

- `check/sensors.ent`: what the sensors report of thirteen things at
  ranges known by hand, to the millimetre.

- `check/belief.ent`: a drive up to a low wall that the cameras lose
  sight of: the grid keeps it, and reads its distance within a
  centimetre without errors.

- `check/autopark.ent`: the parker in all twelve places, a Model 3
  (2024) with Tesla's cameras and their errors: parked in eleven without
  a touch; in the dark garage with plain white walls no camera sees a
  wall, and it gives up.

- `check/places.ent`: every place is made, and a vehicle at its start
  touches nothing.

- `check/carpark.ent`: six vehicles park at once in two aisles: all
  park, none touches anything; the same lines on one core (39 s) and
  on six threads (20 s). With 24 in eight aisles (`Lot.aisles = 8`,
  24 rows of `Fleet`), one core takes 169 s and 24 threads 36 s; 23
  park, and one touches a parked car once and gives up.

- `check/trailer.ent`: a trailer forwards round a circle stands across
  the vehicle by what the formula gives, within 10 millionths of a
  radian, its axle on its circle within 0.1 mm; backwards with the
  wheels straight it is jackknifed after 10 m (the caravan: 16 m) from
  one degree across; with the knob it comes to the angle asked for,
  within a hundredth of a degree, and stays for 90 m; backed at a
  wall it stops short. The same lines on one core and on six threads.

- `check/backin.ent`: every vehicle with each trailer backs it into
  the yard's bay, with the errors of its odometry: all ten stand in
  the slot, nothing touched, in half a minute (the EQS with the box
  trailer pulls forwards once to straighten it: a minute).

- `check/sweep.ent`: how tight is too tight: sixty vehicles at once,
  each in a street of its own, the gap at the kerb from 1.6 m longer
  than the vehicle down to 0.5 m. A Model 3 parks in gaps down to its
  length and 1.1 to 1.4 m (by what its cameras' errors happen to be),
  the smart down to 1.0 m; the EQS and the Sprinter in none. It needs
  3.5 GB and three minutes on all cores, and is not a test:

      ENT_PARALLEL_MIN=1 tools/ent run --parallel -I examples/parking \
        examples/parking/check/sweep.ent

  (`ENT_PARALLEL_MIN=1`: loops over few entities run on many threads
  too; with fewer vehicles than threads, `OMP_NUM_THREADS` as many as
  there are vehicles is faster.)

The tests `parking_circle`, `parking_clear`, `parking_sensors`,
`parking_belief`, `parking_autopark`, `parking_carpark`,
`parking_trailer`, `parking_backin`, `parking_places` and `parking_show` (in `test/Integration`) run the
checks and compile the window and the sweep. `parking_autopark` and
`parking_carpark` take about a minute each.
