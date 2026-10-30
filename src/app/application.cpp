#include "app/application.h"
#include "app/audio_settings.h"
#include "app/config.h"
#include "app/connection_screen.h"
#include "app/direct_session.h"
#include "app/recovery.h"
#include "app/usb_shell_lock.h"
#include "inputs/vita_input.h"
#include "video/video_pipeline.h"
#include "video/video_presenter.h"
#include "logger/performance.h"
#include "logger/vita_log.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

namespace app {

int run()
{
    constexpr uint64_t RETRY_DELAY_US = 3000000;
    constexpr uint64_t UI_POLL_US = 16000;
    constexpr uint64_t SCREEN_REFRESH_US = 250000;
    constexpr uint32_t DISCONNECT_COMBO = SCE_CTRL_START | SCE_CTRL_SELECT;
    UsbShellLock shellLock;
    bool running = false;
    bool inputWorker = false;
    bool streaming = false;
    bool exiting = false;
    bool restartAllowed = true;
    bool retryPending = false;
    bool disconnectRequested = false;
    bool audioOnVita = loadAudioOnVita();
    bool audioSettingSaved = true;
    AppError lastError = AppError::None;
    AppError cleanupError = AppError::None;
    uint32_t previousButtons = 0;
    uint64_t retryAtUs = 0;
    uint64_t nextUiPollUs = 0;
    uint64_t nextScreenUs = 0;
    uint64_t nextPowerTickUs = 0;

    auto releaseHome = [&]() {
        if (!shellLock.releaseHome()) {
            if (lastError == AppError::None)
                lastError = shellLock.lastError();
            restartAllowed = false;
            retryPending = false;
        }
    };

    auto finishAttempt = [&]() {
        vitaLogInfo("cleanup: joining input and IO threads");
        inputs::stopWorker();
        direct_session::join();
        inputs::stop();
        vitaLogInfo("cleanup: stopping video decoder");
        video::pipeline::stop();
        vitaLogInfo("cleanup: stopping display");
        const bool displayStopped = video::presenter::shutdown();
        running = false;
        streaming = false;
        lastError = direct_session::lastError();
        const bool usbStopped = direct_session::canRestart();
        cleanupError = !usbStopped ? AppError::UsbStopFailed
            : !displayStopped ? AppError::GraphicsShutdownFailed : AppError::None;
        if (!displayStopped && lastError == AppError::None)
            lastError = AppError::GraphicsShutdownFailed;
        const Recovery recovery = recoveryAfterSession(
            lastError, usbStopped && displayStopped);
        restartAllowed = recovery != Recovery::RestartRequired;
        retryPending = recovery == Recovery::AutomaticRetry &&
                         !disconnectRequested && !exiting;
        if (restartAllowed && !retryPending)
            releaseHome();
        vitaLogInfo(
            "cleanup: complete restart_allowed=%u auto_retry=%u error=%s",
            restartAllowed ? 1u : 0u,
            retryPending ? 1u : 0u,
            appErrorName(lastError)
        );
        retryAtUs = sceKernelGetSystemTimeWide() + RETRY_DELAY_US;
        nextScreenUs = 0;
    };

    auto beginAttempt = [&]() {
        lastError = AppError::None;
        cleanupError = AppError::None;
        retryPending = false;
        disconnectRequested = false;
        nextScreenUs = 0;
        if (!shellLock.acquire()) {
            lastError = shellLock.lastError();
            releaseHome();
            return;
        }
        logger::performance::reset();
        inputs::init();
        video::presenter::init();
        streaming = false;
        inputWorker = false;
        nextScreenUs = 0;
        running = direct_session::start(audioOnVita);
        if (running)
            inputWorker = inputs::startWorker();
        else
            finishAttempt();
    };

    vitaLogInfo("DIRECT USB runner start");
    beginAttempt();
    while (true) {
        if (running && direct_session::finished())
            finishAttempt();
        if (exiting && !running)
            break;

        const uint64_t nowUs = sceKernelGetSystemTimeWide();
        if (running) {
            if (nowUs >= nextPowerTickUs) {
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
                nextPowerTickUs = nowUs + 1000000;
            }
            if (!inputWorker)
                inputs::sample();
            if (!disconnectRequested && video::pipeline::present())
                streaming = true;
        }

        if (nowUs >= nextUiPollUs) {
            nextUiPollUs = nowUs + UI_POLL_US;
            SceCtrlData pad{};
            if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
                const uint32_t pressed = pad.buttons & ~previousButtons;
                const bool disconnectCombo =
                    (pad.buttons & DISCONNECT_COMBO) == DISCONNECT_COMBO &&
                    (previousButtons & DISCONNECT_COMBO) != DISCONNECT_COMBO;
                previousButtons = pad.buttons;
                if (!streaming && (pressed & SCE_CTRL_CIRCLE)) {
                    exiting = true;
                    if (running)
                        direct_session::requestStop();
                } else if (disconnectCombo) {
                    disconnectRequested = true;
                    retryPending = false;
                    if (running)
                        direct_session::requestStop();
                    else if (restartAllowed)
                        releaseHome();
                    nextScreenUs = 0;
                } else if (!running && restartAllowed && (pressed & SCE_CTRL_TRIANGLE)) {
                    audioOnVita = !audioOnVita;
                    audioSettingSaved = saveAudioOnVita(audioOnVita);
                    retryPending = false;
                    releaseHome();
                    nextScreenUs = 0;
                } else if (!running && restartAllowed && (pressed & SCE_CTRL_CROSS)) {
                    disconnectRequested = false;
                    retryPending = true;
                    nextScreenUs = 0;
                }
            }
        }

        if (!exiting && !running && retryPending && nowUs >= retryAtUs)
            beginAttempt();

        if (!exiting && (!streaming || disconnectRequested) &&
            nowUs >= nextScreenUs) {
            ConnectionScreen screen;
            if (running)
                screen = disconnectRequested ? ConnectionScreen::Disconnecting
                    : direct_session::status() == direct_session::Status::WaitingForPhone
                        ? ConnectionScreen::WaitingForPhone : ConnectionScreen::Connecting;
            else if (!restartAllowed)
                screen = ConnectionScreen::RestartRequired;
            else if (disconnectRequested)
                screen = ConnectionScreen::Disconnected;
            else
                screen = retryPending ? ConnectionScreen::Retrying : ConnectionScreen::Error;
            const unsigned seconds = nowUs < retryAtUs
                ? static_cast<unsigned>((retryAtUs - nowUs + 999999) / 1000000) : 0;
            showConnectionScreen(
                screen,
                screen == ConnectionScreen::RestartRequired &&
                        cleanupError != AppError::None ? cleanupError : lastError,
                seconds,
                audioOnVita,
                audioSettingSaved
            );
            nextScreenUs = nowUs + SCREEN_REFRESH_US;
        }
        sceKernelDelayThread(config::IDLE_POLL_US);
    }

    if (restartAllowed && !shellLock.release())
        restartAllowed = false;
    vitaLogInfo("direct-usb: shutdown complete");
    return restartAllowed ? 0 : 1;
}

} // namespace app
