#!/usr/bin/env python3
"""Makes the tour of ent map, build/map-video/ent-map-tour.mp4 (and its
captions, ent-map-tour.srt), from the script below.

The script is segments, each a map of an example program and beats: a
sentence or two said (macOS's `say`) and shown as a caption, and what is
done while it is said, at seconds from the beat's start. A beat lasts as
long as its words, or as its `hold` where the map needs longer. From it
come the timed part of the driver (what is done when, the captions, the
frames round what is talked about), which goes with driver.ent.part into a
copy of tools/map/ent_map.ent; that is built and run on the segment's map,
and records itself (video.c, through ffmpeg); then the segments are put
one after the other and the words under the picture.

The maps are made first, of the examples as they are:
- beecs (examples/beecs), traced over its year: examples/beecs/reference/
  fetch.sh has fetched its weather into build/beecs/run (it needs Go and
  the network; make.py runs it where that is not done);
- melt (examples/lj), built --parallel and traced.

    python3 tools/map/video/make.py

Needs ffmpeg (brew install ffmpeg) and macOS. The window opens for the
length of each segment: leave the mouse and keyboard alone while it plays.
"""
import os
import shutil
import subprocess
import sys

VOICE = os.environ.get("VIDEO_VOICE", "Samantha")
RATE = os.environ.get("VIDEO_RATE", "178")
FPS = 30
START = 1.2  # seconds before the first beat of a segment: the window settles

GLIDE = 0.55
CLICK_PRESS = 0.62   # the glide, and a moment's rest
CLICK_EACH = 0.13
TYPE_EACH = 0.11
DRAG = 1.2

# Keys: (characters, Mac key code, shown as).
KEYS = {
    "escape": ("", 53, "Esc"),
    "backspace": ("", 51, "Backspace"),
    "a": ("a", 0, "A"),
}


def ent_string(t):
    """An ent text literal of t: \\ " { } escaped."""
    return '"' + (t.replace("\\", "\\\\").replace('"', '\\"')
                  .replace("{", "\\{").replace("}", "\\}")) + '"'


# ---- Where things are: targets, as ent that works out (tx, ty) from the
# map's own state. A target is a tuple: its kind, then what it needs.

def at(kind, *rest):
    return (kind,) + rest


