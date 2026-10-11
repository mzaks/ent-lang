// In place of the game's soundsys.h: the sounds a tick starts are kept
// in a list, for the trace.
#ifndef REFERENCE_SOUNDSYS_H
#define REFERENCE_SOUNDSYS_H
#include <vector>
class soundSystem_c {
  public:
    enum {
      SE_STANDARD, SE_STOPPER, SE_SPLITTER, SE_EXPLODER, SE_DELAY,
      SE_TUMBLER, SE_BRIDGER, SE_VANISH, SE_TRIGGER, SE_ASCENDER,
      SE_ANT_FALLING, SE_ANT_LANDING, SE_PICK_UP_DOMINO, SE_NU_WHAT,
      SE_SHRUGGING, SE_DOOR_CLOSE, SE_DOOR_OPEN, SE_VICTORY,
    };
    std::vector<unsigned int> started;
    void startSound(unsigned int snd) { started.push_back(snd); }
    static soundSystem_c * instance(void) { static soundSystem_c it; return &it; }
};
#endif
