#!/usr/bin/env python3
"""Makes the music of swarm.ent: four short pieces, written here as notes
and made into sound by this script (then into .ogg by ffmpeg).

  python3 examples/swarm/make_music.py

calm.ogg    the title screen: slow, a soft arpeggio
fight.ogg   the game: a bass that drives and a lead over it
frenzy.ogg  when the swarm is thick: the same, faster and harsher
fall.ogg    the hero falls: a few notes down, heard once
"""
import math
import os
import struct
import subprocess
import sys
import wave

RATE = 32000
HERE = os.path.dirname(os.path.abspath(__file__))


def pitch(note):
    """Hertz of a note counted in half tones from the A of 440."""
    return 440.0 * 2 ** (note / 12)


def voice(kind, phase):
    if kind == "square":
        return 1.0 if phase % 1 < 0.5 else -1.0
    if kind == "thin":
        return 1.0 if phase % 1 < 0.25 else -1.0
    if kind == "saw":
        return 2 * (phase % 1) - 1
    if kind == "triangle":
        return 4 * abs(phase % 1 - 0.5) - 1
    return math.sin(2 * math.pi * phase)


def play(out, start, seconds, note, kind, loud, fall=6.0):
    """Adds one note: a fast rise, then dying away."""
    first = int(start * RATE)
    hertz = pitch(note)
    for i in range(int(seconds * RATE)):
        if first + i >= len(out):
            break
        t = i / RATE
        rise = min(1.0, t * 200)
        end = min(1.0, (seconds - t) * 60)
        out[first + i] += (voice(kind, hertz * t) * loud * rise * end *
                           math.exp(-fall * t))


def beat(out, start, loud, hiss=False):
    """A drum: a low thump that falls, or a short hiss."""
    first = int(start * RATE)
    seed = 12345
    for i in range(int(0.12 * RATE)):
        if first + i >= len(out):
            break
        t = i / RATE
        if hiss:
            seed = (seed * 1103515245 + 12345) % 2147483648
            out[first + i] += (seed / 1073741824 - 1) * loud * math.exp(-45 * t)
        else:
            out[first + i] += (math.sin(2 * math.pi * (110 * t - 300 * t * t))
                               * loud * math.exp(-22 * t))


def piece(name, tempo, bars, bass, lead, bass_kind, lead_kind, drums, chords):
    step = 60.0 / tempo / 4            # a sixteenth
    out = [0.0] * int(bars * 16 * step * RATE)
    for bar in range(bars):
        root = chords[bar % len(chords)]
        for i in range(16):
            at = (bar * 16 + i) * step
            if bass[i] is not None:
                play(out, at, step * 1.9, root + bass[i] - 24, bass_kind, 0.30)
            turn = lead[(bar % 2) * 16 + i] if len(lead) > 16 else lead[i]
            if turn is not None:
                play(out, at, step * 2.5, root + turn, lead_kind, 0.16, 4.0)
            if drums and i % 4 == 0:
                beat(out, at, 0.5)
            if drums and i % 4 == 2:
                beat(out, at, 0.12 * drums, hiss=True)
    write(name, out)


def write(name, out):
    top = max(1e-9, max(abs(x) for x in out))
    raw = os.path.join(HERE, "music", name + ".wav")
    with wave.open(raw, "wb") as sound:
        sound.setnchannels(1)
        sound.setsampwidth(2)
        sound.setframerate(RATE)
        sound.writeframes(b"".join(
            struct.pack("<h", int(x / top * 26000)) for x in out))
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", raw,
                    "-c:a", "libvorbis", "-q:a", "3",
                    os.path.join(HERE, "music", name + ".ogg")], check=True)
    os.remove(raw)


N = None
# (Half tones over the chord's root.)
piece("calm", 84, 8,
      [0, N, N, N, 7, N, N, N, 12, N, N, N, 7, N, N, N],
      [N, N, 12, N, 15, N, 19, N, 24, N, 19, N, 15, N, 12, N],
      "sine", "triangle", 0, [0, -4, -7, -2])
piece("fight", 138, 8,
      [0, N, 0, 12, 0, N, 0, 12, 0, N, 0, 12, 0, 7, 10, 12],
      [12, N, 15, N, 19, N, 15, 12, N, 22, N, 19, N, 15, N, N,
       24, N, 22, N, 19, N, 15, N, 19, N, 15, N, 12, N, 10, N],
      "saw", "square", 1, [0, 0, -4, -2])
piece("frenzy", 168, 8,
      [0, 0, 12, 0, 0, 12, 0, 12, 0, 0, 12, 0, 7, 12, 10, 12],
      [24, 19, 15, 19, 24, 19, 15, 19, 27, 22, 19, 22, 27, 22, 19, 22,
       24, 19, 15, 12, 24, 19, 15, 12, 22, 19, 15, 10, 22, 19, 15, 10],
      "saw", "thin", 2, [0, -2, -4, 1])
fall = [0.0] * int(2.6 * RATE)
for i, note in enumerate([12, 8, 5, 0, -4, -12]):
    play(fall, i * 0.26, 1.2, note, "triangle", 0.3, 2.5)
write("fall", fall)
print("made:", ", ".join(sorted(os.listdir(os.path.join(HERE, "music")))))
