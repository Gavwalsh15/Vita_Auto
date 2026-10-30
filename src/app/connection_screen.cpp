#include "app/connection_screen.h"
#include "video/video_presenter.h"

#include <cstdio>

namespace app {
namespace {

const char* errorMessage(AppError error)
{
    switch (error) {
    case AppError::None: return "CONNECT YOUR PHONE WITH USB";
    case AppError::UsbConnectTimedOut: return "CONNECT YOUR PHONE WITH USB";
    case AppError::UsbBridgeLoadFailed: return "CHECK THE USB PLUGIN AND REBOOT";
    case AppError::UsbHostStartFailed: return "COULD NOT START THE USB PORT";
    case AppError::UsbEnumerationFailed: return "CHECK THE USB CABLE AND UNLOCK YOUR PHONE";
    case AppError::UsbStopFailed: return "USB CLEANUP DID NOT COMPLETE";
    case AppError::ShellEventsInitFailed:
    case AppError::ShellLockFailed: return "COULD NOT PROTECT THE USB SESSION";
    case AppError::ShellUnlockFailed: return "COULD NOT RELEASE THE HOME BUTTON";
    case AppError::UsbSendRejected:
    case AppError::UsbWriteFailed:
    case AppError::UsbWriteTimedOut:
    case AppError::UsbReadFailed: return "CHECK THE CABLE AND RECONNECT YOUR PHONE";
    case AppError::AndroidAutoStartFailed:
    case AppError::AndroidAutoStartTimedOut:
    case AppError::AndroidAutoFeedFailed:
    case AppError::AndroidAutoSessionFailed: return "UNLOCK YOUR PHONE AND CHECK ANDROID AUTO";
    case AppError::MicrophoneSemaphoreCreateFailed:
    case AppError::MicrophoneThreadCreateFailed:
    case AppError::MicrophoneThreadStartFailed:
    case AppError::MicrophonePortOpenFailed:
    case AppError::MicrophoneCaptureFailed:
    case AppError::MicrophoneSendFailed: return "MICROPHONE COULD NOT START OR SEND AUDIO";
    case AppError::InputSendFailed: return "COULD NOT SEND INPUT TO YOUR PHONE";
    default: return "COULD NOT START THE SESSION. TRY AGAIN";
    }
}

} // namespace

void showConnectionScreen(ConnectionScreen screen, AppError error, unsigned retrySeconds,
                          bool audioOnVita, bool audioSettingSaved)
{
    const char* title = "CONNECTION ERROR";
    const char* detail = errorMessage(error);
    const char* action = "CROSS: RETRY    CIRCLE: EXIT";
    char code[96]{};
    char countdown[80]{};
    char audio[112]{};
    if (error != AppError::None)
        std::snprintf(code, sizeof(code), "ERROR %d: %s", static_cast<int>(error), appErrorName(error));

    switch (screen) {
    case ConnectionScreen::WaitingForPhone:
        title = "WAITING FOR PHONE";
        detail = "CONNECT YOUR PHONE WITH USB";
        action = "START + SELECT: DISCONNECT    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Connecting:
        title = "CONNECTING";
        detail = "UNLOCK YOUR PHONE AND ALLOW ANDROID AUTO";
        action = "START + SELECT: DISCONNECT    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Disconnecting:
        title = "DISCONNECTING";
        detail = "CLOSING ANDROID AUTO AND RELEASING USB";
        action = "PLEASE WAIT";
        break;
    case ConnectionScreen::Retrying:
        title = error == AppError::UsbConnectTimedOut ? "WAITING FOR PHONE" : "RECONNECTING";
        std::snprintf(countdown, sizeof(countdown), "RETRY IN %u S    CIRCLE: EXIT", retrySeconds);
        action = countdown;
        break;
    case ConnectionScreen::RestartRequired:
        title = "RESTART REQUIRED";
        detail = "SESSION CLEANUP DID NOT COMPLETE";
        action = "RESTART YOUR VITA    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Disconnected:
        title = "DISCONNECTED";
        detail = "USB RELEASED. YOU CAN PRESS HOME";
        action = "CROSS: RECONNECT    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Error:
        break;
    }
    const bool canChangeAudio = screen == ConnectionScreen::Disconnected ||
        screen == ConnectionScreen::Error || screen == ConnectionScreen::Retrying;
    const char* audioHint = canChangeAudio ? "TRIANGLE: CHANGE" :
        (screen == ConnectionScreen::WaitingForPhone || screen == ConnectionScreen::Connecting)
            ? "DISCONNECT TO CHANGE" : "";
    std::snprintf(audio, sizeof(audio), "MUSIC + NAV: %s    %s%s",
        audioOnVita ? "VITA" : "PHONE", audioHint,
        audioSettingSaved ? "" : "    NOT SAVED");
    video::presenter::presentStatus(title, detail, code, action, audio);
}

} // namespace app
