#ifndef CGB_AUDIO_H
#define CGB_AUDIO_H

#define MIXED_AUDIO_BUFFER_SIZE 4907

// The state of the emulated Game Boy sound channels: two squares (the first
// with sweep), the wave channel and noise
struct AudioCGB{
    u16 ch1Freq;
    u8 ch1SweepCounter;
    u8 ch1SweepCounterI;
    bool8 ch1SweepDir;
    u8 ch1SweepShift;
    u8 Vol[4];
    u8 VolI[4];
    u16 Len[4];
    bool8 LenOn[4];
    u8 EnvCounter[4];
    u8 EnvCounterI[4];
    bool8 EnvDir[4];
    bool8 DAC[4];
    u8 wave[32];           // the wave channel's 4-bit samples
    u32 timer[4];          // CPU cycles until each channel's next step
    u8 position[3];        // the step of the squares' duty cycles, and of the wave
    u16 lfsr;              // the noise channel's shift register
    u8 sequencerSamples;   // samples until the next step of the 512 Hz frame sequencer
    u8 sequencerStep;
    __attribute__((aligned(4))) float outBuffer[MIXED_AUDIO_BUFFER_SIZE * 2];
};

void cgb_audio_init(u32 rate);
void cgb_set_sweep(u8 sweep);
void cgb_set_wavram();
void cgb_toggle_length(u8 channel, bool8 state);
void cgb_set_length(u8 channel, u8 length);
void cgb_set_envelope(u8 channel, u8 envelope);
void cgb_trigger_note(u8 channel);
void cgb_audio_generate(u16 samplesPerFrame);
float *cgb_get_buffer();

#endif
