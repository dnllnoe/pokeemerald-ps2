#ifdef PLATFORM_SDL2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "global.h"
#include "platform/settings.h"

// What the keyboard and controllers can do: the GBA buttons, in the order of
// their bits in KEYINPUT, and fast-forward
enum
{
    BINDING_A,
    BINDING_B,
    BINDING_SELECT,
    BINDING_START,
    BINDING_RIGHT,
    BINDING_LEFT,
    BINDING_UP,
    BINDING_DOWN,
    BINDING_R,
    BINDING_L,
    BINDING_SPEEDUP,
    NUM_BINDINGS
};

static const char *const sBindingNames[NUM_BINDINGS] = {
    "a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "speedup",
};

#define MAX_BINDING_INPUTS 4
#define MAX_CONTROLLERS 4
// How far a trigger or stick has to go to count as pressed
#define AXIS_THRESHOLD 16384

// A controller button, or an axis in a direction: 1 for the triggers, and 1
// or -1 for the sticks
struct PadInput
{
    bool isAxis;
    s8 direction;
    u8 index;
};

struct Binding
{
    SDL_Scancode keys[MAX_BINDING_INPUTS];
    struct PadInput pads[MAX_BINDING_INPUTS];
    u8 numKeys;
    u8 numPads;
};

struct PadState
{
    bool buttons[SDL_CONTROLLER_BUTTON_MAX];
    Sint16 axes[SDL_CONTROLLER_AXIS_MAX];
};

// The default settings, which also get written to a new settings file. They're
// read before the settings file, so a setting the file leaves out keeps them.
static const char sDefaultSettings[] =
    "# Settings for pokeemerald. Each line is \"name = value\", and lines starting\n"
    "# with # are comments. Delete this file to get the defaults back.\n"
    "\n"
    "# The window's size, as a multiple of the GBA's 240x160 screen\n"
    "scale = 3\n"
    "# Start in fullscreen. F11 or Alt+Enter switches while playing.\n"
    "fullscreen = false\n"
    "\n"
    "# The keys for each button, separated by commas. Keys are named for where they\n"
    "# are on a US keyboard: letters, digits, Return, Space, Backspace, Tab, Up,\n"
    "# Down, Left, Right, Left Shift, Right Ctrl, Keypad 8 and so on (SDL's names).\n"
    "key_a = Z\n"
    "key_b = X\n"
    "key_start = Return\n"
    "key_select = \\, Backspace\n"
    "key_l = A\n"
    "key_r = S\n"
    "key_up = Up\n"
    "key_down = Down\n"
    "key_left = Left\n"
    "key_right = Right\n"
    "key_speedup = Space\n"
    "\n"
    "# The controller inputs for each button: a, b, x and y (where they are on an\n"
    "# Xbox controller), back, start, guide, leftshoulder, rightshoulder,\n"
    "# leftstick, rightstick, dpup, dpdown, dpleft, dpright, lefttrigger,\n"
    "# righttrigger, and the sticks' directions: leftx- (left), leftx+ (right),\n"
    "# lefty- (up) and lefty+ (down), and the same for rightx and righty.\n"
    "pad_a = a\n"
    "pad_b = b, x\n"
    "pad_start = start\n"
    "pad_select = back\n"
    "pad_l = leftshoulder\n"
    "pad_r = rightshoulder\n"
    "pad_up = dpup, lefty-\n"
    "pad_down = dpdown, lefty+\n"
    "pad_left = dpleft, leftx-\n"
    "pad_right = dpright, leftx+\n"
    "pad_speedup = righttrigger\n";

struct Settings gSettings;
static struct Binding sBindings[NUM_BINDINGS];
static SDL_GameController *sControllers[MAX_CONTROLLERS];
static Uint8 sTestKeyboard[SDL_NUM_SCANCODES];
static struct PadState sTestPad;

static char *Trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t')
        text++;
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    *end = '\0';
    return text;
}

static bool ParseKey(const char *name, SDL_Scancode *key)
{
    *key = SDL_GetScancodeFromName(name);
    return *key != SDL_SCANCODE_UNKNOWN;
}

