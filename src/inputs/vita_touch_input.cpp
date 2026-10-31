#include "inputs/vita_touch_input.h"

#include "app/app_errors.h"
#include "logger/performance.h"
#include "logger/vita_log.h"

#include <psp2/touch.h>

namespace inputs {
namespace performance = logger::performance;

namespace {

static SceTouchPanelInfo g_touchPanelInfo{};
static bool g_touchSamplingStarted = false;
static bool g_touchWasDown = false;
static uint16_t g_lastTouchX = 0;
static uint16_t g_lastTouchY = 0;
static uint32_t g_touchReportsSent = 0;
static uint32_t g_touchSendFailures = 0;

// Must match the touch area advertised in service discovery (16:9, 720p).
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
    g_touchWasDown = false;
}

// Send edges immediately; suppress tiny movements while a contact stays down.
void pollTouchInput(TouchSender sender)
{
    if (!g_touchSamplingStarted || sender == nullptr)
        return;

    SceTouchData touch{};
    const int sampleCount = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (sampleCount <= 0)
        return;

    if (touch.reportNum == 0) {
        if (g_touchWasDown) {
            inputs::TouchEvent event;
            event.action = inputs::TouchAction::Up;
            event.x = g_lastTouchX;
            event.y = g_lastTouchY;
            const bool sent = sender(event);
            if (sent) {
                ++g_touchReportsSent;
                if (VITA_AUTO_VERBOSE_LOGGING) {
                    vitaLogInfo(
                        "touch tx action=up x=%u y=%u reports=%u",
                        g_lastTouchX,
                        g_lastTouchY,
                        g_touchReportsSent
                    );
                }
                g_touchWasDown = false;
                performance::endGesture(performance::nowUs(), g_touchReportsSent);
            } else {
                ++g_touchSendFailures;
                if (VITA_AUTO_VERBOSE_LOGGING)
                    vitaLogInfo("touch tx failed action=up failures=%u", g_touchSendFailures);
            }
        }
        return;
    }

    const SceTouchReport& report = touch.report[0];
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
    const uint16_t x = clampU16(
        static_cast<uint32_t>(rawX < 0 ? 0 : rawX) * AA_TOUCH_MAX_X /
            static_cast<uint32_t>(width),
        AA_TOUCH_MAX_X
    );
    const uint16_t y = clampU16(
        static_cast<uint32_t>(rawY < 0 ? 0 : rawY) * AA_TOUCH_MAX_Y /
            static_cast<uint32_t>(height),
        AA_TOUCH_MAX_Y
    );

    const uint32_t dx = x > g_lastTouchX ? x - g_lastTouchX : g_lastTouchX - x;
    const uint32_t dy = y > g_lastTouchY ? y - g_lastTouchY : g_lastTouchY - y;

    const bool isDown = !g_touchWasDown;
    if (!isDown && dx < TOUCH_MOVE_THRESHOLD && dy < TOUCH_MOVE_THRESHOLD)
        return;

    if (isDown)
        performance::beginGesture(performance::nowUs(), g_touchReportsSent);

    inputs::TouchEvent event;
    event.action = isDown ? inputs::TouchAction::Down : inputs::TouchAction::Move;
    event.x = x;
    event.y = y;
    if (!sender(event)) {
        ++g_touchSendFailures;
        if (VITA_AUTO_VERBOSE_LOGGING)
            vitaLogInfo("touch tx failed action=%s failures=%u",
                isDown ? "down" : "move", g_touchSendFailures);
        return;
    }

    ++g_touchReportsSent;
    if (isDown && VITA_AUTO_VERBOSE_LOGGING) {
        vitaLogInfo(
            "touch tx action=down x=%u y=%u reports=%u",
            x,
            y,
            g_touchReportsSent
        );
    }
    g_touchWasDown = true;
    g_lastTouchX = x;
    g_lastTouchY = y;
}

} // namespace inputs