def target_code(target):
    """ent statements that set the locals tx, ty (-1: not found)."""
    kind = target[0]
    if kind == "tab":
        # (Lifecycle 0, Systems × data 1, Schedule 2, Cores 3, Provenance 4,
        # Time 5, Memory 6.)
        return (f"tx = Window.width as f32 - 980.0 + {target[1]}.0 * 130.0 + 61.0\n"
                f"  ty = 24.0")
    if kind == "xy":
        return f"tx = {target[1]}\n  ty = {target[2]}"
    if kind == "area":
        # A place in the part right of the list: fractions of it.
        return (f"tx = panel() + (Window.width as f32 - panel()) * {target[1]}\n"
                f"  ty = top() + (Window.height as f32 - top()) * {target[2]}")
    if kind == "row":
        # A row of the list at the left (0: All).
        return f"tx = 90.0\n  ty = top() + 30.0 + ({target[1]}.0 + 0.5) * row_height()"
    if kind == "every":
        return "tx = 40.0\n  ty = Window.height as f32 - 18.0"
    if kind == "schedule":
        # A schedule of the list at the left, by its name.
        return (f"Pick.a = -1\n"
                f"  for c: Sch where c.name == {ent_string(target[1])} {{ Pick.a max= c.row }}\n"
                f"  if Pick.a >= 0 {{\n    tx = 90.0\n"
                f"    ty = top() + 30.0 + (Pick.a as f32 + 0.5) * row_height()\n  }}")
    if kind == "cell":
        # A cell of the wave: in matrix k, a change of that kind (its place
        # on the matrix's two axes, as one number).
        k, kind_of = target[1], target[2]
        return (f"let n = View.n + 2\n  let total = View.n\n  Pick.a = -1\n"
                f"  for f: Fl where f.col == {k} && find(f.kind, {ent_string(kind_of)}, 0) == 0 {{\n"
                f"    let a = axis(f.from, StateRow[max(f.from, 0)].slot, total)\n"
                f"    let b = axis(f.to, StateRow[max(f.to, 0)].slot, total)\n"
                f"    Pick.a max= WaveAt[{k} * n + a].pos * 4096 + WaveAt[({k} + 1) * n + b].pos\n"
                f"  }}\n"
                f"  if Pick.a >= 0 && {k} < len(WaveCol) {{\n"
                f"    let pa = Pick.a / 4096\n    let pb = Pick.a % 4096\n"
                f"    let cx = if {k} % 2 == 0 {{ pb }} else {{ pa }}\n"
                f"    let cy = if {k} % 2 == 0 {{ pa }} else {{ pb }}\n"
                f"    let u = WaveCol[{k}].x + cx as f32 + 0.5\n"
                f"    let v = WaveCol[{k}].y + cy as f32 + 0.5\n"
                f"    tx = View.ox + (u + v) * View.cell * 0.70710678\n"
                f"    ty = View.oy + (v - u) * View.cell * 0.70710678\n  }}")
    if kind == "grid":
        # A cell of Systems × data: the first run shown of a system whose
        # name has these words, at the last column it writes (else any).
        name = ent_string(target[1])
        return (f"Pick.a = 1000000000\n"
                f"  for w: Rw where w.shown >= 0 && find(w.system, {name}, 0) >= 0 {{ Pick.a min= w.index }}\n"
                f"  if Pick.a < 1000000000 {{\n"
                f"    let index = Pick.a\n    let r = RowAt[index].shown\n    Pick.b = -1\n"
                f"    for e: Ce where e.row == index && ColAt[e.col].shown >= 0 {{ Pick.b max= ColAt[e.col].shown }}\n"
                f"    Pick.c = -1\n"
                f"    for e: Ce where e.row == index && len(e.writes) > 0 && ColAt[e.col].shown >= 0 {{ Pick.c max= ColAt[e.col].shown }}\n"
                f"    let c = if Pick.c >= 0 {{ Pick.c }} else {{ Pick.b }}\n"
                f"    if c >= 0 {{\n"
                f"      tx = Grid.ox + (c as f32 + 0.5) * Grid.cell\n"
                f"      ty = Grid.oy + (r as f32 + 0.5) * Grid.cell\n    }}\n  }}")
    if kind == "fused":
        # Schedule: a group of fused runs (1), or a run not fused (2): the
        # first in sight.
        test = ("f.kind == 2 && f.count > 1" if target[1] == 1 else
                "f.kind == 3 && f.group < 0 && len(f.why) > 0")
        return (f"Pick.f = 1000000000.0\n"
                f"  for f: FL where f.y >= 0.0 && {test} && f.y - Fz.top > 44.0 {{ Pick.f min= f.y }}\n"
                f"  if Pick.f < 1000000000.0 {{\n    tx = panel() + 160.0\n"
                f"    ty = top() + 16.0 + Pick.f - Fz.top + 12.0\n  }}")
    if kind == "value":
        # Provenance: a value of the list, by its words.
        return (f"Pick.a = -1\n"
                f"  for v: Val where v.label == {ent_string(target[1])} && v.row >= 0 {{ Pick.a max= v.row }}\n"
                f"  if Pick.a >= 0 {{\n    tx = 90.0\n"
                f"    ty = list_y() + (Pick.a as f32 + 0.5) * row_height() - Prov.list_top\n  }}")
    if kind == "chip":
        # Provenance: the first chip of a "comes from" card with these words.
        return (f"Pick.a = 1000000000\n"
                f"  for f: Fr where f.shown && f.label == {ent_string(target[1])} {{ Pick.a min= f.order }}\n"
                f"  if Pick.a < 1000000000 {{\n    let order = Pick.a\n"
                f"    Pick.f = -1.0\n    Pick.g = -1.0\n"
                f"    for f: Fr where f.order == order {{\n"
                f"      Pick.f max= from_x() + 14.0 + f.x + f.w * 0.5\n"
                f"      Pick.g max= cards_y() - Prov.top + WrAt[f.write].y + card_head() + f.y + chip_h() * 0.5\n"
                f"    }}\n    tx = Pick.f\n    ty = Pick.g\n  }}")
    if kind == "chart":
        return (f"tx = chart_x() + chart_w(Window.width as f32) * {target[1]}\n"
                f"  ty = counts_y() + 70.0")
    if kind == "took":
        return (f"tx = panel() + 420.0\n"
                f"  ty = table_y() + 24.0 + ({target[1]}.0 + 0.5) * took_h() - Tm.scroll")
    if kind == "memory":
        # Memory: the biggest box under the one shown whole that has parts.
        return (f"var best = -1\n  var k = 0\n"
                f"  while k < len(MemAt) {{\n"
                f"    if MemAt[k].parent == Mem.root && MemAt[k].count > 0 && MemAt[k].w > 40.0 {{\n"
                f"      if best < 0 || MemAt[k].bytes > MemAt[best].bytes {{ best = k }}\n    }}\n"
                f"    k += 1\n  }}\n"
                f"  if best >= 0 {{\n    tx = MemAt[best].x + MemAt[best].w * {target[1]}\n"
                f"    ty = MemAt[best].y + MemAt[best].h * {target[2]}\n  }}")
    if kind == "frame":
        # Cores: a frame of the list at the left.
        return (f"tx = 90.0\n"
                f"  ty = top() + 30.0 + ({target[1]}.0 + 0.5) * row_height() * 2.0")
    if kind == "slice":
        # Cores: a thread's longest share of the work in the frame shown.
        thread = target[1]
        return (f"let x0 = cores_x0()\n  let w = Window.width as f32 - x0 - 28.0\n"
                f"  let scale = w / max(Co.us, 1.0)\n  let chosen = Co.frame\n  let deepest = Co.depth\n"
                f"  Pick.f = -1.0\n"
                f"  for s: CSlice where s.frame == chosen && s.thread == {thread} && s.kind == 2 {{ Pick.f max= s.us }}\n"
                f"  let most = Pick.f\n  Pick.g = -1.0\n  Pick.h = -1.0\n"
                f"  for s: CSlice where s.frame == chosen && s.thread == {thread} && s.kind == 2 && s.us == most {{\n"
                f"    Pick.g max= x0 + (s.from + s.us * 0.5) * scale\n"
                f"    Pick.h max= lane_y(top() + 56.0, s.thread, s.depth, deepest) + lane_h() * 0.5\n"
                f"  }}\n"
                f"  if Pick.g >= 0.0 {{\n    tx = Pick.g\n    ty = Pick.h\n  }}")
    raise ValueError(target)


