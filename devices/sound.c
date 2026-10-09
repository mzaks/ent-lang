// The sound device (devices/sound.ent), played with raylib. Compiled with
// the program, against the declarations generated for it (ent_extern.h);
// it never sees the world.

#include "ent_extern.h"
#include "ent_files.h"

#include <math.h>
#include <raylib.h>
#include <stdlib.h>
#include <string.h>

enum { ENT_SOUND_RATE = 44100, ENT_SOUND_VOICES = 24, ENT_SOUND_FILES = 32 };

static void ent_sound_close(void) { CloseAudioDevice(); }

// Opens the machine's sound the first time something is played; false
// where there is none.
static bool ent_sound_ready(void) {
  static bool tried, ready;
  if (!tried) {
    tried = true;
    SetTraceLogLevel(LOG_WARNING);
    InitAudioDevice();
    ready = IsAudioDeviceReady();
    if (ready)
      atexit(ent_sound_close);
  }
  return ready;
}

// The tones that may sound together: a new one takes the place of one
// that has ended, or else of the oldest.
static Sound ent_sound_voices[ENT_SOUND_VOICES];
static bool ent_sound_used[ENT_SOUND_VOICES];

static void ent_sound_start(Wave wave, float volume) {
  static unsigned oldest;
  int voice = -1;
  for (int i = 0; i < ENT_SOUND_VOICES && voice < 0; ++i)
    if (!ent_sound_used[i] || !IsSoundPlaying(ent_sound_voices[i]))
      voice = i;
  if (voice < 0)
    voice = (int)(oldest++ % ENT_SOUND_VOICES);
  if (ent_sound_used[voice]) {
    StopSound(ent_sound_voices[voice]);
    UnloadSound(ent_sound_voices[voice]);
  }
  ent_sound_voices[voice] = LoadSoundFromWave(wave);
  ent_sound_used[voice] = true;
  SetSoundVolume(ent_sound_voices[voice], volume);
  PlaySound(ent_sound_voices[voice]);
}

void ent_sound_tone(float pitch, float to, float seconds, float after,
                    float volume, ent_sound_Wave wave) {
  if (!ent_sound_ready() || !(seconds > 0) || !(pitch > 0) || !(volume > 0))
    return;
  if (!(to > 0))
    to = pitch;
  if (!(after > 0))
    after = 0;
  // Nothing here is meant to last: a tone is cut at ten seconds, and
  // starts within a minute.
  seconds = fminf(seconds, 10);
  after = fminf(after, 60);
  int silent = (int)(after * ENT_SOUND_RATE);
  int sounding = (int)(seconds * ENT_SOUND_RATE);
  if (sounding < 1)
    return;
  short *samples = calloc((size_t)(silent + sounding), sizeof(short));
  if (!samples)
    return;
  // A few milliseconds to come and go, so that it does not click.
  float fade_in = 0.004f * ENT_SOUND_RATE, fade_out = 0.010f * ENT_SOUND_RATE;
  float phase = 0, held = 1;
  unsigned noise = 0x1234567u;
  for (int i = 0; i < sounding; ++i) {
    float along = (float)i / (float)sounding;
    float value;
    switch (wave) {
    case ent_sound_Wave_Triangle:
      value = 4 * fabsf(phase - 0.5f) - 1;
      break;
    case ent_sound_Wave_Sine:
      value = sinf(6.2831853f * phase);
      break;
    case ent_sound_Wave_Noise:
      value = held;
      break;
    default:
      value = phase < 0.5f ? 0.6f : -0.6f;
      break;
    }
    float level = fminf(1, fminf((float)i / fade_in,
                                 (float)(sounding - 1 - i) / fade_out));
    samples[silent + i] = (short)(value * level * 20000);
    phase += (pitch + (to - pitch) * along) / ENT_SOUND_RATE;
    if (phase >= 1) {
      phase -= floorf(phase);
      // A hiss is a new height at the pitch's pace.
      noise = noise * 1664525u + 1013904223u;
      held = (float)(noise >> 16 & 0xffff) / 32768.0f - 1;
    }
  }
  Wave made = {(unsigned)(silent + sounding), ENT_SOUND_RATE, 16, 1, samples};
  ent_sound_start(made, fminf(volume, 1));
  free(samples);
}

void ent_sound_sample(const ent_text126 *file, float volume) {
  static struct {
    char name[sizeof(file->bytes) + 1];
    Sound sound;
    bool loaded;
  } files[ENT_SOUND_FILES];
  static int count;
  if (!ent_sound_ready() || !(volume > 0) || file->length == 0)
    return;
  char name[sizeof(file->bytes) + 1];
  memcpy(name, file->bytes, file->length);
  name[file->length] = 0;
  int found = -1;
  for (int i = 0; i < count && found < 0; ++i)
    if (strcmp(files[i].name, name) == 0)
      found = i;
  if (found < 0) {
    // More files than fit: the first ones stay, the rest are not heard.
    if (count == ENT_SOUND_FILES)
      return;
    found = count++;
    strcpy(files[found].name, name);
    // (From the file's bytes, wherever they are: also in the program.)
    const char *kind = GetFileExtension(name);
    struct ent_file held;
    if (kind && ent_file_open(name, &held)) {
      Wave wave = LoadWaveFromMemory(kind, held.bytes, (int)held.size);
      ent_file_close(&held);
      if (wave.data) {
        files[found].sound = LoadSoundFromWave(wave);
        UnloadWave(wave);
      }
    }
    files[found].loaded = files[found].sound.frameCount > 0;
  }
  if (!files[found].loaded)
    return;
  SetSoundVolume(files[found].sound, fminf(volume, 1));
  PlaySound(files[found].sound);
}
