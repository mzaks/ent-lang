// What the game's logic names of SDL, where nothing is drawn and no key
// is read.
#ifndef REFERENCE_SDL_H
#define REFERENCE_SDL_H
#include <stdint.h>
typedef uint8_t Uint8;
enum { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_SPACE, SDLK_LAST };
inline Uint8 * SDL_GetKeyState(int *) { static Uint8 none[SDLK_LAST]; return none; }
#endif