// A button, a trigger, or a stick's direction, like "leftx-"
static bool ParsePadInput(const char *name, struct PadInput *input)
{
    char axisName[32];
    size_t length = strlen(name);
    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(name);
    SDL_GameControllerAxis axis;

    if (button != SDL_CONTROLLER_BUTTON_INVALID)
    {
        input->isAxis = false;
        input->direction = 0;
        input->index = button;
        return true;
    }

    input->isAxis = true;
    input->direction = 1;
    if (length > 1 && length < sizeof(axisName) && (name[length - 1] == '+' || name[length - 1] == '-'))
    {
        input->direction = (name[length - 1] == '-') ? -1 : 1;
        memcpy(axisName, name, length - 1);
        axisName[length - 1] = '\0';
        name = axisName;
    }
    axis = SDL_GameControllerGetAxisFromString(name);
    if (axis == SDL_CONTROLLER_AXIS_INVALID)
        return false;
    input->index = axis;
    return true;
}

static void ParseBinding(struct Binding *binding, bool isKey, char *value, const char *source, int line)
{
    u8 count = 0;

    for (char *item = strtok(value, ","); item != NULL; item = strtok(NULL, ","))
    {
        item = Trim(item);
        if (*item == '\0')
            continue;
        if (count == MAX_BINDING_INPUTS)
        {
            fprintf(stderr, "%s:%d: only %d inputs per button\n", source, line, MAX_BINDING_INPUTS);
            break;
        }
        if (isKey ? ParseKey(item, &binding->keys[count]) : ParsePadInput(item, &binding->pads[count]))
            count++;
        else
            fprintf(stderr, "%s:%d: unknown %s \"%s\"\n", source, line, isKey ? "key" : "controller input", item);
    }

    if (isKey)
        binding->numKeys = count;
    else
        binding->numPads = count;
}

static void ParseSetting(const char *name, char *value, const char *source, int line)
{
    if (strcmp(name, "scale") == 0)
    {
        char *end;
        long scale = strtol(value, &end, 10);

        if (*value != '\0' && *end == '\0' && scale >= 1 && scale <= 16)
            gSettings.scale = scale;
        else
            fprintf(stderr, "%s:%d: the scale has to be a number from 1 to 16\n", source, line);
    }
    else if (strcmp(name, "fullscreen") == 0)
    {
        if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0)
            gSettings.fullscreen = (value[0] == 't');
        else
            fprintf(stderr, "%s:%d: fullscreen has to be true or false\n", source, line);
    }
    else if (strncmp(name, "key_", 4) == 0 || strncmp(name, "pad_", 4) == 0)
    {
        for (int i = 0; i < NUM_BINDINGS; i++)
        {
            if (strcmp(name + 4, sBindingNames[i]) == 0)
            {
                ParseBinding(&sBindings[i], name[0] == 'k', value, source, line);
                return;
            }
        }
        fprintf(stderr, "%s:%d: there's no button called \"%s\"\n", source, line, name + 4);
    }
    else
    {
        fprintf(stderr, "%s:%d: unknown setting \"%s\"\n", source, line, name);
    }
}

static void ParseSettings(char *text, const char *source)
{
    char *next;
    int line = 0;

    for (char *start = text; start != NULL; start = next)
    {
        char *equals;

        next = strchr(start, '\n');
        if (next != NULL)
            *next++ = '\0';
        line++;

        start = Trim(start);
        if (start[0] == '\0' || start[0] == '#')
            continue;
        equals = strchr(start, '=');
        if (equals == NULL)
        {
            fprintf(stderr, "%s:%d: expected \"name = value\"\n", source, line);
            continue;
        }
        *equals = '\0';
        ParseSetting(Trim(start), Trim(equals + 1), source, line);
    }
}

