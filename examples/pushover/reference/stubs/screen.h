// In place of the game's screen.h: of the screen the logic wants only
// the function that would translate a text.
#ifndef REFERENCE_SCREEN_H
#define REFERENCE_SCREEN_H
#include <SDL.h>
#include <string>
inline const std::string _(const std::string & x) { return x; }
inline const char * _(const char * x) { return x; }
#endif
