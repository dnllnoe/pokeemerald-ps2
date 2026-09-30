#ifdef PORTABLE
#include "global.h"
#include "cgb_audio.h"

// The GBA samples the Game Boy channels at 65536 Hz (with SOUNDBIAS set up the
// way the sound engine does it), so they're made at that rate here too, from
// the channels' CPU cycle timings, and then filtered down to the output rate.
// Point sampling the channels at the output rate aliases their harmonics to
// frequencies that aren't in the tune, which garbles high notes.
#define APU_RATE 65536
#define CYCLES_PER_SAMPLE 64    // of the 4194304 Hz CPU clock
#define SEQUENCER_SAMPLES 128   // the frame sequencer steps at 512 Hz

// The resampling filter: TAPS input samples per output sample, with its
// coefficients worked out for PHASES positions between input samples
#ifdef __PS2__
// Half the taps, which is half the work, for a softer cutoff: the full filter
// alone takes a fifth of the PS2's time
#define TAPS 32
#else
#define TAPS 64
#endif
#define PHASES 512
#define HIGH_RATE_CAPACITY 2048

static struct AudioCGB gb;
static u32 sampleRate;

static float sCoefficients[PHASES + 1][TAPS];
static bool8 sCoefficientsReady;
static float sHighRate[HIGH_RATE_CAPACITY][2];
static u32 sHighRateCount;
// Where the next output sample is in sHighRate: the sample before it, and how
// far past that one it is in units of 1/sampleRate samples. Kept apart instead
// of as one count, which would need a 64-bit division every sample, and that's
// slow on 32-bit CPUs without one, like the PS2's.
static u32 sResampleIndex;
static u32 sResampleFrac;

// The duty cycles' steps, with each at the same level on average, as the GBA's
// output has its DC removed, so a volume change doesn't click
static const float sDutyLevels[4][8] = {
    {-0.25f, -0.25f, -0.25f, -0.25f, -0.25f, -0.25f, -0.25f, 1.75f},  // 12.5%
    {1.5f, -0.5f, -0.5f, -0.5f, -0.5f, -0.5f, -0.5f, 1.5f},           // 25%
    {1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f},             // 50%
    {-1.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -1.5f},               // 75%
};

#define PI 3.14159265358979323846

