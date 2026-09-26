#ifdef PLATFORM_SDL2
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#define NOMINMAX // global.h has its own
#include <windows.h>
#endif

#include <SDL2/SDL.h>

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "platform/dma.h"
#include "platform/framedraw.h"
#include "platform/settings.h"
#include "platform/system.h"
#include "platform/test_state.h"

extern void (*const gIntrTable[])(void);

SDL_Thread *mainLoopThread;
SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_sem *vBlankSemaphore;
SDL_atomic_t isFrameAvailable;
bool speedUp = false;
unsigned int videoScale = 1;
bool videoScaleChanged = false;
bool isRunning = true;
bool paused = false;
double simTime = 0;
double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
double timeScale = 1.0;
struct SiiRtcInfo internalClock;

static FILE *sSaveFile = NULL;
static const char *sSavePath;
static const char *sSettingsPath;
static char *sDataDir;

// Headless test mode, see RunTestMode()
static bool sTestMode = false;
static FILE *sTestInput;
static FILE *sTestHashes;
static const char *sTestShotDir;
static u32 sTestShotEvery;
static time_t sTestClockBase = 1767268800; // 2026-01-01 12:00:00 UTC
static u32 sTestFrame;
static u64 sTestAudioHash;
static FILE *sTestAudio;
static FILE *sTestState;

extern void AgbMain(void);

int DoMain(void *param);
void ProcessEvents(void);
void VDraw(SDL_Texture *texture);

static void ReadSaveFile(const char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);

static void InitInternalClock(void);
static void UpdateInternalClock(void);

static bool ParseArgs(int argc, char **argv);
static void WriteWavHeader(FILE *file, u32 dataSize);
static void FindDataFiles(void);
static void ToggleFullscreen(void);
static int RunTestMode(void);

int main(int argc, char **argv)
{
    if (!ParseArgs(argc, argv))
        return 1;

    // Open an output console on Windows. The test mode keeps stdout, which the
    // headless tests can read the game's state from.
#ifdef _WIN32
    if (!sTestMode)
    {
        AllocConsole() ;
        AttachConsole( GetCurrentProcessId() ) ;
        freopen( "CON", "w", stdout ) ;
    }
#endif

    // The test mode only uses files it's given, apart from the save
    if (!sTestMode)
        FindDataFiles();
    else if (sSavePath == NULL)
        sSavePath = "pokeemerald.sav";
    LoadSettings(sSettingsPath);
    ReadSaveFile(sSavePath);
    // Before AgbMain, whose RtcInit reads it
    InitInternalClock();

    if (sTestMode)
        return RunTestMode();

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    // Mappings for controllers SDL doesn't know, from https://github.com/mdqinc/SDL_GameControllerDB
    {
        char path[1024];
        FILE *mappings;

        snprintf(path, sizeof(path), "%sgamecontrollerdb.txt", sDataDir);
        mappings = fopen(path, "r");
        if (mappings != NULL)
        {
            fclose(mappings);
            if (SDL_GameControllerAddMappingsFromFile(path) < 0)
                fprintf(stderr, "Could not read %s: %s\n", path, SDL_GetError());
        }
    }

    videoScale = gSettings.scale;
    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (gSettings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    // Whole pixels only, with borders in fullscreen
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);

    sdlTexture = SDL_CreateTexture(sdlRenderer,
                                   SDL_PIXELFORMAT_ABGR1555,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL)
    {
        DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    isFrameAvailable.value = 0;
    vBlankSemaphore = SDL_CreateSemaphore(0);

    SDL_AudioSpec want;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
    cgb_audio_init(want.freq);


    if (SDL_OpenAudio(&want, 0) < 0)
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    else
    {
        if (want.format != AUDIO_F32) /* we let this one thing change. */
            SDL_Log("We didn't get Float32 audio format.");
        SDL_PauseAudio(0);
    }

    AgbMain();

    double accumulator = 0.0;

    bool isGameStepDrawn = false;
    while (isRunning)
    {
        double deltaTime;

        ProcessEvents();

        curGameTime = SDL_GetPerformanceCounter();
        deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
        deltaTime *= timeScale; //apply speedup

        if (!paused)
        {
            accumulator += deltaTime;

            isGameStepDrawn = false;

            while (accumulator >= fixedTimestep)
            {
                //run game logic, draw frame and process DMAs and vblank
                ENTER_VBLANK(); //you must be in VBlank before running a game tick
                if (!RunMainLoop())
                {
                    // Soft reset: drop the old sound, and start the next frame with MainLoop
                    SDL_ClearQueuedAudio(1);
                    accumulator -= fixedTimestep;
                    continue;
                }
                if (!isGameStepDrawn)
                {
                    VDraw(sdlTexture);
                    //SDL_RenderClear(sdlRenderer);
                    isGameStepDrawn = true;
                }
                RunDMAsAndVBlank();

                accumulator -= fixedTimestep;
            }

            //samples per frame is 701, that gets multipled by two when being queued and then multipled by four because samples are float32 which are 4 bytes long hence the divide by 8
            //this number is then checked against samples per frame multipled by three rounded down to 2000 to give it enough margin of error while not desyncing
            //this is all done to sync audio to gameplay
            if (SDL_GetQueuedAudioSize(1)/8 < 2000)
            {
                AudioUpdate();
            }

            if (videoScaleChanged)
            {
                SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
                videoScaleChanged = false;
            }
        }

        lastGameTime = curGameTime;

        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
        SDL_RenderPresent(sdlRenderer);
    }

    CloseSaveFile();

    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}

static void ReadSaveFile(const char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL)
    {
        sSaveFile = fopen(path, "w+b");
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL)
    {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
    }
}

