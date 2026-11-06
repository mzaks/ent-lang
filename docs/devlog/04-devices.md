# 04 · Devices: keeping the world away from C

*3 to 26 October 2026. One decision on 5 October, then a few weeks of
growing devices around it: a window, sound, fonts, pictures, files, a
saved world, a 3D scene, and an editor to read it all in.*

## The problem with the first door

A program that only computes is a heater. ent-lang programs have to print,
draw, play sounds and read files, so they need a way out. The first one was
the obvious door: an *extern system*. A system written in C got the whole
world and promised to keep to the access it had declared.

It didn't keep that promise for long. The console's C walked one archetype by
hand. When a `Print` was spawned together with another component, it landed
in a different archetype and the line was silently dropped. The C didn't do
anything wrong by C's standards. It simply didn't know about a table the
compiler had made after the C was written. That's the whole case against
handing the world to foreign code: **whatever the compiler knows, the C
doesn't.**

## 5 October: devices are ent-lang, C gets values

That day I decided that device C never gets the world. A device is a module
written in ent-lang. Its systems call C functions, and only with values:

- `extern fn name(params) -> T` is pure. Its calls are computation, so the
  system around it is fused, parallelised and scheduled like any other.
- `extern proc name(params) [-> T]` acts on the outside. Its system keeps its
  place among all the others, is never fused, and visits its entities in
  order.

The console became twelve lines of ent-lang and two C functions:

```
component Print { line: text } capacity 1024

extern proc put(line: text)
extern proc flush()

system write() {
  for p: Print { put(p.line) }
  flush()
}

system clear() {
  for e with Print { e.destroy() }
}
```

Now `Print` can share an entity with anything, because the *compiler* finds
every archetype that holds one. The clock went the same way: its C is a single
`wait_until`.

A module's implementation sits next to it as `m.c`, `m.cpp`, `m.o`, `m.a` or
`m.build` (a command that builds the object), with `m.link` naming libraries.
The driver compiles the program with the LLVM that made its IR and hands the
C to the machine's own compiler.

The old extern system still exists. It's the wide door, unchecked, for when
you really need it. It's also how saving the world works, see below.

## Buffers: bulk data without the world

Values one at a time are fine for `put`. They're not fine for drawing 382,000
things. So on 9 October came `buffer`: rows the program fills, and an
`extern proc` can be handed them.

```
transient buffer Shapes {
  x: f32, y: f32, w: f32, h: f32, color: i32, round: bool
} capacity 16384
extern proc draw_shapes(them: Shapes)
```

A `for` over entities appends with `Shapes += { x: .., y: .., ... }`. Each
entity writes to a place of its own, and the rows are copied out afterwards
in entity order. So the `for` stays pure computation, runs on all cores, and
gives the same rows in the same order every time. The C gets a count and one
pointer per field: columns in, columns out.

The swarm example, at 382,000 entities a frame:

| | before | with `Shapes` |
|---|---|---|
| all cores | 15 ms | 6.4 ms |
| one core | 33.5 ms | 24 ms |
| drawing alone | 10 ms | 2 ms |

A third of the old cost was the program's own `for`s. They called the window
once per thing, and a `for` that calls a proc runs on one core. Batching fixed
the drawing, and it also gave the cores back.

Later, buffers could be windowed, passed as rows to fns, and hold entities.
Pushover's grid of "who stands where" is a buffer of `entity` (see
[post 05](05-games.md)).

## The window, sound, fonts, pictures

Most devices are raylib underneath and ent-lang on top. They grew whenever a
game needed something, which is a polite way of saying "constantly":

- **Window** (5 Oct): `rect`, `circle`, `line`, `label`, keys and mouse. Then
  `sector` (a slice of a circle, for a certain yellow mouth), resizing,
  fullscreen, sizes in points, zoom, a least size, the mouse's motion and
  middle button, and a program keeping Esc for itself. Also `draw_pixels`,
  which hands over the program's own 320×200 picture.
