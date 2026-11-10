#include "app/connection_screen.h"
#include "transport/wireless_status.h"
#include "video/status_screen.h"
#include "video/video_presenter.h"

#include <cstdio>

namespace app {
namespace {

const char* errorMessage(AppError error)
{
    switch (error) {
    case AppError::WifiApFailed: return "THE VITA'S WI-FI HOTSPOT WOULD NOT START. RESTART THE VITA";
    case AppError::CarplayStartFailed: return "COULD NOT START CARPLAY. CHECK THE LOG";
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

const char* waitingDetail(ConnectionMode mode, bool carplay)
{
    if (carplay)
        return "CONNECT YOUR PAIRED IPHONE OVER BLUETOOTH";
    const bool bluetooth = wireless_status::progress() != wireless_status::Progress::Off;
    switch (mode) {
    case ConnectionMode::Usb: return "CONNECT YOUR PHONE WITH USB";
    case ConnectionMode::Wireless:
        return bluetooth ? "CONNECT YOUR PHONE OVER BLUETOOTH, OR START HEADUNIT SERVER"
                         : "TURN ON YOUR HOTSPOT AND START HEADUNIT SERVER";
    case ConnectionMode::Auto: break;
    }
    return bluetooth ? "PLUG IN USB, OR CONNECT YOUR PHONE OVER BLUETOOTH"
                     : "PLUG IN USB, OR START HEADUNIT SERVER FOR WI-FI";
}

// How far the Bluetooth bootstrap got, for the waiting screen. Null while it is
// simply advertising (or off), so the normal instruction shows.
const char* bluetoothDetail(char* buffer, size_t size)
{
    switch (wireless_status::progress()) {
    case wireless_status::Progress::NoWifi:
        return "BLUETOOTH: PHONE LINKED. JOIN WI-FI ON THE VITA";
    case wireless_status::Progress::Sending:
        return "BLUETOOTH: PHONE LINKED. WAITING FOR ITS REPLY";
    case wireless_status::Progress::NoChannel:
        return "BLUETOOTH: PHONE LINKED BUT NOT STARTING ANDROID AUTO";
    case wireless_status::Progress::SendFailed:
        return "BLUETOOTH: PHONE LINKED. SEND FAILED, RETRYING";
    case wireless_status::Progress::DetailsSent:
        return "BLUETOOTH: WI-FI DETAILS SENT TO PHONE";
    case wireless_status::Progress::WaitingForWifi:
        return "BLUETOOTH: WAITING FOR PHONE TO CONNECT OVER WI-FI";
    case wireless_status::Progress::PhoneRejected:
        // -11 is what a phone reports when the network the Vita named is its own
        // hotspot: it looks for it as a client and never finds it.
        if (wireless_status::failureStage() == -11)
            return "BLUETOOTH: PHONE CANNOT FIND THE WI-FI. IT CANNOT JOIN ITS OWN HOTSPOT";
        std::snprintf(buffer, size, "BLUETOOTH: PHONE COULD NOT JOIN THE WI-FI (STATUS %d)",
                      wireless_status::failureStage());
        return buffer;
    case wireless_status::Progress::Failed:
        std::snprintf(buffer, size, "BLUETOOTH FAILED (STAGE %d). USB AND WI-FI STILL WORK",
                      wireless_status::failureStage());
        return buffer;
    case wireless_status::Progress::Off:
    case wireless_status::Progress::Waiting:
        break;
    }
    return nullptr;
}

// The modes in the order Square cycles through them.
const char* const MODE_NAMES[] = {"AUTO", "USB", "WI-FI", "CARPLAY"};
const char* const MUSIC_CHOICES[] = {"VITA", "PHONE"};

int modeIndex(ConnectionMode mode, bool carplay)
{
    if (carplay)
        return 3;
    switch (mode) {
    case ConnectionMode::Usb: return 1;
    case ConnectionMode::Wireless: return 2;
    case ConnectionMode::Auto: break;
    }
    return 0;
}

// One line saying what the active mode does.
const char* modeDescription(int index)
{
    switch (index) {
    case 1: return "ANDROID AUTO OVER USB ONLY";
    case 2: return "ANDROID AUTO OVER WI-FI ONLY (BLUETOOTH OR HEADUNIT SERVER)";
    case 3: return "APPLE CARPLAY OVER WIRELESS";
    default: return "ANDROID AUTO OVER USB OR WI-FI, WHICHEVER CONNECTS FIRST";
    }
}

} // namespace

void showConnectionScreen(ConnectionScreen screen, AppError error, unsigned retrySeconds,
                          bool audioOnVita, bool settingsSaved, ConnectionMode mode,
                          bool canChangeMode, bool carplay)
{
    // CarPlay is wireless only and ignores the connection mode.
    if (carplay)
        mode = ConnectionMode::Wireless;
    const char* protocol = carplay ? "CARPLAY" : "ANDROID AUTO";
    const char* title = "CONNECTION ERROR";
    const char* detail = errorMessage(error);
    if (error == AppError::None || error == AppError::UsbConnectTimedOut)
        detail = waitingDetail(mode, carplay);
    const char* action = "CROSS: RETRY    CIRCLE: EXIT";
    char note[96]{};
    char countdown[80]{};
    char musicHint[64]{};
    char btText[80]{};
    char detailText[80]{};
    if (error != AppError::None)
        std::snprintf(note, sizeof(note), "ERROR %d: %s", static_cast<int>(error), appErrorName(error));
    const int activeMode = modeIndex(mode, carplay);
    if (error == AppError::None)
        std::snprintf(note, sizeof(note), "%s", modeDescription(activeMode));

    switch (screen) {
    case ConnectionScreen::WaitingForPhone: {
        title = "WAITING FOR PHONE";
        detail = waitingDetail(mode, carplay);
        const char* bluetooth = mode != ConnectionMode::Usb
            ? bluetoothDetail(btText, sizeof(btText)) : nullptr;
        if (bluetooth != nullptr)
            detail = bluetooth;
        action = "START + SELECT: DISCONNECT    CIRCLE: EXIT";
        break;
    }
    case ConnectionScreen::Connecting:
        title = "CONNECTING";
        std::snprintf(detailText, sizeof(detailText), "UNLOCK YOUR %s AND ALLOW %s",
                      carplay ? "IPHONE" : "PHONE", protocol);
        detail = detailText;
        action = "START + SELECT: DISCONNECT    CIRCLE: EXIT";
        break;
    case ConnectionScreen::Disconnecting:
        title = "DISCONNECTING";
        if (mode == ConnectionMode::Wireless)
            std::snprintf(detailText, sizeof(detailText), "CLOSING %s", protocol);
        else
            std::snprintf(detailText, sizeof(detailText), "CLOSING %s AND RELEASING USB", protocol);
        detail = detailText;
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
    std::snprintf(musicHint, sizeof(musicHint), "%s%s", audioHint, settingsSaved ? "" : "  NOT SAVED");

    // When the mode cannot be changed now, show only the one in use.
    const char* const* modeChoices = MODE_NAMES;
    int modeCount = 4;
    int modeSelected = activeMode;
    if (!canChangeMode) {
        modeChoices = &MODE_NAMES[activeMode];
        modeCount = 1;
        modeSelected = 0;
    }
    video::ChoiceRow rows[2] = {
        {"MODE", modeChoices, modeCount, modeSelected, canChangeMode ? "SQUARE: CHANGE" : ""},
        {"MUSIC + NAV", MUSIC_CHOICES, 2, audioOnVita ? 0 : 1, musicHint},
    };
    // The music setting only applies to Android Auto.
    video::presenter::presentStatus(title, detail, note, action, rows, carplay ? 1 : 2);
}

} // namespace app
