#include "inputs/vita_controller_input.h"

#include "app/app_errors.h"
#include "logger/vita_log.h"

#include <cstddef>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

namespace inputs {
namespace {

constexpr uint64_t NAVIGATION_REPEAT_DELAY_US = 350000;
constexpr uint64_t NAVIGATION_REPEAT_INTERVAL_US = 120000;

constexpr int STICK_PRESS = 72;
constexpr int STICK_RELEASE = 40;

constexpr uint64_t STICK_BOUNCE_US = 150000;

constexpr uint8_t NAV_UP = 1u << 0;
constexpr uint8_t NAV_DOWN = 1u << 1;
constexpr uint8_t NAV_LEFT = 1u << 2;
constexpr uint8_t NAV_RIGHT = 1u << 3;

constexpr uint8_t ITEM_PREVIOUS = 1u << 0;
constexpr uint8_t ITEM_NEXT = 1u << 1;

constexpr uint32_t DPAD_BUTTONS = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;

struct ButtonMapping {
    uint32_t mask;
    ButtonCommand command;
};

const ButtonMapping BUTTON_MAPPINGS[] = {
    { SCE_CTRL_CIRCLE,   ButtonCommand::Back },
    { SCE_CTRL_SQUARE,   ButtonCommand::VoiceAssistant },
    { SCE_CTRL_CROSS,    ButtonCommand::Select },
    { SCE_CTRL_LTRIGGER, ButtonCommand::Previous },
    { SCE_CTRL_RTRIGGER, ButtonCommand::Next },
    { SCE_CTRL_TRIANGLE, ButtonCommand::PlayPause },
};

const ButtonMapping FOCUS_MAPPINGS[] = {
    { NAV_UP,    ButtonCommand::Up },
    { NAV_DOWN,  ButtonCommand::Down },
    { NAV_LEFT,  ButtonCommand::Left },
    { NAV_RIGHT, ButtonCommand::Right },
};

const ButtonMapping ITEM_MAPPINGS[] = {
    { ITEM_PREVIOUS, ButtonCommand::PreviousItem },
    { ITEM_NEXT,     ButtonCommand::NextItem },
};

constexpr uint32_t MAPPED_BUTTONS =
    SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE | SCE_CTRL_CROSS |
    SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE;

struct Stick {
    uint8_t held = 0;
    uint8_t released = 0;
    uint64_t releasedAtUs = 0;
    bool blocked = false;
};

struct Dpad {
    uint32_t held = 0;
    uint32_t ignored = 0;
};

bool g_controllerSamplingStarted = false;
SceCtrlPadInputMode g_previousControllerMode = SCE_CTRL_MODE_DIGITAL;
uint32_t g_reportedControllerButtons = 0;
uint32_t g_reportedFocusDirections = 0;
uint32_t g_reportedItemDirections = 0;
uint64_t g_focusRepeatAtUs[4]{};
uint64_t g_itemRepeatAtUs[2]{};
uint32_t g_staleButtons = 0;
Stick g_focusStick;
Stick g_itemStick;
Dpad g_dpad;

int magnitude(int value) { return value < 0 ? -value : value; }

uint8_t opposite(uint8_t direction)
{
    switch (direction) {
    case NAV_UP: return NAV_DOWN;
    case NAV_DOWN: return NAV_UP;
    case NAV_LEFT: return NAV_RIGHT;
    case NAV_RIGHT: return NAV_LEFT;
    default: return 0;
    }
}

int deflection(uint8_t direction, int dx, int dy)
{
    switch (direction) {
    case NAV_UP: return -dy;
    case NAV_DOWN: return dy;
    case NAV_LEFT: return -dx;
    case NAV_RIGHT: return dx;
    default: return 0;
    }
}

uint8_t stickDirection(Stick& stick, uint8_t x, uint8_t y, uint64_t nowUs)
{
    const int dx = static_cast<int>(x) - 128;
    const int dy = static_cast<int>(y) - 128;
    const int furthest = magnitude(dx) > magnitude(dy) ? magnitude(dx) : magnitude(dy);

    if (stick.blocked) {
        if (furthest >= STICK_RELEASE)
            return 0;
        stick.blocked = false;
    }
    if (stick.held != 0) {
        if (deflection(stick.held, dx, dy) >= STICK_RELEASE)
            return stick.held;
        stick.released = stick.held;
        stick.releasedAtUs = nowUs;
        stick.held = 0;
    }
    if (furthest < STICK_PRESS)
        return 0;
    const uint8_t direction = magnitude(dx) >= magnitude(dy)
        ? (dx < 0 ? NAV_LEFT : NAV_RIGHT)
        : (dy < 0 ? NAV_UP : NAV_DOWN);
    if (direction == opposite(stick.released) && nowUs - stick.releasedAtUs < STICK_BOUNCE_US)
        return 0;
    stick.held = direction;
    return direction;
}

uint32_t dpadDirection(Dpad& dpad, uint32_t buttons)
{
    const uint32_t down = buttons & DPAD_BUTTONS;
    dpad.ignored &= down;
    if ((dpad.held & down) == 0)
        dpad.held = 0;
    const uint32_t fresh = down & ~dpad.held & ~dpad.ignored;
    if (dpad.held != 0 || (fresh & (fresh - 1)) != 0) {
        dpad.ignored |= fresh;
    } else if (fresh != 0) {
        dpad.held = fresh;
    }
    return dpad.held;
}

template <size_t Count>
void sendButtonChanges(
    const ButtonMapping (&mappings)[Count],
    uint32_t current,
    uint32_t& reported,
    ButtonSender sender
) {
    for (const ButtonMapping& mapping : mappings) {
        const bool pressed = (current & mapping.mask) != 0;
        if (pressed == ((reported & mapping.mask) != 0))
            continue;

        ButtonEvent event;
        event.command = mapping.command;
        event.pressed = pressed;
        if (!sender(event))
            continue;

        if (pressed)
            reported |= mapping.mask;
        else
            reported &= ~mapping.mask;
    }
}

template <size_t Count>
void sendNavigationChanges(
    const ButtonMapping (&mappings)[Count],
    uint32_t current,
    uint32_t& reported,
    uint64_t (&repeatAtUs)[Count],
    uint64_t nowUs,
    ButtonSender sender
)
{
    for (size_t index = 0; index < Count; ++index) {
        const ButtonMapping& mapping = mappings[index];
        const bool pressed = (current & mapping.mask) != 0;
        const bool wasPressed = (reported & mapping.mask) != 0;

        if (pressed != wasPressed) {
            ButtonEvent event;
            event.command = mapping.command;
            event.pressed = pressed;
            if (!sender(event))
                continue;

            if (pressed) {
                reported |= mapping.mask;
                repeatAtUs[index] = nowUs + NAVIGATION_REPEAT_DELAY_US;
            } else {
                reported &= ~mapping.mask;
                repeatAtUs[index] = 0;
            }
            continue;
        }

        if (!pressed || nowUs < repeatAtUs[index])
            continue;

        ButtonEvent event;
        event.command = mapping.command;
        event.pressed = true;
        event.repeated = true;
        if (sender(event)) {
            repeatAtUs[index] = nowUs + NAVIGATION_REPEAT_INTERVAL_US;
        }
    }
}

void resetReported()
{
    g_reportedControllerButtons = 0;
    g_reportedFocusDirections = 0;
    g_reportedItemDirections = 0;
    for (uint64_t& at : g_focusRepeatAtUs)
        at = 0;
    for (uint64_t& at : g_itemRepeatAtUs)
        at = 0;
    g_staleButtons = 0;
    g_focusStick = Stick{};
    g_itemStick = Stick{};
    g_dpad = Dpad{};
}

bool offCentre(uint8_t x, uint8_t y)
{
    return magnitude(static_cast<int>(x) - 128) >= STICK_RELEASE ||
           magnitude(static_cast<int>(y) - 128) >= STICK_RELEASE;
}

} // namespace

void initControllerInput()
{
    const int previousMode = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (previousMode < 0)
        vitaLogAppError(app::AppError::ControllerSamplingStartFailed, previousMode);
    g_controllerSamplingStarted = previousMode >= 0;
    if (g_controllerSamplingStarted) {
        g_previousControllerMode = static_cast<SceCtrlPadInputMode>(previousMode);
    }
    resetReported();
    vitaLogInfo("controller init mode=0x%08x", previousMode);
}

void stopControllerInput()
{
    if (g_controllerSamplingStarted)
        sceCtrlSetSamplingMode(g_previousControllerMode);

    g_controllerSamplingStarted = false;
    resetReported();
}

void resyncControllerInput()
{
    resetReported();
    if (!g_controllerSamplingStarted)
        return;
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0)
        return;
    g_staleButtons = pad.buttons & MAPPED_BUTTONS;
    g_dpad.ignored = pad.buttons & DPAD_BUTTONS;
    g_focusStick.blocked = offCentre(pad.rx, pad.ry);
    g_itemStick.blocked = offCentre(pad.lx, pad.ly);
}

