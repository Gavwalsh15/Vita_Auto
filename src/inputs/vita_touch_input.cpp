#include "inputs/vita_touch_input.h"

#include "app/app_errors.h"
#include "logger/performance.h"
#include "logger/vita_log.h"

#include <psp2/touch.h>

namespace inputs {
namespace performance = logger::performance;

namespace {

// A finger the phone has been told about. The phone knows it by pointerId; the
// panel knows it by the id in its reports.
struct Contact {
    uint8_t panelId = 0;
    uint32_t pointerId = 0;
    uint16_t x = 0;
    uint16_t y = 0;
};

// A finger on the panel right now.
struct PanelContact {
    uint8_t panelId = 0;
    uint16_t x = 0;
    uint16_t y = 0;
};

static SceTouchPanelInfo g_touchPanelInfo{};
static bool g_touchSamplingStarted = false;
// What the phone has been told, oldest finger first. It only changes once an event
// has been queued, so a full queue just means the same change is tried again.
static Contact g_sent[MAX_TOUCH_POINTERS];
static uint8_t g_sentCount = 0;
static uint32_t g_touchReportsSent = 0;
static uint32_t g_touchSendFailures = 0;

constexpr uint32_t AA_TOUCH_WIDTH = 1280;
constexpr uint32_t AA_TOUCH_HEIGHT = 720;
constexpr uint32_t AA_TOUCH_MAX_X = AA_TOUCH_WIDTH - 1;
constexpr uint32_t AA_TOUCH_MAX_Y = AA_TOUCH_HEIGHT - 1;
constexpr int VITA_TOUCH_FALLBACK_MAX_X = 1919;
constexpr int VITA_TOUCH_FALLBACK_MAX_Y = 1087;
constexpr uint32_t TOUCH_MOVE_THRESHOLD = 2;

uint16_t clampU16(uint32_t value, uint32_t maxValue)
{
    return static_cast<uint16_t>(value > maxValue ? maxValue : value);
}

uint16_t distance(uint16_t a, uint16_t b)
{
    return a > b ? static_cast<uint16_t>(a - b) : static_cast<uint16_t>(b - a);
}

// Panel coordinates to Android Auto's 1280x720 touch space.
void mapReport(const SceTouchReport& report, uint16_t& x, uint16_t& y)
{
    const int minX = g_touchPanelInfo.minDispX;
    const int minY = g_touchPanelInfo.minDispY;
    const int width = g_touchPanelInfo.maxDispX > minX
        ? g_touchPanelInfo.maxDispX - minX
        : VITA_TOUCH_FALLBACK_MAX_X;
    const int height = g_touchPanelInfo.maxDispY > minY
        ? g_touchPanelInfo.maxDispY - minY
        : VITA_TOUCH_FALLBACK_MAX_Y;

    const int rawX = report.x - minX;
    const int rawY = report.y - minY;
    x = clampU16(
        static_cast<uint32_t>(rawX < 0 ? 0 : rawX) * AA_TOUCH_MAX_X /
            static_cast<uint32_t>(width),
        AA_TOUCH_MAX_X
    );
    y = clampU16(
        static_cast<uint32_t>(rawY < 0 ? 0 : rawY) * AA_TOUCH_MAX_Y /
            static_cast<uint32_t>(height),
        AA_TOUCH_MAX_Y
    );
}

int findSent(uint8_t panelId)
{
    for (uint8_t i = 0; i < g_sentCount; ++i) {
        if (g_sent[i].panelId == panelId)
            return i;
    }
    return -1;
}

int findPanel(const PanelContact* panel, uint8_t count, uint8_t panelId)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (panel[i].panelId == panelId)
            return i;
    }
    return -1;
}

// The lowest id no finger down is using, so a lone finger is always pointer 0.
uint32_t freePointerId()
{
    for (uint32_t id = 0; id < MAX_TOUCH_POINTERS; ++id) {
        bool used = false;
        for (uint8_t i = 0; i < g_sentCount; ++i)
            used = used || g_sent[i].pointerId == id;
        if (!used)
            return id;
    }
    return 0; // Not reached: this is only called with fewer than the maximum down.
}

