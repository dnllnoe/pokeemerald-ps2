#ifndef GUARD_SYSTEM_H
#define GUARD_SYSTEM_H

// The rate the platform mixes the game's sound at
#define AUDIO_SAMPLE_RATE 42048

void RunDMAsAndVBlank(void);
void AudioUpdate(void);
bool8 RunMainLoop(void);
void RequestSoftReset(void);
#define ENTER_VBLANK() REG_VCOUNT = 161
#endif