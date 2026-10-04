#include "inputs/vita_input.h"
#include "logger/performance.h"
#include "android_auto/aa_direct.h"
#include "inputs/input_event_queue.h"
#include "inputs/vita_controller_input.h"
#include "inputs/vita_touch_input.h"
#include "logger/vita_log.h"
#include "transport/usb_transport.h"

#include <psp2/kernel/threadmgr.h>

namespace inputs {
namespace performance = logger::performance;

namespace {

constexpr int DIRECT_INPUT_THREAD_PRIORITY = 0x10000100;
constexpr int DIRECT_INPUT_THREAD_STACK_SIZE = 0x10000;
constexpr size_t DIRECT_INPUT_QUEUE_CAPACITY = 64;
constexpr unsigned DIRECT_INPUT_POLL_US = 4000;

// A single input producer wakes the IO owner after queuing each event.
static inputs::InputEventQueue<DIRECT_INPUT_QUEUE_CAPACITY> g_directInputQueue;
static SceUID g_directInputThread = -1;
static volatile uint32_t g_directInputThreadRunning = 0;
static volatile uint32_t g_directInputReady = 0;
static DeliveryStats g_deliveryStats;

android_auto::TouchAction toAndroidAutoTouchAction(inputs::TouchAction action)
{
    switch (action) {
    case inputs::TouchAction::Down:
        return android_auto::TouchAction::Down;
    case inputs::TouchAction::Up:
        return android_auto::TouchAction::Up;
    case inputs::TouchAction::Move:
    default:
        return android_auto::TouchAction::Move;
    }
}

uint32_t androidKeycode(inputs::ButtonCommand command)
{
    // android.view.KeyEvent constants advertised by service discovery.
    switch (command) {
    case inputs::ButtonCommand::Back:           return 4;
    case inputs::ButtonCommand::VoiceAssistant: return 84;
    case inputs::ButtonCommand::Select:         return 23; 
    case inputs::ButtonCommand::Up:             return 19;
    case inputs::ButtonCommand::Down:           return 20;
    case inputs::ButtonCommand::Left:           return 21;
    case inputs::ButtonCommand::Right:          return 22;
    case inputs::ButtonCommand::PlayPause:      return 85;
    case inputs::ButtonCommand::Next:           return 87;
    case inputs::ButtonCommand::Previous:       return 88;
    case inputs::ButtonCommand::Home:           return 3;
    default:                                    return 0;
    }
}

bool isRotaryCommand(inputs::ButtonCommand command)
{
    return command == inputs::ButtonCommand::PreviousItem ||
           command == inputs::ButtonCommand::NextItem;
}

bool sendControllerEvent(
    android_auto::DirectSession& session,
    const inputs::ButtonEvent& event
)
{
    if (isRotaryCommand(event.command))
    {
        return session.sendRotary(
            event.command == inputs::ButtonCommand::PreviousItem ? -1 : 1
        );
    }
    const uint32_t keycode = androidKeycode(event.command);
    if (keycode == 0)
        return false;
    // Repeat navigation with a new key edge, not another unmatched DOWN.
    if (event.repeated && !session.sendButton(keycode, false))
        return false;
    return session.sendButton(keycode, event.pressed);
}

bool queueTouchEvent(const inputs::TouchEvent& touch)
{
    inputs::InputEvent event;
    event.type = inputs::InputEventType::Touch;
    event.queuedAtUs = performance::nowUs();
    event.touch = touch;
    const bool queued = g_directInputQueue.push(event);
    if (queued)
        UsbTransport::wake();
    return queued;
}

bool queueButtonEvent(const inputs::ButtonEvent& button)
{
    // Rotary releases only update the controller's local held-button state.
    if (isRotaryCommand(button.command) && !button.pressed)
        return true;
    inputs::InputEvent event;
    event.type = inputs::InputEventType::Button;
    event.queuedAtUs = performance::nowUs();
    event.button = button;
    const bool queued = g_directInputQueue.push(event);
    if (queued)
        UsbTransport::wake();
    return queued;
}

bool sampleDirectInput()
{
    const bool inputReady = __atomic_load_n(
        &g_directInputReady,
        __ATOMIC_ACQUIRE
    ) != 0;

    if (!inputReady)
        return false;

    pollTouchInput(queueTouchEvent);
    pollControllerInput(queueButtonEvent);
    return true;
}

int directInputThreadEntry(SceSize, void*)
{
    while (__atomic_load_n(
               &g_directInputThreadRunning,
               __ATOMIC_ACQUIRE) != 0)
    {
        sampleDirectInput();
        sceKernelDelayThread(DIRECT_INPUT_POLL_US);
    }
    return sceKernelExitThread(0);
}

bool startDirectInputThread()
{
    g_directInputThread = sceKernelCreateThread(
        "vita-auto-direct-input",
        directInputThreadEntry,
        DIRECT_INPUT_THREAD_PRIORITY,
        DIRECT_INPUT_THREAD_STACK_SIZE,
        0,
        0,
        nullptr
    );
    if (g_directInputThread < 0) {
        vitaLog(
            "[%s] direct-usb: input thread create failed err=0x%08x; main fallback", app::appErrorName(app::AppError::InputThreadCreateFailed),
            g_directInputThread
        );
        g_directInputThread = -1;
        return false;
    }

    __atomic_store_n(&g_directInputThreadRunning, 1u, __ATOMIC_RELEASE);
    const int result = sceKernelStartThread(g_directInputThread, 0, nullptr);
    if (result < 0) {
        vitaLog(
            "[%s] direct-usb: input thread start failed err=0x%08x; main fallback", app::appErrorName(app::AppError::InputThreadStartFailed),
            result
        );
        __atomic_store_n(&g_directInputThreadRunning, 0u, __ATOMIC_RELEASE);
        sceKernelDeleteThread(g_directInputThread);
        g_directInputThread = -1;
        return false;
    }

    vitaLogInfo("direct-usb: input thread started");
    return true;
}

void stopDirectInputThread()
{
    if (g_directInputThread < 0)
        return;

    __atomic_store_n(&g_directInputThreadRunning, 0u, __ATOMIC_RELEASE);
    sceKernelWaitThreadEnd(g_directInputThread, nullptr, nullptr);
    sceKernelDeleteThread(g_directInputThread);
    g_directInputThread = -1;
}

bool deliverDirectInputEvent(
    android_auto::DirectSession& session,
    const inputs::InputEvent& event
)
{
    if (event.queuedAtUs != 0) {
        const uint64_t nowUs = performance::nowUs();
        const uint64_t delayUs = nowUs > event.queuedAtUs
            ? nowUs - event.queuedAtUs
            : 0;
        const uint32_t boundedDelay = delayUs > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(delayUs);
        performance::record(performance::Metric::InputQueue, boundedDelay);
        if (boundedDelay > g_deliveryStats.maxQueueUs)
            g_deliveryStats.maxQueueUs = boundedDelay;
    }

    const uint64_t encodeStartUs = performance::nowUs();
    bool sent = false;
    if (event.type == inputs::InputEventType::Touch) {
        sent = session.sendTouch(
            toAndroidAutoTouchAction(event.touch.action),
            event.touch.x,
            event.touch.y,
            event.touch.pointerId
        );
    } else {
        sent = sendControllerEvent(session, event.button);
    }
    performance::record(performance::Metric::InputEncode, performance::elapsedUs(encodeStartUs));
    if (sent) {
        ++g_deliveryStats.delivered;
        if (event.type == inputs::InputEventType::Touch)
            ++g_deliveryStats.touchDelivered;
        else if (isRotaryCommand(event.button.command))
            ++g_deliveryStats.rotarySteps;
        else
            ++g_deliveryStats.buttonDelivered;
    }
    return sent;
}

bool drainDirectInputQueue(android_auto::DirectSession& session)
{
    inputs::InputEvent event;
    inputs::InputEvent latestMove;
    bool hasMove = false;

    while (g_directInputQueue.pop(event)) {
        ++g_deliveryStats.dequeued;
        const bool isMove =
            event.type == inputs::InputEventType::Touch &&
            event.touch.action == inputs::TouchAction::Move;
        if (isMove) {
            if (hasMove)
                ++g_deliveryStats.movesCoalesced;
            latestMove = event;
            hasMove = true;
            continue;
        }

        // Preserve event ordering across DOWN, UP, and every button edge, but
        // never make Android process obsolete intermediate drag positions.
        if (hasMove) {
            if (!deliverDirectInputEvent(session, latestMove))
                return false;
            hasMove = false;
        }
        if (!deliverDirectInputEvent(session, event))
            return false;
    }

    return !hasMove || deliverDirectInputEvent(session, latestMove);
}

} // namespace

void init()
{
    initTouchInput();
    initControllerInput();
    g_directInputQueue.reset();
    g_deliveryStats = {};
    setReady(false);
}

void stop()
{
    stopTouchInput();
    stopControllerInput();
}

bool startWorker() { return startDirectInputThread(); }
void stopWorker() { stopDirectInputThread(); }
bool sample() { return sampleDirectInput(); }

void setReady(bool ready)
{
    __atomic_store_n(&g_directInputReady, ready ? 1u : 0u, __ATOMIC_RELEASE);
}

bool drain(android_auto::DirectSession& session)
{
    return drainDirectInputQueue(session);
}

DeliveryStats deliveryStats() { return g_deliveryStats; }

} // namespace inputs