- **Fonts and text:** labels in a font the program names, then in the
  machine's fonts. Paragraphs, wrapping and measuring came for the Clay port.
  Fonts and pictures can be asked for before the window is open: the number
  comes at once and the work happens on first use.
- **Pictures:** loaded, tinted (and see-through since 12 Oct), up to 2047 of
  them. Since 3 November they're files in both directions, and files can be
  dropped on the window.
- **Sound:** first a `Tone` spawned as an entity, a wave sliding between two
  pitches, then samples from files and from bytes the program holds. On 11
  October came `stream(samples, count, rate, volume)` with `stream_wants`,
  for a program that is its own synthesiser. The commit admitted: "Tried with
  a tone made for three seconds; not heard by anyone yet." The program it was
  for was Doom's music, played on a sound chip written in ent-lang
  ([post 05](05-games.md)).
- **Files:** a file's bytes into a buffer, a buffer's bytes out, `list_files`.
  Then `asset`: a program declares the files it uses (`asset "themes/*.png"`),
  `ent build` takes them along, and `--embed` builds them into the binary.
  Pushover is one 33 MB file that opens no game files.
- **A saved world:** `import world`, then `save_world()` and `load_world()`.
  A world is one arena with a fixed layout and no addresses in it, so saving
  writes out the parts of the arena in use. What isn't part of the game is
  marked `transient` (the screen, the menu, the music player) and stays as it
  is across a load. A file of another layout is refused. Doom's save games
  are this, 500 to 700 kB each, and a demo saved at tic 1500 and loaded back
  plays on exactly like the reference.

## A third dimension

On 21 October `devices/scene` drew boxes, lines and balls in 3D into the same
window, under the 2D drawing. On 26 October `devices/mesh` arrived for worlds
too big to draw box by box. A program fills `Faces` with the visible faces of
its blocks, `set_mesh(slot, Faces)` uploads them to the GPU once, and
`draw_meshes` draws every slot in view, with fog, ambient occlusion and two
lights per corner. The first user was a block game in ent-lang with a world
of 256 × 256 × 96.

## Reading it all: the editor, 6 October

The language server came a day after devices, and with a clear priority. In
the words I used then: "as we do AI assisted development the browsing
capabilities are much more important than editing capabilities." Claude
writes most of the code, and I mostly read and navigate it.

So `ent-lsp` started with what helps *reading*:

- the errors and warnings of every stage: importer, verifier, scheduling and
  lowering;
- an outline, and go to definition, also into imported modules;
- on hover, the declaration with its comment, and for a system **what it
  reads and writes, whether it acts on the outside, and what it waits for in
  the schedule and why.**

That last one is the point. The access analysis from [day one](01-day-one.md)
is the compiler's best knowledge about a program, and now it shows up under
the mouse. Completion and recovery after a parse error could wait. While a
file doesn't parse, the server answers from the last version that did.

Next to it came a tree-sitter grammar (colours, outline, brackets,
indentation), a Zed extension and a VS Code extension. The commit log shows
the cost of a fast-moving language: about twenty commits titled "Point the
Zed extension at the grammar with ...", one for each new piece of syntax. On
11 October the language got an icon, which is how you know it's serious.

## Later: other languages

Devices in Mojo and Rust, and purity checked instead of trusted, came in
[post 11](11-other-languages-and-v0-1.md). A device in Goose, what it taught
the checks, and a comparison of ent, C, Rust and Goose for device functions
are in [post 12](12-goose-and-device-languages.md).

## What this settled

- **The compiler sees everything, or it guarantees nothing.** Foreign code
  gets values and columns, never the world.
- **Pure or not is a declaration**, and it decides scheduling. (Whether an
  `extern fn` *really* is pure became a compile-time check much later, for
  C, Rust and Mojo: [post 11](11-other-languages-and-v0-1.md), and Goose:
  [post 12](12-goose-and-device-languages.md).)
- **Bulk data goes in columns**, filled in parallel and deterministically,
  read by C as plain arrays.
- **Devices grow when a program needs them**, not from a wish list.

Next: [05 · Pac-Man, Doom, Pushover and a UI library](05-games.md).