void Platform_StoreSaveFile(void)
{
    StoreSaveFile();
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    DBGPRINTF("ReadFlash(sectorNum=0x%04X,offset=0x%08X,size=0x%02X)\n",sectorNum,offset,size);
    FILE * savefile = fopen(sSavePath, "r+b");
    if (savefile == NULL)
    {
        puts("Error opening save file.");
        return;
    }
    if (fseek(savefile, (sectorNum << gFlash->sector.shift) + offset, SEEK_SET))
    {
        fclose(savefile);
        return;
    }
    if (fread(dest, 1, size, savefile) != size)
    {
        fclose(savefile);
        return;
    }
    fclose(savefile);
}

static u64 HashBytes(u64 hash, const void *data, size_t size)
{
    const u8 *bytes = data;

    // FNV-1a
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    return hash;
}

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    if (sTestMode)
    {
        sTestAudioHash = HashBytes(sTestAudioHash, audioBuffer, samplesPerFrame);
        if (sTestAudio != NULL)
            fwrite(audioBuffer, 1, samplesPerFrame, sTestAudio);
    }
    else
        SDL_QueueAudio(1, audioBuffer, samplesPerFrame);
}


static void CloseSaveFile()
{
    if (sSaveFile != NULL)
    {
        fclose(sSaveFile);
    }
}

// The GBA buttons in the headless test mode, from its input file
static u16 sTestButtons;

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    RequestSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
                }
                break;
            case SDLK_F11:
                ToggleFullscreen();
                break;
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            Input_HandleEvent(&event);
            break;
        case SDL_WINDOWEVENT:
            // Keep the window a whole multiple of the GBA screen
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
             && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN))
            {
                unsigned int w = event.window.data1;
                unsigned int h = event.window.data2;
                
                videoScale = 0;
                if (w / DISPLAY_WIDTH > videoScale)
                    videoScale = w / DISPLAY_WIDTH;
                if (h / DISPLAY_HEIGHT > videoScale)
                    videoScale = h / DISPLAY_HEIGHT;
                if (videoScale < 1)
                    videoScale = 1;

                videoScaleChanged = true;
            }
            break;
        }
    }

    // Fast-forward while it's held
    Input_GetButtons(&speedUp);
    timeScale = speedUp ? 5.0 : 1.0;
}

