// Plays a recording with the game's own logic and writes a line for each
// tick: the keys, what the tick came to, the time left, the ant, the
// doors, the level's platforms and ladders as one number, every domino,
// and the sounds the tick started. The port writes the same lines
// (../check/trace_check.ent), and the two are compared.
//
//   trace LEVELS RECORDING.rec [more]
//
// LEVELS is the game's folder of level sets; the recording names the set
// and the level. `more` is how many ticks without keys follow the
// recording (100, as the game's own check of its recordings has). The
// first line says which file the level is.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <dirent.h>

#define private public
#define protected public
#include "textsections.h"
#include "soundsys.h"
#include "screen.h"
#include "levelplayer.h"
#include "ant.h"
#include "recorder.h"
#undef private
#undef protected

std::string getHome(void) { return "."; }

static std::vector<std::string> entries(const std::string & path) {
  std::vector<std::string> names;
  DIR * dir = opendir(path.c_str());
  if (!dir) return names;
  while (struct dirent * entry = readdir(dir))
    if (entry->d_name[0] != '.') names.push_back(entry->d_name);
  closedir(dir);
  return names;
}

static std::string nameOf(const std::string & file) {
  std::ifstream stream(file.c_str());
  try {
    textsections_c sections(stream, true);
    return sections.getSingleLine("Name");
  } catch (...) {
    return "";
  }
}

// The file of a level by the names of its set and of itself.
static std::string find(const std::string & levels, const std::string & set, const std::string & level) {
  std::vector<std::string> sets = entries(levels);
  for (size_t i = 0; i < sets.size(); i++) {
    std::string dir = levels + "/" + sets[i];
    if (nameOf(dir + "/index.level") != set) continue;
    std::vector<std::string> files = entries(dir);
    for (size_t j = 0; j < files.size(); j++)
      if (files[j] != "index.level" && nameOf(dir + "/" + files[j]) == level)
        return dir + "/" + files[j];
  }
  return "";
}

static void line(int tick, int keys, int result, levelPlayer_c & l, ant_c & a) {
  printf("%d keys %d result %d time %d", tick, keys, result, l.getTimeLeft());
  printf(" ant %d %d %u %u at %d %d carries %d way %d push %u %d idle %u fell %u left %d flags %d%d%d%d%d%d",
      (int)a.state, (int)a.animation, a.animationImage, a.animationTimer,
      (int)a.blockX, (int)a.blockY, (int)a.carriedDomino, a.direction,
      a.pushDelay, a.pushDelay ? (int)a.pushAnimation : 0, a.inactiveTimer,
      a.fallingHight, a.numPushsLeft, a.finalAnimationPlayed, a.downChecker,
      a.upChecker, a.levelFail, a.levelSuccess, a.triggerNotLast);
  printf(" doors %d %d %d %d idle %d", (int)l.getEntryState(),
      (int)l.getExitState(), l.openDoorEntry, l.openDoorExit, l.inactive);
  unsigned int ground = 0;
  for (size_t y = 0; y < l.levelY(); y++)
    for (size_t x = 0; x < l.levelX(); x++)
      ground = ground * 31 + (l.getPlatform(x, y) ? 1 : 0) + (l.getLadder(x, y) ? 2 : 0);
  printf(" ground %u dominoes", ground);
  for (size_t y = 0; y < l.levelY(); y++)
    for (size_t x = 0; x < l.levelX(); x++)
      if (l.getDominoType(x, y) != DominoTypeEmpty)
        printf(" %d:%d,%d,%d,%d,%d", (int)(y * 20 + x), (int)l.getDominoType(x, y),
            (int)l.getDominoState(x, y), (int)l.getDominoDir(x, y),
            (int)l.getDominoYOffset(x, y), (int)l.getDominoExtra(x, y));
  printf(" sounds");
  std::vector<unsigned int> & started = soundSystem_c::instance()->started;
  for (size_t i = 0; i < started.size(); i++) printf(" %u", started[i]);
  started.clear();
  printf("\n");
}

int main(int argc, char * argv[]) {
  if (argc < 3) {
    fprintf(stderr, "trace LEVELS RECORDING.rec [more]\n");
    return 2;
  }
  int more = argc > 3 ? atoi(argv[3]) : 100;

  recorder_c rec;
  rec.load(argv[2]);
  std::string path = find(argv[1], rec.getLevelsetName(), rec.getLevelName());
  if (path == "") {
    fprintf(stderr, "no level %s / %s\n", rec.getLevelsetName().c_str(), rec.getLevelName().c_str());
    return 1;
  }
  printf("level %s\n", path.c_str());

  std::ifstream file(path.c_str());
  textsections_c sections(file, true);
  levelPlayer_c l;
  l.load(sections, "");
  ant_c a(l);
  // (The game leaves these as they happen to be until they are first
  // set; here they start as nought.)
  a.upChecker = false;
  a.pushAnimation = AntAnimWalkLeft;

  int tick = 0;
  while (!rec.endOfRecord()) {
    int keys = rec.getEvent();
    int result = a.performAnimation(keys);
    line(tick++, keys, result, l, a);
  }
  for (int j = 0; j < more; j++) {
    int result = a.performAnimation(0);
    line(tick++, 0, result, l, a);
  }
  printf("end visible %d fallen %d rubble %d\n", a.isVisible(), l.dominosFalln(), l.rubblePile());
  return 0;
}