void LoadSettings(const char *path)
{
    char *text = malloc(sizeof(sDefaultSettings));
    FILE *file;
    long size;

    memset(sBindings, 0, sizeof(sBindings));
    memcpy(text, sDefaultSettings, sizeof(sDefaultSettings));
    ParseSettings(text, "default settings");
    free(text);

    if (path == NULL)
        return;
    file = fopen(path, "rb");
    if (file == NULL)
    {
        file = fopen(path, "wb");
        if (file != NULL && fputs(sDefaultSettings, file) >= 0 && fclose(file) == 0)
            printf("Wrote the default settings to %s\n", path);
        else
            fprintf(stderr, "Could not write the settings to %s\n", path);
        return;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = malloc(size + 1);
    size = fread(text, 1, size, file);
    text[size] = '\0';
    fclose(file);
    ParseSettings(text, path);
    free(text);
}

void Input_HandleEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERDEVICEADDED)
    {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(event->cdevice.which);
        int slot = -1;

        for (int i = 0; i < MAX_CONTROLLERS; i++)
        {
            if (sControllers[i] == NULL)
            {
                if (slot < 0)
                    slot = i;
            }
            else if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sControllers[i])) == id)
            {
                return;
            }
        }
        if (slot < 0)
            return;
        sControllers[slot] = SDL_GameControllerOpen(event->cdevice.which);
        if (sControllers[slot] != NULL)
            printf("Controller connected: %s\n", SDL_GameControllerName(sControllers[slot]));
        else
            fprintf(stderr, "Could not open a controller: %s\n", SDL_GetError());
    }
    else if (event->type == SDL_CONTROLLERDEVICEREMOVED)
    {
        for (int i = 0; i < MAX_CONTROLLERS; i++)
        {
            if (sControllers[i] != NULL
             && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sControllers[i])) == event->cdevice.which)
            {
                printf("Controller disconnected: %s\n", SDL_GameControllerName(sControllers[i]));
                SDL_GameControllerClose(sControllers[i]);
                sControllers[i] = NULL;
            }
        }
    }
}

static bool PadInputHeld(const struct PadState *pad, const struct PadInput *input)
{
    if (!input->isAxis)
        return pad->buttons[input->index];
    return pad->axes[input->index] * input->direction > AXIS_THRESHOLD;
}

// The bindings held, as bits: the GBA buttons and fast-forward
static u32 BindingsHeld(const Uint8 *keyboard, const struct PadState *pads, int numPads)
{
    u32 held = 0;

    for (int i = 0; i < NUM_BINDINGS; i++)
    {
        const struct Binding *binding = &sBindings[i];

        for (int j = 0; j < binding->numKeys; j++)
        {
            if (keyboard[binding->keys[j]])
                held |= 1 << i;
        }
        for (int pad = 0; pad < numPads; pad++)
        {
            for (int j = 0; j < binding->numPads; j++)
            {
                if (PadInputHeld(&pads[pad], &binding->pads[j]))
                    held |= 1 << i;
            }
        }
    }
    return held;
}

u16 Input_GetButtons(bool *speedUp)
{
    struct PadState pads[MAX_CONTROLLERS];
    int numPads = 0;
    u32 held;

    for (int i = 0; i < MAX_CONTROLLERS; i++)
    {
        if (sControllers[i] == NULL)
            continue;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++)
            pads[numPads].buttons[button] = SDL_GameControllerGetButton(sControllers[i], button);
        for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++)
            pads[numPads].axes[axis] = SDL_GameControllerGetAxis(sControllers[i], axis);
        numPads++;
    }

    held = BindingsHeld(SDL_GetKeyboardState(NULL), pads, numPads);
    if (speedUp != NULL)
        *speedUp = (held >> BINDING_SPEEDUP) & 1;
    return held & KEYS_MASK;
}

void Input_ReleaseTestInputs(void)
{
    memset(sTestKeyboard, 0, sizeof(sTestKeyboard));
    memset(&sTestPad, 0, sizeof(sTestPad));
}

bool Input_PressTestInput(const char *name)
{
    if (strncmp(name, "key:", 4) == 0)
    {
        SDL_Scancode key;

        if (!ParseKey(name + 4, &key))
            return false;
        sTestKeyboard[key] = 1;
        return true;
    }
    if (strncmp(name, "pad:", 4) == 0)
    {
        struct PadInput input;

        if (!ParsePadInput(name + 4, &input))
            return false;
        if (input.isAxis)
            sTestPad.axes[input.index] = input.direction * SDL_JOYSTICK_AXIS_MAX;
        else
            sTestPad.buttons[input.index] = true;
        return true;
    }
    return false;
}

u16 Input_GetTestButtons(void)
{
    return BindingsHeld(sTestKeyboard, &sTestPad, 1) & KEYS_MASK;
}
#endif // PLATFORM_SDL2