static void ToggleFullscreen(void)
{
    bool fullscreen = !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN);

    SDL_SetWindowFullscreen(sdlWindow, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

u16 Platform_GetKeyInput(void)
{
    if (sTestMode)
        return sTestButtons;
    return Input_GetButtons(NULL);
}

void VDraw(SDL_Texture *texture)
{
    static uint16_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    memset(image, 0, sizeof(image));
    DrawFrame(image);
    SDL_UpdateTexture(texture, NULL, image, DISPLAY_WIDTH * sizeof (Uint16));
    REG_VCOUNT = 161; // prep for being in VBlank period
}

int DoMain(void *data)
{
    AgbMain();
}

void VBlankIntrWait(void)
{
    return;
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do
    {
        out |= (bin % 10) * placeCounter;
        placeCounter *= 16;
    }
    while ((bin /= 10) > 0);

    return out;
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = internalClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    internalClock.status = rtc->status;
}

static void InitInternalClock(void)
{
    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();
}

static void UpdateInternalClock(void)
{
    struct tm *now;

    if (sTestMode)
    {
        // Deterministic clock: advances one second every 60 frames
        time_t rawTime = sTestClockBase + sTestFrame / 60;
        now = gmtime(&rawTime);
    }
    else
    {
        time_t rawTime = time(NULL);
        now = localtime(&rawTime);
    }

    internalClock.year = BinToBcd(now->tm_year - 100);
    internalClock.month = BinToBcd(now->tm_mon + 1);
    internalClock.day = BinToBcd(now->tm_mday);
    internalClock.dayOfWeek = BinToBcd(now->tm_wday);
    internalClock.hour = BinToBcd(now->tm_hour);
    internalClock.minute = BinToBcd(now->tm_min);
    internalClock.second = BinToBcd(now->tm_sec);
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->year = internalClock.year;
    rtc->month = internalClock.month;
    rtc->day = internalClock.day;
    rtc->dayOfWeek = internalClock.dayOfWeek;
    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetDateTime: %d-%02d-%02d %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->year),
                                                         ConvertBcdToBinary(rtc->month),
                                                         ConvertBcdToBinary(rtc->day),
                                                         ConvertBcdToBinary(rtc->hour),
                                                         ConvertBcdToBinary(rtc->minute),
                                                         ConvertBcdToBinary(rtc->second));
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    internalClock.month = rtc->month;
    internalClock.day = rtc->day;
    internalClock.dayOfWeek = rtc->dayOfWeek;
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetTime: %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->hour),
                                        ConvertBcdToBinary(rtc->minute),
                                        ConvertBcdToBinary(rtc->second));
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_SetAlarm(u8 *alarmData)
{
    // The game never sets an alarm
}

