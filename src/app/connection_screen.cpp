#include "app/connection_screen.h"
#include "video/video_presenter.h"

#include <cstdio>

namespace app {
namespace {

const char* errorMessage(AppError error)
{
    switch (error) {
    case AppError::NetInitFailed: return "COULD NOT START WI-FI NETWORKING";
    case AppError::NetNotConnected: return "JOIN YOUR PHONE'S HOTSPOT IN VITA SETTINGS";
    case AppError::NetNoGateway: return "COULD NOT FIND YOUR PHONE ON THE HOTSPOT";
    case AppError::NetSendRejected:
    case AppError::NetWriteFailed:
    case AppError::NetWriteTimedOut:
    case AppError::NetReadFailed:
    case AppError::NetPeerClosed: return "WI-FI TO YOUR PHONE WAS LOST";
    case AppError::None: return "CONNECT YOUR PHONE WITH USB";
    case AppError::UsbConnectTimedOut: return "CONNECT YOUR PHONE WITH USB";
    case AppError::UsbBridgeLoadFailed: return "CHECK THE USB PLUGIN AND REBOOT";
    case AppError::UsbHostStartFailed: return "COULD NOT START THE USB PORT";
    case AppError::UsbEnumerationFailed: return "CHECK THE USB CABLE AND UNLOCK YOUR PHONE";
    case AppError::UsbStaleAccessory: return "UNPLUG AND REPLUG YOUR PHONE, THEN RETRY";
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

const char* waitingDetail(ConnectionMode mode)
{
    switch (mode) {
    case ConnectionMode::Usb: return "CONNECT YOUR PHONE WITH USB";
    case ConnectionMode::Wireless: return "TURN ON YOUR HOTSPOT AND START HEADUNIT SERVER";
    case ConnectionMode::Auto: break;
    }
    return "PLUG IN USB, OR START HEADUNIT SERVER FOR WI-FI";
}

const char* modeLabel(ConnectionMode mode)
{
    switch (mode) {
    case ConnectionMode::Usb: return "USB ONLY";
    case ConnectionMode::Wireless: return "WI-FI ONLY";
    case ConnectionMode::Auto: break;
    }
    return "AUTO";
}

} // namespace

void showConnectionScreen(ConnectionScreen screen, AppError error, unsigned retrySeconds,
                          bool audioOnVita, bool settingsSaved, ConnectionMode mode,
                          bool canChangeMode)
{
    const char* title = "CONNECTION ERROR";
    const char* detail = errorMessage(error);
    if (error == AppError::None || error == AppError::UsbConnectTimedOut)
        detail = waitingDetail(mode);
    const char* action = "CROSS: RETRY    CIRCLE: EXIT";
    char code[96]{};
    char countdown[80]{};
    char audio[112]{};
    if (error != AppError::None)
        std::snprintf(code, sizeof(code), "ERROR %d: %s", static_cast<int>(error), appErrorName(error));
    else
        std::snprintf(code, sizeof(code), "CONNECTION: %s%s", modeLabel(mode),
                      canChangeMode ? "    SQUARE: CHANGE" : "");

    switch (screen) {
    case ConnectionScreen::WaitingForPhone:
        title = "WAITING FOR PHONE";
        detail = waitingDetail(mode);
        action = "START + SELECT: DISCONNECT    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Connecting:
        title = "CONNECTING";
        detail = "UNLOCK YOUR PHONE AND ALLOW ANDROID AUTO";
        action = "START + SELECT: DISCONNECT    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Disconnecting:
        title = "DISCONNECTING";
        detail = mode == ConnectionMode::Wireless ? "CLOSING ANDROID AUTO"
                                                  : "CLOSING ANDROID AUTO AND RELEASING USB";
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
        detail = mode == ConnectionMode::Wireless ? "DISCONNECTED. YOU CAN PRESS HOME"
                                                  : "USB RELEASED. YOU CAN PRESS HOME";
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
        settingsSaved ? "" : "    NOT SAVED");
    video::presenter::presentStatus(title, detail, code, action, audio);
}

} // namespace app