def aim(target, seconds=GLIDE):
    """ent statements that start the pointer gliding to target."""
    return ("var tx = -1.0\n  var ty = -1.0\n  " + target_code(target) +
            f"\n  if tx >= 0.0 {{\n    Glide.fx = Glide.x\n    Glide.fy = Glide.y\n"
            f"    Glide.tx = tx\n    Glide.ty = ty\n    Glide.t0 = now\n"
            f"    Glide.t1 = now + {seconds}\n  }}")


# ---- The frames round what is talked about: rectangles, in the window's
# points, as ent of `width` and `height`.
def box(x, y, w, h):
    return (x, y, w, h)


CONTENT = box("panel() + 4.0", "top() + 4.0", "width - panel() - 8.0", "height - top() - 8.0")
LIST = box("4.0", "top() + 4.0", "panel() - 8.0", "height - top() - 8.0")
TITLE = box("10.0", "8.0", "560.0", "52.0")
TABS = box("width - 984.0", "8.0", "910.0", "32.0")


def tab_box(k):
    return box(f"width - 982.0 + {k}.0 * 130.0", "8.0", "126.0", "32.0")


# ---- The script.
# Beats: (words, [(at, action, ...)], hold). Actions: ("click", target),
# ("move", target), ("type", text), ("key", name), ("wheel", lines,
# notches), ("hold", target) and ("drag_to", target) and ("let_go",),
# ("mark", box, label, seconds).
BEECS = [
    ("This is ent map. It draws the world of an e n t program: what its entities "
     "are, which systems touch which data, and how the program ran.",
     [(0.4, "mark", TITLE, "", 3.0), (3.2, "mark", TABS, "Seven views", 3.4)], 0),
    ("One command makes it: ent map, and the program. This is beecs, a model "
     "of a honeybee colony, from e n t's examples.", [], 0),

    ("The first view is the lifecycle. Each square is a run of a system that "
     "changes entities: spawns them, adds or removes components, or despawns them.",
     [(0.3, "mark", CONTENT, "Lifecycle", 5.5)], 0),
    ("The runs follow each other to the right, as the schedules run them. "
     "Point at a cell to see the change, the run and the line it is made at.",
     [(1.4, "move", at("cell", 0, "spawn")), (4.6, "move", at("cell", 4, "despawn"))], 2.0),
    ("On the left are the archetypes. Pick one to see only its entities.",
     [(1.2, "click", at("row", 3)), (3.6, "click", at("row", 0))], 1.0),
    ("The wheel zooms, and dragging moves the wave.",
     [(0.2, "move", at("area", 0.45, 0.42)), (0.9, "wheel", 1.0, 6),
      (2.0, "hold", at("area", 0.45, 0.42)), (2.2, "drag_to", at("area", 0.38, 0.5)),
      (3.6, "let_go")], 1.0),
    ("Click a change, and its source opens in the map. From there it opens in "
     "V S Code or Zed, at that line.",
     [(0.6, "click", at("cell", 4, "despawn")),
      (2.8, "mark", box("width - 254.0", "top() + 6.0", "178.0", "34.0"), "Open in your editor", 3.2)], 0),
    ("Escape closes it.", [(0.2, "key", "escape")], 1.0),

    ("Systems by data: a row for each run of a system, a column for each "
     "component, unique or buffer it touches. Blue reads, orange writes.",
     [(0.2, "click", at("tab", 1)), (2.0, "mark", CONTENT, "Systems × data", 4.5)], 0),
    ("A red dot marks a system with effects outside the world, such as "
     "printing or reading a file. Point at a cell to see what the run reads "
     "and writes there, and the stage it runs in.",
     [(3.6, "move", at("grid", "dance"))], 1.5),
    ("Pick a schedule to see its runs alone: here, a round of foraging.",
     [(0.8, "click", at("schedule", "foraging.round"))], 1.5),

    ("The schedule view shows which runs e n t build fuses into one loop, "
     "and why the others stay apart.",
     [(0.2, "click", at("tab", 2)), (2.4, "click", at("schedule", "dusk"))], 0),
    ("Fused runs are one loop over each archetype, every run's body in turn "
     "for each entity.",
     [(0.5, "move", at("fused", 1))], 1.5),
    ("Where a run is not fused, the map says why.",
     [(0.4, "move", at("fused", 2))], 2.0),

    ("Provenance answers where a value comes from. Type to find one.",
     [(0.2, "click", at("tab", 4)), (2.4, "click", at("xy", "80.0", "top() + 24.0")),
      (2.9, "type", "honey")], 0),
    ("Pick the honey in the hive.",
     [(0.4, "click", at("value", "Hive.honey"))], 1.0),
    ("On the left, each write that gives it a value, and what that is made "
     "from. On the right, the writes it goes into.",
     [(0.4, "mark", box("from_x() - 6.0", "top() + 50.0", "card_width() + 12.0", "height - top() - 56.0"),
       "Comes from", 3.0),
      (3.6, "mark", box("to_x() - 6.0", "top() + 50.0", "card_width() + 12.0", "height - top() - 56.0"),
       "Goes into", 2.6)], 0),
    ("Click a value to follow it back, and back again.",
     [(0.6, "click", at("chip", "EnergyContent.honey")),
      (2.8, "click", at("xy", "from_x() + 30.0", "top() + 27.0"))], 1.5),

    ("With a trace of a run, the time view shows how the program ran: how many "
     "entities each archetype had, after each run of the schedules.",
     [(0.2, "click", at("tab", 5)), (2.6, "move", at("chart", 0.15)),
      (4.2, "move", at("chart", 0.55)), (5.8, "move", at("chart", 0.85))], 0),
    ("Below it, how long each run took, and every system, schedule and loop "
     "by its own time. A click shows the source.",
     [(0.8, "move", at("took", 0)), (3.4, "click", at("took", 0))], 1.0),
    ("", [(0.0, "key", "escape")], 0.8),

    ("Memory lays out the world's bytes as a treemap: archetypes, uniques, "
     "relations, buffers and indexes, each as big as it is.",
     [(0.2, "click", at("tab", 6)), (2.6, "move", at("memory", 0.3, 0.3))], 0),
    ("Click to go in, and Backspace to come out again.",
     [(0.4, "click", at("memory", 0.4, 0.4)), (2.6, "key", "backspace")], 1.5),
]