void removeSent(uint8_t index)
{
    for (uint8_t i = index; i + 1 < g_sentCount; ++i)
        g_sent[i] = g_sent[i + 1];
    --g_sentCount;
}

// An event listing every finger the phone knows about, where it was told they are.
TouchEvent eventFromSent(TouchAction action, uint8_t actionIndex)
{
    TouchEvent event;
    event.action = action;
    event.actionIndex = actionIndex;
    event.count = g_sentCount;
    for (uint8_t i = 0; i < g_sentCount; ++i) {
        event.pointers[i].x = g_sent[i].x;
        event.pointers[i].y = g_sent[i].y;
        event.pointers[i].id = g_sent[i].pointerId;
    }
    return event;
}

bool noteSendFailure(const char* action)
{
    ++g_touchSendFailures;
    vitaLogInfo("touch tx failed action=%s failures=%u", action, g_touchSendFailures);
    return false;
}

// A finger the phone knows about has left the panel.
bool sendLift(TouchSender sender, uint8_t index)
{
    const bool last = g_sentCount == 1;
    const TouchEvent event = eventFromSent(
        last ? TouchAction::Up : TouchAction::PointerUp,
        last ? 0 : index
    );
    if (!sender(event))
        return noteSendFailure(last ? "up" : "pointer-up");

    ++g_touchReportsSent;
    if (last) {
        vitaLogInfo(
            "touch tx action=up x=%u y=%u reports=%u",
            g_sent[0].x,
            g_sent[0].y,
            g_touchReportsSent
        );
        removeSent(0);
        performance::endGesture(performance::nowUs(), g_touchReportsSent);
    } else {
        vitaLogInfo(
            "touch tx action=pointer-up id=%u fingers=%u reports=%u",
            g_sent[index].pointerId,
            g_sentCount,
            g_touchReportsSent
        );
        removeSent(index);
    }
    return true;
}

// A finger the phone has not heard of is on the panel.
bool sendLand(TouchSender sender, const PanelContact& finger)
{
    const bool first = g_sentCount == 0;
    Contact contact;
    contact.panelId = finger.panelId;
    contact.pointerId = freePointerId();
    contact.x = finger.x;
    contact.y = finger.y;

    TouchEvent event = eventFromSent(
        first ? TouchAction::Down : TouchAction::PointerDown,
        0
    );
    event.actionIndex = first ? 0 : event.count;
    event.pointers[event.count].x = contact.x;
    event.pointers[event.count].y = contact.y;
    event.pointers[event.count].id = contact.pointerId;
    ++event.count;

    if (first)
        performance::beginGesture(performance::nowUs(), g_touchReportsSent);
    if (!sender(event))
        return noteSendFailure(first ? "down" : "pointer-down");

    ++g_touchReportsSent;
    g_sent[g_sentCount++] = contact;
    if (first) {
        vitaLogInfo(
            "touch tx action=down x=%u y=%u reports=%u",
            contact.x,
            contact.y,
            g_touchReportsSent
        );
    } else {
        vitaLogInfo(
            "touch tx action=pointer-down id=%u x=%u y=%u fingers=%u reports=%u",
            contact.pointerId,
            contact.x,
            contact.y,
            g_sentCount,
            g_touchReportsSent
        );
    }
    return true;
}

