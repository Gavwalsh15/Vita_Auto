#pragma once

#include <cstdint>

struct VitaAutoBtInfo; // vita_auto_usb.h

// The kernel plugin's Bluetooth stack as the app uses it, shared by the Android Auto
// and CarPlay bootstraps: bring-up, the saved device name, the serial stream claim
// and the raw event log.
class BtStack {
public:
    // Loads the bridge, hands over the name, sets the plugin options and enables
    // Bluetooth. Failures are logged under tag.
    bool enable(const char* tag, const char* name, const char* recoverOriginal, uint32_t options);
    // Releases the stream, disables Bluetooth and reports the name as restored.
    void disable(const char* tag);
    bool enabled() const { return enabled_; }

    // The phone's serial stream: only one reader at a time. claim() returns the
    // plugin's result (negative: not claimed).
    int claim();
    void release();
    bool claimed() const { return claimed_; }

    // Once per enable, after the stack is up: publishes the saved original name when
    // the Vita was renamed. Returns whether it was.
    bool noteName(const VitaAutoBtInfo& info);

    // The plugin's status. False when it could not be read, or when the plugin is
    // not the build this app was made with (mismatched() then says so).
    bool readInfo(VitaAutoBtInfo& info);
    bool mismatched() const { return mismatched_; }

    // Writes the queued raw stack events to the log.
    void logEvents();

private:
    bool enabled_ = false;
    bool claimed_ = false;
    bool nameChanged_ = false;
    bool nameNoted_ = false;
    bool mismatched_ = false;
};
