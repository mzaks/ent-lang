#!/usr/bin/env python3
"""Measures examples/swarm/swarm.ent: the game played by itself.

  python3 bench/swarm/run.py [frames] [--parallel] [--laps]
                             [--shot file.png]

A copy of the game is made that takes steps of a sixtieth of a second
however long a frame takes, starts itself, moves the hero round and aims
round, and from frame 600 on runs the stress (the field filled, rings of
shots). Every 120 frames it says what there is and how long a frame took
on average: all of it, and of that what is computed (the systems before
the drawing) and what is drawn. With --laps it says at the end how long
each system of the frame took, on average over the last 120 frames. It
needs a window, as the game does.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

AUTO = '''
unique Probe { start: f64, computed: f64, drawn: f64, sum_all: f64,
               sum_computed: f64, sum_drawn: f64, last: f64 }
system auto() {
  // Starts itself, walks a square, aims round; then the stress.
  Keys.enter = Window.frame == 3
  let leg = (Window.frame / 240) % 4
  Keys.right = leg == 0
  Keys.down = leg == 1
  Keys.left = leg == 2
  Keys.up = leg == 3
  let turn = Window.frame as f32 * 0.021
  Mouse.x = Window.width as f32 * 0.5 + cos(turn) * 200.0
  Mouse.y = Window.height as f32 * 0.5 + sin(turn) * 200.0
  if Window.frame == STRESS && Game.state == State.Playing { Game.stress = true }
  Probe.start = wait_until(0.0)
}
system computed() { Probe.computed = wait_until(0.0) }
system drawn() {
  Probe.drawn = wait_until(0.0)
  if Probe.last > 0.0 { Probe.sum_all += Probe.start - Probe.last }
  Probe.last = Probe.start
  Probe.sum_computed += Probe.computed - Probe.start
  Probe.sum_drawn += Probe.drawn - Probe.computed
  if Window.frame == SHOT_AT { screenshot("SHOT") }
  if Window.frame % 120 == 0 && Window.frame > 0 {
    let all = Count.foes + Count.shots + Count.bolts + Count.gems + Count.sparks
    put("frame {Window.frame}: {all} entities: foes {Count.foes} (close {Count.close}), shots {Count.shots}, bolts {Count.bolts}, gems {Count.gems}, sparks {Count.sparks}")
    put("  level {Hero.level + 1}, hp {Hero.hp as i32}, fallen {Game.kills}; {(Probe.sum_all / 120.0 * 1000000.0) as i32} us a frame: computing {(Probe.sum_computed / 120.0 * 1000000.0) as i32}, drawing {(Probe.sum_drawn / 120.0 * 1000000.0) as i32}")
    Probe.sum_all = 0.0
    Probe.sum_computed = 0.0
    Probe.sum_drawn = 0.0
  }
}
'''


def with_laps(game, frames):
    """After every system of the frame, one that notes the time: what
    each took is said at the end."""
    head, _, rest = game.partition("schedule frame() {\n")
    body, _, tail = rest.partition("}\n")
    runs = [line for line in body.splitlines() if line.strip()]
    names = [line.split("(")[0].strip() for line in runs]
    fields = ", ".join(f"s{index}: f64" for index in range(len(runs)))
    laps = f"unique Laps {{ last: f64, {fields} }}\n"
    for index in range(len(runs)):
        laps += (f"system lap_{index}() {{\n  let now = wait_until(0.0)\n"
                 f"  if Window.frame > {frames - 121} {{ Laps.s{index} += "
                 f"now - Laps.last }}\n  Laps.last = now\n")
        if index == len(runs) - 1:
            laps += f"  if Window.frame == {frames - 1} {{\n"
            for at, name in enumerate(names):
                laps += (f'    put("  {name:<8} {{(Laps.s{at} / 120.0 * '
                         f'1000000.0) as i32}} us")\n')
            laps += "  }\n"
        laps += "}\n"
    body = "".join(f"{line}\n  lap_{index}()\n"
                   for index, line in enumerate(runs))
    return head + laps + "schedule frame() {\n" + body + "}\n" + tail


def main():
    arguments = sys.argv[1:]
    parallel = "--parallel" in arguments
    shot = ""
    if "--shot" in arguments:
        shot = os.path.abspath(arguments[arguments.index("--shot") + 1])
    numbers = [a for a in arguments if a.isdigit()]
    frames = int(numbers[0]) if numbers else 1800
    with open(os.path.join(ROOT, "examples/swarm/swarm.ent")) as source:
        game = source.read()

    def swap(old, new, times=None):
        nonlocal game
        if old not in game or (times and game.count(old) != times):
            sys.exit(f"bench/swarm/run.py: the game has not '{old}' as "
                     "this script expects it")
        game = game.replace(old, new)

    swap("min(Window.dt, 0.05)", "0.016667")
    swap("Window.fps = 60", "Window.fps = 0", 1)
    swap("import math\n", "import math\nimport clock\nimport console\n", 1)
    swap("schedule frame() {", AUTO.replace("STRESS", "600")
         .replace("SHOT_AT", str(frames - 2) if shot else "-1")
         .replace("SHOT", "swarm_bench_shot.png") + "schedule frame() {", 1)
    if "--laps" in arguments:
        game = with_laps(game, frames)
    swap("  begin()\n", "  begin()\n  auto()\n", 1)
    swap("  tune()\n", "  tune()\n  computed()\n", 1)
    swap("  hud()\n", "  hud()\n  drawn()\n", 1)
    swap("until Window.closed", f"until Window.frame >= {frames}", 1)
    # (Next to the game, so that it finds the game's music.)
    with tempfile.NamedTemporaryFile(
            "w", suffix=".ent", prefix="swarm_bench_", delete=False,
            dir=os.path.join(ROOT, "examples/swarm")) as copy:
        copy.write(game)
    try:
        environment = dict(os.environ)
        environment.setdefault("ENT_CFLAGS", "-march=native")
        command = [os.path.join(ROOT, "tools/ent"), "run", "--no-cache",
                   "-I", os.path.join(ROOT, "examples")]
        if parallel:
            command.append("--parallel")
        done = subprocess.run(command + [copy.name], env=environment)
        # (The window writes a picture where the program runs.)
        if shot and os.path.isfile("swarm_bench_shot.png"):
            os.replace("swarm_bench_shot.png", shot)
        return done.returncode
    finally:
        os.remove(copy.name)


if __name__ == "__main__":
    sys.exit(main())