MELT = [
    ("The last views need a program that runs on many cores. This is melt, "
     "LAMMPS's Lennard-Jones benchmark: thirty two thousand atoms, built with "
     "dash dash parallel, and traced.", [], 0),
    ("Cores shows what each thread did in a frame: here, one step of the "
     "simulation on twelve threads.",
     [(0.2, "click", at("tab", 3)), (2.2, "mark", CONTENT, "Cores", 3.5)], 0),
    ("On the left, a few frames: the first, the shortest, the longest, the "
     "median.",
     [(1.4, "click", at("frame", 2)), (3.6, "click", at("frame", 4))], 1.0),
    ("Point at a slice to see what the thread worked on, and for how long.",
     [(0.6, "move", at("slice", 4))], 2.0),
    ("Below, each loop over entities: how long it took, and how busy its "
     "threads were.",
     [(0.4, "mark", box("panel() + 4.0", "top() + 356.0", "width - panel() - 8.0", "150.0"),
       "Loops", 3.5)], 0),
    ("And in time, the loops by their own time: the forces loop keeps its "
     "threads busy ninety three percent of the time.",
     [(0.2, "click", at("tab", 5)), (2.8, "move", at("took", 0))], 1.5),
    ("That is ent map. ent map, and the program; add dash dash trace for "
     "how it ran.", [(0.4, "mark", TABS, "", 4.0)], 1.0),
]