// Fingers that stay down have moved. Every finger goes in the event at its current
// position, but it is only sent when at least one has moved more than the threshold.
bool sendMove(TouchSender sender, const PanelContact* panel, uint8_t panelCount)
{
    TouchEvent event = eventFromSent(TouchAction::Move, 0);
    bool moved = false;
    for (uint8_t i = 0; i < g_sentCount; ++i) {
        const int p = findPanel(panel, panelCount, g_sent[i].panelId);
        if (p < 0)
            return false;
        event.pointers[i].x = panel[p].x;
        event.pointers[i].y = panel[p].y;
        if (distance(panel[p].x, g_sent[i].x) >= TOUCH_MOVE_THRESHOLD ||
            distance(panel[p].y, g_sent[i].y) >= TOUCH_MOVE_THRESHOLD)
        {
            moved = true;
        }
    }
    if (!moved)
        return false;
    if (!sender(event))
        return noteSendFailure("move");

    ++g_touchReportsSent;
    for (uint8_t i = 0; i < g_sentCount; ++i) {
        g_sent[i].x = event.pointers[i].x;
        g_sent[i].y = event.pointers[i].y;
    }
    return true;
}

// Sends the one event that brings the phone's picture of the fingers closer to the
// panel's: lifts first, then new fingers, then movement. True if it sent one.
bool sendNextChange(TouchSender sender, const PanelContact* panel, uint8_t panelCount)
{
    for (uint8_t i = 0; i < g_sentCount; ++i) {
        if (findPanel(panel, panelCount, g_sent[i].panelId) < 0)
            return sendLift(sender, i);
    }
    for (uint8_t p = 0; p < panelCount; ++p) {
        if (findSent(panel[p].panelId) < 0 && g_sentCount < MAX_TOUCH_POINTERS)
            return sendLand(sender, panel[p]);
    }
    return sendMove(sender, panel, panelCount);
}

} // namespace

void initTouchInput()
{
    const int infoResult = sceTouchGetPanelInfo(
        SCE_TOUCH_PORT_FRONT,
        &g_touchPanelInfo
    );
    const int sampleResult = sceTouchSetSamplingState(
        SCE_TOUCH_PORT_FRONT,
        SCE_TOUCH_SAMPLING_STATE_START
    );

    if (infoResult < 0)
        vitaLogAppError(app::AppError::TouchPanelInfoFailed, infoResult);
    if (sampleResult < 0)
        vitaLogAppError(app::AppError::TouchSamplingStartFailed, sampleResult);
    g_touchSamplingStarted = sampleResult >= 0;
    g_sentCount = 0;
    g_touchReportsSent = 0;
    g_touchSendFailures = 0;
    vitaLogInfo(
        "touch init info=0x%08x sample=0x%08x aa=1280x720 panel=%d,%d-%d,%d",
        infoResult,
        sampleResult,
        g_touchPanelInfo.minDispX,
        g_touchPanelInfo.minDispY,
        g_touchPanelInfo.maxDispX,
        g_touchPanelInfo.maxDispY
    );
}

void stopTouchInput()
{
    if (g_touchSamplingStarted)
        sceTouchSetSamplingState(
            SCE_TOUCH_PORT_FRONT,
            SCE_TOUCH_SAMPLING_STATE_STOP
        );

    g_touchSamplingStarted = false;
    g_sentCount = 0;
}

// Send each change as it happens: a finger landing or lifting at once, small
// movements held back while the fingers stay put.
void pollTouchInput(TouchSender sender)
{
    if (!g_touchSamplingStarted || sender == nullptr)
        return;

    SceTouchData touch{};
    const int sampleCount = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (sampleCount <= 0)
        return;

    PanelContact panel[MAX_TOUCH_POINTERS];
    uint8_t panelCount = 0;
    for (uint32_t i = 0; i < touch.reportNum && panelCount < MAX_TOUCH_POINTERS; ++i) {
        panel[panelCount].panelId = touch.report[i].id;
        mapReport(touch.report[i], panel[panelCount].x, panel[panelCount].y);
        ++panelCount;
    }

    // One Android-style event per change. Stop at the first that cannot be queued;
    // nothing was recorded for it, so the next poll tries it again.
    for (unsigned step = 0; step < 2u * MAX_TOUCH_POINTERS + 1u; ++step) {
        if (!sendNextChange(sender, panel, panelCount))
            return;
    }
}

} // namespace inputs