void pollControllerInput(ButtonSender sender)
{
    if (!g_controllerSamplingStarted || sender == nullptr)
        return;

    SceCtrlData pad{};
    const int sampleCount = sceCtrlPeekBufferPositive(0, &pad, 1);
    if (sampleCount <= 0)
        return;

    g_staleButtons &= pad.buttons;
    sendButtonChanges(BUTTON_MAPPINGS, pad.buttons & ~g_staleButtons,
                      g_reportedControllerButtons, sender);

    const uint64_t nowUs = sceKernelGetSystemTimeWide();
    const uint32_t dpad = dpadDirection(g_dpad, pad.buttons);
    const uint8_t focusStick = stickDirection(g_focusStick, pad.rx, pad.ry, nowUs);
    const uint8_t itemStick = stickDirection(g_itemStick, pad.lx, pad.ly, nowUs);

    uint8_t focus = focusStick;
    if (dpad & SCE_CTRL_UP)
        focus = NAV_UP;
    else if (dpad & SCE_CTRL_DOWN)
        focus = NAV_DOWN;
    sendNavigationChanges(FOCUS_MAPPINGS, focus, g_reportedFocusDirections,
                          g_focusRepeatAtUs, nowUs, sender);

    uint8_t item = 0;
    if (dpad & SCE_CTRL_LEFT)
        item = ITEM_PREVIOUS;
    else if (dpad & SCE_CTRL_RIGHT)
        item = ITEM_NEXT;
    else if (itemStick == NAV_UP || itemStick == NAV_LEFT)
        item = ITEM_PREVIOUS;
    else if (itemStick == NAV_DOWN || itemStick == NAV_RIGHT)
        item = ITEM_NEXT;
    sendNavigationChanges(ITEM_MAPPINGS, item, g_reportedItemDirections,
                          g_itemRepeatAtUs, nowUs, sender);
}

} // namespace inputs
