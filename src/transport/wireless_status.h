#pragma once

#include <cstddef>

namespace wireless_status {

enum class Progress {
    Off,            // Not enabled
    Waiting,        // Advertising
    NoWifi,         // Phone linked, but the Vita is not on Wi-Fi
    Sending,        // Waiting for the phone's reply
    NoChannel,      // Phone linked, but it has not opened channel
    SendFailed,     // The first Bluetooth send failed
    DetailsSent,    // Sent the Wi-Fi details
    WaitingForWifi, // Waiting for its TCP connection
    PhoneRejected,  // Phone reported a non-zero Wi-Fi status
    Failed          // Bluetooth unusable on this run
};

// The Vita's own device name, while it is renamed for Bluetooth
enum class NameState { Unknown, Changed, Restored };

Progress progress();
int failureStage();
NameState nameState(char* original, size_t size);

// Called by the bootstrap.
void publish(Progress progress, int failureStage = 0);
void publishName(NameState state, const char* original);

} // namespace wireless_status
