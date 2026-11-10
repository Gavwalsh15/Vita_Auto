#include "transport/bt_stack.h"

#include "transport/wireless_status.h"

#include "logger/vita_log.h"
#include "vita_auto_usb.h"

namespace {

constexpr const char* CLIENT_BRIDGE_PATH = "app0:vita_auto_usb_client.suprx";

// Names for the event ids seen so far; the rest are logged as "?". These come
// from what the logs showed, not from documentation, so treat them as hints.
const char* eventName(uint8_t id)
{
    switch (id) {
    case 0x01: return "inquiry result";
    case 0x02: return "inquiry stopped";
    case 0x04: return "pairing code";
    case 0x05: return "connected";
    case 0x06: return "disconnected";
    case 0x07: return "link activity";
    case 0x08: return "incoming class";
    case 0x09: return "device name";
    case 0x15: return "stack state";
    case 0x1c: return "audio config";
    default: return "?";
    }
}

} // namespace

bool BtStack::enable(const char* tag, const char* name, const char* recoverOriginal, uint32_t options)
{
    wireless_status::publishName(wireless_status::NameState::Unknown, "");
    tag_ = tag;
    summaryLogged_ = false;
    nameChanged_ = false;
    nameNoted_ = false;
    mismatched_ = false;
    const int loadResult = vitaAutoUsbClientLoad(CLIENT_BRIDGE_PATH);
    if (loadResult < 0) {
        vitaLog("%s: client bridge unavailable (0x%08x)", tag, static_cast<unsigned>(loadResult));
        return false;
    }
    // The name must be handed over before Enable: the plugin renames the Vita
    // while it sets Bluetooth up.
    const int nameResult = vitaAutoBtConfigureName(name != nullptr ? name : "",
                                                   recoverOriginal != nullptr ? recoverOriginal : "");
    if (nameResult < 0)
        vitaLog("%s: could not set the Bluetooth name (0x%08x)", tag, static_cast<unsigned>(nameResult));
    const int optionsResult = vitaAutoBtSetOptions(options);
    if (optionsResult < 0)
        vitaLog("%s: could not set Bluetooth options (0x%08x)", tag, static_cast<unsigned>(optionsResult));
    const int result = vitaAutoBtEnable();
    if (result < 0) {
        vitaLog("%s: Bluetooth enable failed 0x%08x", tag, static_cast<unsigned>(result));
        return false;
    }
    enabled_ = true;
    return true;
}

void BtStack::disable(const char* tag)
{
    release();
    if (!enabled_)
        return;
    const int result = vitaAutoBtDisable();
    enabled_ = false;
    if (result < 0)
        vitaLog("%s: Bluetooth disable refused (0x%08x)", tag, static_cast<unsigned>(result));
    else
        vitaLog("%s: Bluetooth disabled", tag);
    // Disable puts the Vita's name back, so the saved copy is no longer needed.
    if (nameChanged_) {
        wireless_status::publishName(wireless_status::NameState::Restored, "");
        nameChanged_ = false;
    }
}

int BtStack::claim()
{
    const int result = vitaAutoBtStart();
    if (result >= 0)
        claimed_ = true;
    return result;
}

void BtStack::release()
{
    if (claimed_) {
        vitaAutoBtStop();
        claimed_ = false;
    }
}

BtStack::Status BtStack::poll(VitaAutoBtInfo& info)
{
    logEvents();
    if (!readInfo(info)) {
        if (!mismatched_)
            return Status::Starting;
        release();
        wireless_status::publish(wireless_status::Progress::Failed, -3);
        return Status::Failed;
    }
    if (info.state == VITA_AUTO_BT_FAILED) {
        vitaLog("%s: Bluetooth failed stage=%d result=0x%08x", tag_, info.failureStage,
                static_cast<unsigned>(info.failureResult));
        if (info.failureStage == VITA_AUTO_BT_FAILURE_CALLBACK &&
            static_cast<unsigned>(info.failureResult) == 0x802f0c02u)
            vitaLog("%s: SceBt has no free callback slot (earlier runs of an older plugin leaked "
                    "them); restart the Vita to clear it", tag_);
        release();
        wireless_status::publish(wireless_status::Progress::Failed, info.failureStage);
        return Status::Failed;
    }
    if (info.state != VITA_AUTO_BT_READY && info.state != VITA_AUTO_BT_LINKED)
        return Status::Starting;
    if (!summaryLogged_) {
        summaryLogged_ = true;
        vitaLog("%s: stack up. SDP record %s, service search patch %s (result %d), RFCOMM credit "
                "patch %s (result %d), hooks %s (result %d), iAP2 client %s (result %d)",
                tag_, info.patchVerified ? "verified" : "NOT verified",
                info.searchPatched ? "applied" : "NOT applied", info.searchResult,
                info.rfcommPatched ? "applied" : "NOT applied", info.rfcommResult,
                info.rfcommHooked ? "in" : "NOT in", info.rfcommHookResult,
                info.iap2Installed ? "running" : "not running", info.iap2InstallResult);
    }
    noteName(info);
    return Status::Ready;
}

void BtStack::noteName(const VitaAutoBtInfo& info)
{
    if (nameNoted_)
        return;
    nameNoted_ = true;
    if (info.nameChanged) {
        char original[64] = {};
        if (vitaAutoBtGetOriginalName(original, sizeof(original)) > 0) {
            wireless_status::publishName(wireless_status::NameState::Changed, original);
            nameChanged_ = true;
        }
        vitaLog("%s: Bluetooth name changed (the Vita's own name is saved in settings)", tag_);
    } else {
        vitaLog("%s: Bluetooth name not changed (0x%08x)", tag_, static_cast<unsigned>(info.nameResult));
    }
}

bool BtStack::readInfo(VitaAutoBtInfo& info)
{
    info = VitaAutoBtInfo{};
    if (vitaAutoBtGetInfo(&info) < 0)
        return false;
    if (info.layoutSize != sizeof(info)) {
        if (!mismatched_)
            vitaLog("bt: the kernel plugin is not the build this app was made with (it reports layout %u, "
                    "the app expects %u); install all three files from dist/ and reboot the Vita",
                    static_cast<unsigned>(info.layoutSize), static_cast<unsigned>(sizeof(info)));
        mismatched_ = true;
        return false;
    }
    return true;
}

void BtStack::logEvents()
{
    VitaAutoBtEvent events[8];
    for (;;) {
        const int count = vitaAutoBtReadEvents(events, 8);
        if (count <= 0)
            return;
        for (int i = 0; i < count; i++) {
            const uint8_t* d = events[i].data;
            if (VITA_AUTO_VERBOSE_LOGGING)
                vitaLog("bt-event: t=%u id=0x%02x (%s) raw=%02x%02x%02x%02x %02x%02x%02x%02x "
                        "%02x%02x%02x%02x %02x%02x%02x%02x",
                        static_cast<unsigned>(events[i].timeMs), d[0], eventName(d[0]),
                        d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                        d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        }
    }
}
