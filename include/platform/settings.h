#ifndef GUARD_PLATFORM_SETTINGS_H
#define GUARD_PLATFORM_SETTINGS_H

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "gba/types.h"

struct Settings
{
    int scale;
    bool fullscreen;
};

extern struct Settings gSettings;

// Reads the settings file. If it doesn't exist, it gets written with the defaults.
void LoadSettings(const char *path);

// Opens and closes controllers as they come and go
void Input_HandleEvent(const SDL_Event *event);
// The GBA buttons held on the keyboard and the controllers, as KEYINPUT bits
u16 Input_GetButtons(bool *speedUp);

// The headless test mode's keyboard and controller. Input names are "key:" and
// a key, or "pad:" and a controller input, like in the settings file.
void Input_ReleaseTestInputs(void);
bool Input_PressTestInput(const char *name);
u16 Input_GetTestButtons(void);

#endif // GUARD_PLATFORM_SETTINGS_H