SEGMENTS = [("beecs", BEECS), ("melt", MELT)]


def seconds_of(audio):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "format=duration", "-of", "csv=p=0", audio],
                         capture_output=True, text=True, check=True)
    return float(out.stdout.strip())


def busy(action, at):
    """Until when an action keeps the keyboard and mouse busy."""
    kind = action[1]
    if kind == "click":
        return at + CLICK_PRESS + CLICK_EACH + 0.15
    if kind == "type":
        return at + len(action[2]) * TYPE_EACH + 0.15
    if kind == "key":
        return at + 0.35
    if kind == "hold":
        return at + CLICK_PRESS + 0.1
    if kind == "drag_to":
        return at + DRAG + 0.1
    if kind == "move":
        return at + GLIDE + 0.1
    if kind == "let_go":
        return at + 0.1
    if kind == "wheel":
        return at + 0.06 * (action[3] if len(action) > 3 else 1)
    return at


def sequenced(actions):
    """When each action of a beat starts: at its own time, or after the
    click, typing or keys before it."""
    free = 0.0
    out = []
    for action in actions:
        at = action[0]
        if action[1] != "mark":
            at = max(at, free)
            free = busy(action, at)
        out.append(at)
    return out


def beat_length(actions):
    ends = [at + a[4] if a[1] == "mark" else busy(a, at)
            for a, at in zip(actions, sequenced(actions))]
    return max(ends, default=0.0) + 0.6


# (Where the pointer was to go, it is, before a press: frames are slow
# while the video is made, and the map takes a press where the last move
# it saw was.)
LANDED = ("Glide.x = Glide.tx\n  Glide.y = Glide.ty\n  Glide.t1 = 0.0\n"
          "  post_mouse(Glide.x, Glide.y, 5, 0)\n  ")