// All options take a value. Anything else is ignored, like before there were
// options, so e.g. a file dropped onto the executable doesn't stop the game.
static bool ParseArgs(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strncmp(arg, "--", 2) != 0 || value == NULL)
        {
            fprintf(stderr, "Ignoring argument: %s\n", arg);
            continue;
        }
        i++;

        if (strcmp(arg, "--save") == 0)
        {
            sSavePath = value;
        }
        else if (strcmp(arg, "--config") == 0)
        {
            sSettingsPath = value;
        }
        else if (strcmp(arg, "--test-input") == 0)
        {
            sTestMode = true;
            sTestInput = (strcmp(value, "-") == 0) ? stdin : fopen(value, "r");
            if (sTestInput == NULL)
            {
                fprintf(stderr, "Could not open test input %s\n", value);
                return false;
            }
        }
        else if (strcmp(arg, "--test-hashes") == 0)
        {
            sTestHashes = fopen(value, "w");
            if (sTestHashes == NULL)
            {
                fprintf(stderr, "Could not open hash output %s\n", value);
                return false;
            }
        }
        else if (strcmp(arg, "--test-shots") == 0)
        {
            sTestShotDir = value;
        }
        else if (strcmp(arg, "--test-state") == 0)
        {
            sTestState = (strcmp(value, "-") == 0) ? stdout : fopen(value, "w");
            if (sTestState == NULL)
            {
                fprintf(stderr, "Could not open state output %s\n", value);
                return false;
            }
        }
        else if (strcmp(arg, "--test-audio") == 0)
        {
            sTestAudio = fopen(value, "wb");
            if (sTestAudio == NULL)
            {
                fprintf(stderr, "Could not open audio output %s\n", value);
                return false;
            }
            WriteWavHeader(sTestAudio, 0);
        }
        else if (strcmp(arg, "--test-shot-every") == 0)
        {
            sTestShotEvery = strtoul(value, NULL, 10);
        }
        else if (strcmp(arg, "--test-time") == 0)
        {
            sTestClockBase = strtoll(value, NULL, 10);
        }
        else
        {
            fprintf(stderr, "Ignoring unknown option: %s %s\n", arg, value);
        }
    }
    return true;
}

// The save and the settings go in the user's data folder, unless there's a save
// in the current directory, where earlier versions kept it
static void FindDataFiles(void)
{
    static char savePath[1024], settingsPath[1024];
    FILE *oldSave = fopen("pokeemerald.sav", "rb");

    if (oldSave != NULL)
        fclose(oldSave);
    else
        sDataDir = SDL_GetPrefPath("", "pokeemerald");
    if (sDataDir == NULL)
        sDataDir = SDL_strdup("");

    if (sSavePath == NULL)
    {
        snprintf(savePath, sizeof(savePath), "%spokeemerald.sav", sDataDir);
        sSavePath = savePath;
    }
    if (sSettingsPath == NULL)
    {
        snprintf(settingsPath, sizeof(settingsPath), "%spokeemerald.ini", sDataDir);
        sSettingsPath = settingsPath;
    }
    printf("Save file: %s\nSettings: %s\n", sSavePath, sSettingsPath);
}

// Reads the keys to hold for the next frame from the test input. Each line is
// "<frames> <buttons>", where buttons is "-" or names joined by '+', e.g.
// "30 A+UP". Besides the GBA buttons, names can be keys and controller inputs,
// which go through the bindings in the settings, like "key:Z" or "pad:lefty-".
// Lines starting with '#' are ignored. Returns false at the end.
static bool ReadTestInput(u16 *outKeys)
{
    static const struct { const char *name; u16 key; } sButtons[] = {
        {"A", A_BUTTON}, {"B", B_BUTTON}, {"SELECT", SELECT_BUTTON}, {"START", START_BUTTON},
        {"RIGHT", DPAD_RIGHT}, {"LEFT", DPAD_LEFT}, {"UP", DPAD_UP}, {"DOWN", DPAD_DOWN},
        {"R", R_BUTTON}, {"L", L_BUTTON},
    };
    static u32 sHoldFrames;
    static u16 sHoldKeys;
    static bool sStateWritten;
    char line[256];

    while (sHoldFrames == 0)
    {
        char buttons[200];

        // The state after the last line's frames, for a driver that decides
        // what to press next from it (see test/headless/driver.py)
        if (sTestState != NULL && !sStateWritten)
        {
            WriteTestState(sTestState, sTestFrame);
            sStateWritten = true;
        }
        if (fgets(line, sizeof(line), sTestInput) == NULL)
            return false;
        if (line[0] == '#' || sscanf(line, "%u %199s", &sHoldFrames, buttons) != 2)
        {
            sHoldFrames = 0;
            continue;
        }

        sStateWritten = false;
        sHoldKeys = 0;
        Input_ReleaseTestInputs();
        for (char *name = strtok(buttons, "+"); name != NULL; name = strtok(NULL, "+"))
        {
            size_t i;

            if (strcmp(name, "-") == 0 || Input_PressTestInput(name))
                continue;
            for (i = 0; i < ARRAY_COUNT(sButtons); i++)
            {
                if (strcmp(name, sButtons[i].name) == 0)
                {
                    sHoldKeys |= sButtons[i].key;
                    break;
                }
            }
            if (i == ARRAY_COUNT(sButtons))
                fprintf(stderr, "Unknown button in test input: %s\n", name);
        }
    }

    sHoldFrames--;
    *outKeys = sHoldKeys | Input_GetTestButtons();
    return true;
}

