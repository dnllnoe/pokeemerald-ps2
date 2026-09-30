#ifndef GUARD_SYSTEM_H
#define GUARD_SYSTEM_H

// The rate the game's sound gets played at: the mixer makes this many samples
// a frame (see SampleFreqSet), at 60 frames a second. The PS2 mixes at 24000 Hz,
// which its sound driver plays as it is, where 42060 Hz would need resampling,
// and it takes little more than half the time. That's still well over the
// 13379 Hz the GBA mixes at.
#ifdef __PS2__
#define AUDIO_SAMPLES_PER_FRAME 400
#else
#define AUDIO_SAMPLES_PER_FRAME 701
#endif
#define AUDIO_SAMPLE_RATE (60 * AUDIO_SAMPLES_PER_FRAME)

void RunDMAsAndVBlank(void);
void AudioUpdate(void);
bool8 RunMainLoop(void);
void RequestSoftReset(void);
#define ENTER_VBLANK() REG_VCOUNT = 161
#endif