def driver_part(beats, timeline, total):
    """The timed part of the driver: video_script, caption_at, video_marks,
    total_seconds."""
    steps = []   # (time, ent statements)
    captions = []
    marks = []
    for (words, actions, _), (start, said) in zip(beats, timeline):
        if words:
            captions.append((start, start + said + 0.35, words))
        for action, when in zip(actions, sequenced(actions)):
            when += start
            kind = action[1]
            if kind == "mark":
                _, _, where, label, length = action
                marks.append((when, when + length, where, label))
            elif kind == "move":
                steps.append((when, aim(action[2])))
            elif kind == "click":
                steps.append((when, aim(action[2])))
                press = when + CLICK_PRESS
                steps.append((press, LANDED + "Ripple.x = Glide.x\n  Ripple.y = Glide.y\n  Ripple.t = now\n"
                                     "  post_mouse(Glide.x, Glide.y, 1, 1)"))
                steps.append((press + 0.08, "post_mouse(Glide.x, Glide.y, 2, 1)"))
            elif kind == "type":
                steps.append((when, f"Typing.words = {ent_string(action[2])}\n"
                                    f"  Typing.at = 0\n  Typing.next = now"))
            elif kind == "key":
                chars, code, shown = KEYS[action[2]]
                steps.append((when, f"Badge.words = {ent_string(shown)}\n  Badge.t = now"))
                steps.append((when + 0.06, f'post_key("{chars}", {code}, false, 1)'))
                steps.append((when + 0.14, f'post_key("{chars}", {code}, false, 2)'))
            elif kind == "hold":
                steps.append((when, aim(action[2])))
                steps.append((when + CLICK_PRESS, LANDED + "Ripple.x = Glide.x\n  Ripple.y = Glide.y\n"
                              "  Ripple.t = now\n  Glide.down = true\n"
                              "  post_mouse(Glide.x, Glide.y, 1, 1)"))
            elif kind == "drag_to":
                steps.append((when, aim(action[2], DRAG)))
            elif kind == "let_go":
                steps.append((when, "Glide.down = false\n  post_mouse(Glide.x, Glide.y, 2, 1)"))
            elif kind == "wheel":
                # (A notch every 0.06 s, so it is seen to move.)
                lines, notches = action[2], action[3] if len(action) > 3 else 1
                for n in range(notches):
                    steps.append((when + n * 0.06, f"post_wheel({lines})"))
            else:
                raise ValueError(kind)
    steps.sort(key=lambda s: s[0])
    out = ["// ---- tools/map/video: the timed part (made by make.py)\n"]
    out.append(f"fn total_seconds() -> f32 {{ {total:.2f} }}\n")
    out.append("fn caption_at(t: f32) -> text {\n")
    for i, (a, b, words) in enumerate(captions):
        out.append(f"  {'if' if i == 0 else '} else if'} t >= {a:.2f} && t < {b:.2f} {{\n"
                   f"    {ent_string(words)}\n")
    out.append('  } else {\n    ""\n  }\n}\n' if captions else '  ""\n}\n')
    out.append("proc video_marks(t: f32, width: f32, height: f32) {\n")
    for a, b, (x, y, w, h), label in marks:
        out.append(f"  if t >= {a:.2f} && t < {b:.2f} {{ mark_label({x}, {y}, {w}, {h}, "
                   f"{ent_string(label)}, fade(t, {a:.2f}, {b:.2f}), width) }}\n")
    out.append("}\n")
    out.append("system video_script() {\n  let now = VClock.now\n  let prev = VClock.prev\n")
    for when, code in steps:
        out.append(f"  if crossed(prev, now, {when:.2f}) {{\n  {code}\n  }}\n")
    out.append("}\n// ---- (end of tools/map/video's timed part)\n\n")
    return "".join(out)


def srt_time(t):
    ms = int(round(t * 1000))
    return f"{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}"


def run(command, **kw):
    print("make.py:", " ".join(command[:3]), "...", flush=True)
    subprocess.run(command, check=True, **kw)


