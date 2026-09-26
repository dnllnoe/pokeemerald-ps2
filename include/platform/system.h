#ifndef GUARD_SYSTEM_H
#define GUARD_SYSTEM_H

// The rate the game's sound gets played at: the mixer makes 701 samples a
// frame (see SampleFreqSet), at 60 frames a second
#define AUDIO_SAMPLE_RATE (60 * 701)

void RunDMAsAndVBlank(void);
void AudioUpdate(void);
bool8 RunMainLoop(void);
void RequestSoftReset(void);
#define ENTER_VBLANK() REG_VCOUNT = 161
#endif