// A WAV file header for the test mode's audio: 32-bit float stereo, with
// dataSize bytes of samples after it
static void WriteWavHeader(FILE *file, u32 dataSize)
{
    u8 header[44];
    u32 fields[] = {36 + dataSize, 16, 3 | (2 << 16), AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 8, 8 | (32 << 16), dataSize};

    memcpy(header, "RIFF", 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    memcpy(header + 36, "data", 4);
    // Little-endian, like every platform the port runs on
    memcpy(header + 4, &fields[0], 4);
    memcpy(header + 16, &fields[1], 16);
    memcpy(header + 32, &fields[5], 4);
    memcpy(header + 40, &fields[6], 4);
    fwrite(header, 1, sizeof(header), file);
}

static void WriteTestScreenshot(const uint16_t *image)
{
    char path[512];
    FILE *file;

    snprintf(path, sizeof(path), "%s/frame_%06u.ppm", sTestShotDir, sTestFrame);
    file = fopen(path, "wb");
    if (file == NULL)
    {
        fprintf(stderr, "Could not write screenshot %s\n", path);
        return;
    }

    fprintf(file, "P6\n%d %d\n255\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        // ABGR1555
        u8 rgb[3] = {
            (image[i] & 0x1F) << 3,
            ((image[i] >> 5) & 0x1F) << 3,
            ((image[i] >> 10) & 0x1F) << 3,
        };
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
}

// Runs the game without a window or audio device until the test input ends.
// Every frame runs one audio update and uses a fixed clock, so the output only
// depends on the input and the save file. Writes "<frame> <video hash> <audio
// hash>" per frame, which lets two builds be compared frame by frame.
static int RunTestMode(void)
{
    static uint16_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    cgb_audio_init(AUDIO_SAMPLE_RATE);
    AgbMain();

    for (sTestFrame = 0; ReadTestInput(&sTestButtons); sTestFrame++)
    {
        bool ranFrame;

        ENTER_VBLANK();
        ranFrame = RunMainLoop();
        memset(image, 0, sizeof(image));
        DrawFrame(image);
        REG_VCOUNT = 161;
        sTestAudioHash = 0xCBF29CE484222325ull;
        // After a soft reset, the next frame starts with MainLoop, like at startup
        if (ranFrame)
        {
            RunDMAsAndVBlank();
            AudioUpdate();
        }

        if (sTestHashes != NULL)
            fprintf(sTestHashes, "%u %016llx %016llx\n", sTestFrame,
                    (unsigned long long)HashBytes(0xCBF29CE484222325ull, image, sizeof(image)),
                    (unsigned long long)sTestAudioHash);
        if (sTestShotDir != NULL && sTestShotEvery != 0 && sTestFrame % sTestShotEvery == 0)
            WriteTestScreenshot(image);
    }

    if (sTestShotDir != NULL)
        WriteTestScreenshot(image);
    if (sTestHashes != NULL)
        fclose(sTestHashes);
    if (sTestAudio != NULL)
    {
        long size = ftell(sTestAudio);

        fseek(sTestAudio, 0, SEEK_SET);
        WriteWavHeader(sTestAudio, size - 44);
        fclose(sTestAudio);
    }
    fclose(sTestInput);
    CloseSaveFile();
    return 0;
}

#endif