def maps(root, work, env):
    """The examples' maps, with how they ran: beecs.map and melt.map."""
    ent = os.path.join(root, "tools", "ent")
    beecs_run = os.path.join(root, "build", "beecs", "run")
    if not os.path.isdir(os.path.join(beecs_run, "foraging-period")):
        run(["sh", os.path.join(root, "examples", "beecs", "reference", "fetch.sh")], env=env)
    out = {}
    for name, program, folder, cwd in [
            ("beecs", "examples/beecs/beecs.ent", "examples/beecs", beecs_run),
            ("melt", "examples/lj/melt.ent", "examples/lj", work)]:
        trace = os.path.abspath(os.path.join(work, name + ".trace.json"))
        traced = dict(env, ENT_TRACE=trace)
        run([ent, "run", "--parallel", "--trace", "-I", os.path.join(root, folder),
             os.path.join(root, program)], cwd=cwd, env=traced, stdout=subprocess.DEVNULL)
        path = os.path.abspath(os.path.join(work, name + ".map"))
        run([ent, "map", os.path.join(root, program), "-I", os.path.join(root, folder),
             "--trace", "-o", path, "--no-window"], env=traced)
        out[name] = path
    return out


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
    os.chdir(root)
    env = dict(os.environ)
    env.setdefault("ENT_BIN", os.path.join(root, "build-macos", "bin"))
    env.setdefault("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
    work = os.path.join("build", "map-video")
    # (MAP_VIDEO_MAPS=keep: the maps made before, while the script is
    # worked on.)
    keep = os.environ.get("MAP_VIDEO_MAPS") == "keep" and \
        all(os.path.isfile(os.path.join(work, n + ".map")) for n, _ in SEGMENTS)
    if keep:
        for n, _ in SEGMENTS:
            shutil.rmtree(os.path.join(work, n), ignore_errors=True)
        shutil.rmtree(os.path.join(work, "said"), ignore_errors=True)
        made = {n: os.path.abspath(os.path.join(work, n + ".map")) for n, _ in SEGMENTS}
    else:
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
    os.makedirs(os.path.join(work, "said"))
    if not keep:
        made = maps(root, work, env)

    viewer = open("tools/map/ent_map.ent").read()
    fixed = open("tools/map/video/driver.ent.part").read()
    cut = fixed.index("// Over all that was drawn")
    timeline = []   # (segment, start in the video, said, words)
    offset = 0.0
    screens = []
    for n, (name, beats) in enumerate(SEGMENTS):
        # The words, said; each beat as long as they are, or its hold.
        own = []
        t = START
        for i, (words, actions, hold) in enumerate(beats):
            said = 0.0
            if words:
                audio = os.path.join(work, "said", f"{n}-{i:02}.aiff")
                subprocess.run(["say", "-v", VOICE, "-r", RATE, "-o", audio, words], check=True)
                said = seconds_of(audio)
                timeline.append((offset + t, said, words, audio))
            own.append((t, said))
            t += max(said + 0.55 if words else 0.0, hold, beat_length(actions))
        total = t + 0.6
        print(f"make.py: {name}: {len(beats)} beats, {total:.1f} s", flush=True)

        # The map with the driver.
        s = viewer

        def put(old, new):
            nonlocal s
            if s.count(old) != 1:
                sys.exit("make.py: ent_map.ent has no single " + repr(old))
            s = s.replace(old, new)
        part = fixed[:cut] + driver_part(beats, own, total) + fixed[cut:]
        put("import text\n", "import text\nimport video\n")
        put("schedule frame() {", part + "schedule frame() {")
        put("  begin()\n  load_map()\n", "  begin()\n  load_map()\n  video_tick()\n  video_script()\n")
        put("  present()\n  shot()\n", "  video_draw()\n  video_capture()\n  present()\n  shot()\n")
        put("loop { frame() } until Window.closed || Shot.done",
            "loop { frame() } until Window.closed || Shot.done || VideoDone")
        folder = os.path.join(work, name)
        os.makedirs(folder)
        program = os.path.join(folder, "ent_map_video.ent")
        open(program, "w").write(s)
        run([os.path.join("tools", "ent"), "build", "--no-index-remarks", "-I", "tools/map/video",
             program, "-o", os.path.join(folder, "ent_map_video")], env=env)
        app = subprocess.run(["./ent_map_video", made[name]], cwd=folder, env=env)
        if app.returncode != 0:
            sys.exit(f"make.py: the map stopped with {app.returncode}")
        screens.append(os.path.join(folder, "screen.mp4"))
        offset += seconds_of(screens[-1])

    # The segments one after the other; the words under them, each at its
    # beat; the captions as a file.
    listing = os.path.join(work, "screens.txt")
    with open(listing, "w") as f:
        for screen in screens:
            f.write(f"file '{os.path.abspath(screen)}'\n")
    screen = os.path.join(work, "screen.mp4")
    run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", listing,
         "-c", "copy", screen])
    inputs, delays = [], []
    for i, (start, _, _, audio) in enumerate(timeline):
        inputs += ["-i", audio]
        ms = int((start + 0.15) * 1000)
        delays.append(f"[{i + 1}:a]adelay={ms}|{ms},aformat=channel_layouts=stereo[a{i}]")
    mix = ";".join(delays) + ";" + "".join(f"[a{i}]" for i in range(len(timeline))) \
        + f"amix=inputs={len(timeline)}:normalize=0,apad[voice]"
    out = os.path.join(work, "ent-map-tour")
    run(["ffmpeg", "-loglevel", "error", "-y", "-i", screen] + inputs +
        ["-filter_complex", mix, "-map", "0:v", "-map", "[voice]",
         "-c:v", "libx264", "-preset", "slow", "-crf", "20", "-tune", "stillimage",
         "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "160k", "-shortest",
         "-movflags", "+faststart", out + ".mp4"])
    with open(out + ".srt", "w") as srt:
        for k, (start, said, words, _) in enumerate(timeline, 1):
            srt.write(f"{k}\n{srt_time(start)} --> {srt_time(start + said + 0.35)}\n{words}\n\n")
    print(f"make.py: {out}.mp4 ({os.path.getsize(out + '.mp4') // 1024} KB), {out}.srt")


if __name__ == "__main__":
    main()