// sin(x) worked out the same way everywhere, unlike libm's
static double Sine(double x){
    x -= 2 * PI * (double)(s64)(x / (2 * PI) + (x >= 0 ? 0.5 : -0.5));
    double term = x, sum = x;
    for(int n = 1; n < 14; n++){
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

// A windowed sinc low-pass, cutting off below the output's Nyquist frequency
static void MakeCoefficients(void){
    double cutoff = 0.45 * sampleRate / APU_RATE;  // in cycles per input sample

    for(int phase = 0; phase <= PHASES; phase++){
        double sum = 0;
        for(int tap = 0; tap < TAPS; tap++){
            // How far the tap's input sample is from the output sample
            double t = (double)phase / PHASES + TAPS / 2 - 1 - tap;
            double window = 0.42 + 0.5 * Sine(PI * t / (TAPS / 2) + PI / 2) + 0.08 * Sine(2 * PI * t / (TAPS / 2) + PI / 2);
            double sinc = t == 0 ? 1 : Sine(2 * PI * cutoff * t) / (2 * PI * cutoff * t);
            if(t <= -TAPS / 2 || t >= TAPS / 2)
                window = 0;
            sCoefficients[phase][tap] = window * sinc;
            sum += window * sinc;
        }
        for(int tap = 0; tap < TAPS; tap++)
            sCoefficients[phase][tap] /= sum;
    }
}

void cgb_audio_init(u32 rate){
    // Everything starts from zero, also after a soft reset
    memset(&gb, 0, sizeof(gb));
    gb.lfsr = 0x7FFF;
    gb.sequencerSamples = SEQUENCER_SAMPLES;
    if(!sCoefficientsReady || rate != sampleRate){
        sampleRate = rate;
        MakeCoefficients();
        sCoefficientsReady = TRUE;
    }
    // Silence to start, so the filter has what came before the first sample
    memset(sHighRate, 0, sizeof(sHighRate));
    sHighRateCount = TAPS / 2 - 1;
    sResampleIndex = TAPS / 2 - 1;
    sResampleFrac = 0;
}


void cgb_set_sweep(u8 sweep){
    gb.ch1SweepDir = (sweep & 0x08) >> 3;
    gb.ch1SweepCounter = gb.ch1SweepCounterI = (sweep & 0x70) >> 4;
    gb.ch1SweepShift = (sweep & 0x07);
}


void cgb_set_wavram(){
    for(u8 wavi = 0; wavi < 0x10; wavi++){
        gb.wave[(wavi << 1)] = ((*(REG_ADDR_WAVE_RAM0 + wavi)) & 0xF0) >> 4;
        gb.wave[(wavi << 1) + 1] = (*(REG_ADDR_WAVE_RAM0 + wavi)) & 0x0F;
    }
}


void cgb_toggle_length(u8 channel, bool8 state){
    gb.LenOn[channel] = state;
}


// The length written counts up to 64 (256 for the wave channel)
void cgb_set_length(u8 channel, u8 length){
    gb.Len[channel] = (channel == 2 ? 256 : 64) - length;
}


void cgb_set_envelope(u8 channel, u8 envelope){
    if(channel == 2){
        switch((envelope & 0xE0)){
            case 0x00:  // mute
                gb.Vol[2] = gb.VolI[2] = 0;
            break;
            case 0x20:  // full
                gb.Vol[2] = gb.VolI[2] = 4;
            break;
            case 0x40:  // half
                gb.Vol[2] = gb.VolI[2] = 2;
            break;
            case 0x60:  // quarter
                gb.Vol[2] = gb.VolI[2] = 1;
            break;
            case 0x80:  // 3 quarters
                gb.Vol[2] = gb.VolI[2] = 3;
            break;
        }
    }else{
        gb.DAC[channel] = (envelope & 0xF8) > 0;
        gb.Vol[channel] = gb.VolI[channel] = (envelope & 0xF0) >> 4;
        gb.EnvDir[channel] = (envelope & 0x08) >> 3;
        gb.EnvCounter[channel] = gb.EnvCounterI[channel] = (envelope & 0x07);
    }
}


static u32 SquarePeriod(u16 freq){
    return (2048 - (freq & 0x7FF)) * 4;
}

static u32 WavePeriod(void){
    return (2048 - (REG_SOUND3CNT_X & 0x7FF)) * 2;
}

// 0 if the noise channel doesn't clock at all
static u32 NoisePeriod(void){
    u8 ratio = REG_NR43 & 7;
    u8 shift = REG_NR43 >> 4;
    if(shift >= 14)
        return 0;
    return (ratio ? ratio * 16 : 8) << shift;
}


// What writing NRx4 with bit 7 set does
void cgb_trigger_note(u8 channel){
    REG_NR52 |= 1 << channel;
    if(gb.Len[channel] == 0)
        gb.Len[channel] = channel == 2 ? 256 : 64;
    gb.Vol[channel] = gb.VolI[channel];
    switch(channel){
        case 0:
            gb.timer[0] = SquarePeriod(REG_SOUND1CNT_X);
            gb.EnvCounter[0] = gb.EnvCounterI[0];
        break;
        case 1:
            gb.timer[1] = SquarePeriod(REG_SOUND2CNT_H);
            gb.EnvCounter[1] = gb.EnvCounterI[1];
        break;
        case 2:
            gb.timer[2] = WavePeriod();
            gb.position[2] = 0;
        break;
        case 3:
            gb.timer[3] = NoisePeriod();
            gb.EnvCounter[3] = gb.EnvCounterI[3];
            gb.lfsr = 0x7FFF;
        break;
    }
}


// The length counters at 256 Hz, the sweep at 128 Hz and the envelopes at 64 Hz
static void StepSequencer(void){
    u8 step = gb.sequencerStep;
    gb.sequencerStep = (step + 1) & 7;

    if((step & 1) == 0){  // Length
        for(u8 ch = 0; ch < 4; ch++){
            if(gb.LenOn[ch] && gb.Len[ch]){
                if(--gb.Len[ch] == 0){
                    REG_NR52 &= (0xFF ^ (1 << ch));
                }
            }
        }
    }

    if(step == 7){  // Envelope
        for(u8 ch = 0; ch < 4; ch++){
            if(ch == 2) continue;  // Skip wave channel
            if(gb.EnvCounter[ch]){
                if(--gb.EnvCounter[ch] == 0){
                    if(gb.Vol[ch] && !gb.EnvDir[ch]){
                        gb.Vol[ch]--;
                        gb.EnvCounter[ch] = gb.EnvCounterI[ch];
                    }else if(gb.Vol[ch] < 0x0F && gb.EnvDir[ch]){
                        gb.Vol[ch]++;
                        gb.EnvCounter[ch] = gb.EnvCounterI[ch];
                    }
                }
            }
        }
    }

    if((step & 3) == 2){  // Sweep
        if(gb.ch1SweepCounterI && gb.ch1SweepShift){
            if(--gb.ch1SweepCounter == 0){
                gb.ch1Freq = REG_SOUND1CNT_X & 0x7FF;
                if(gb.ch1SweepDir){
                    gb.ch1Freq -= gb.ch1Freq >> gb.ch1SweepShift;
                    if(gb.ch1Freq & 0xF800) gb.ch1Freq = 0;
                }else{
                    gb.ch1Freq += gb.ch1Freq >> gb.ch1SweepShift;
                    if(gb.ch1Freq & 0xF800){
                        gb.ch1Freq = 0;
                        gb.EnvCounter[0] = 0;
                        gb.Vol[0] = 0;
                    }
                }
                REG_NR13 = gb.ch1Freq & 0xFF;
                REG_NR14 &= 0xF8;
                REG_NR14 += (gb.ch1Freq >> 8) & 0x07;
                gb.ch1SweepCounter = gb.ch1SweepCounterI;
            }
        }
    }
}


// Runs a channel's timer for one sample, returning how many times it ran out
static u32 RunTimer(u8 channel, u32 period){
    u32 over;

    if(period == 0)
        return 0;
    if(gb.timer[channel] > CYCLES_PER_SAMPLE){
        gb.timer[channel] -= CYCLES_PER_SAMPLE;
        return 0;
    }
    over = CYCLES_PER_SAMPLE - gb.timer[channel];
    gb.timer[channel] = period - over % period;
    return 1 + over / period;
}


// The wave channel's sample at its volume, which the hardware does by shifting
// the 4-bit samples, centered like the other channels
static float WaveLevel(u8 sample){
    switch(gb.Vol[2]){
        case 4:  // full
            return (2 * sample - 15) / 15.0f;
        case 3:  // 3 quarters
            return (2 * ((sample * 3) >> 2) - 11) / 15.0f;
        case 2:  // half
            return (2 * (sample >> 1) - 7) / 15.0f;
        case 1:  // quarter
            return (2 * (sample >> 2) - 3) / 15.0f;
        default:
            return 0.0f;
    }
}


// One 65536 Hz sample of all four channels
static void GenerateSample(float *out){
    float outputL = 0;
    float outputR = 0;
    float level;

    if(--gb.sequencerSamples == 0){
        gb.sequencerSamples = SEQUENCER_SAMPLES;
        StepSequencer();
    }

    gb.position[0] = (gb.position[0] + RunTimer(0, SquarePeriod(REG_SOUND1CNT_X))) & 7;
    gb.position[1] = (gb.position[1] + RunTimer(1, SquarePeriod(REG_SOUND2CNT_H))) & 7;
    gb.position[2] = (gb.position[2] + RunTimer(2, WavePeriod())) & 31;
    for(u32 clocks = RunTimer(3, NoisePeriod()); clocks > 0; clocks--){
        u16 bit = (gb.lfsr ^ (gb.lfsr >> 1)) & 1;
        gb.lfsr = (gb.lfsr >> 1) | (bit << 14);
        if(REG_NR43 & 0x08)  // 7-bit mode
            gb.lfsr = (gb.lfsr & ~0x40) | (bit << 6);
    }

    if(REG_NR52 & 0x80){
        if((gb.DAC[0]) && (REG_NR52 & 0x01)){
            level = gb.Vol[0] * sDutyLevels[REG_NR11 >> 6][gb.position[0]] / 15.0f;
            if(REG_NR51 & 0x10) outputL += level;
            if(REG_NR51 & 0x01) outputR += level;
        }
        if((gb.DAC[1]) && (REG_NR52 & 0x02)){
            level = gb.Vol[1] * sDutyLevels[REG_NR21 >> 6][gb.position[1]] / 15.0f;
            if(REG_NR51 & 0x20) outputL += level;
            if(REG_NR51 & 0x02) outputR += level;
        }
        if((REG_NR30 & 0x80) && (REG_NR52 & 0x04)){
            level = WaveLevel(gb.wave[gb.position[2]]);
            if(REG_NR51 & 0x40) outputL += level;
            if(REG_NR51 & 0x04) outputR += level;
        }
        if((gb.DAC[3]) && (REG_NR52 & 0x08)){
            level = gb.Vol[3] * ((gb.lfsr & 1) ? -1.0f : 1.0f) / 15.0f;
            if(REG_NR51 & 0x80) outputL += level;
            if(REG_NR51 & 0x08) outputR += level;
        }
    }
    out[0] = outputL / 4.0f;
    out[1] = outputR / 4.0f;
}


void cgb_audio_generate(u16 samplesPerFrame){
    float *outBuffer = gb.outBuffer;

    for(u16 i = 0; i < samplesPerFrame; i++, outBuffer += 2){
        u32 center = sResampleIndex;
        u32 first = center - (TAPS / 2 - 1);
        const float *coefficients;
        float outputL = 0;
        float outputR = 0;

        if(center + TAPS / 2 >= HIGH_RATE_CAPACITY){
            // Drop the samples that have been filtered for the last time
            memmove(sHighRate, sHighRate[first], (sHighRateCount - first) * sizeof(sHighRate[0]));
            sHighRateCount -= first;
            sResampleIndex -= first;
            first = 0;
        }
        while(sHighRateCount < first + TAPS)
            GenerateSample(sHighRate[sHighRateCount++]);

        coefficients = sCoefficients[(sResampleFrac * PHASES + sampleRate / 2) / sampleRate];
        for(u32 tap = 0; tap < TAPS; tap++){
            outputL += coefficients[tap] * sHighRate[first + tap][0];
            outputR += coefficients[tap] * sHighRate[first + tap][1];
        }
        outBuffer[0] = outputL;
        outBuffer[1] = outputR;
        sResampleFrac += APU_RATE;
        while(sResampleFrac >= sampleRate){
            sResampleFrac -= sampleRate;
            sResampleIndex++;
        }
    }
}


float *cgb_get_buffer(){
    return gb.outBuffer;
}
#endif //PORTABLE
