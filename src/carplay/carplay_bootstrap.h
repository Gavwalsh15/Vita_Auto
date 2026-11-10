#pragma once

#include <cstddef>

#include "transport/wireless_config.h"

// Wireless Apple CarPlay (experimental): the Vita dials a paired iPhone over Bluetooth, runs the iAP2
// exchange that ends in the wireless handoff (Wi-Fi network and AirPlay receiver address), then serves
// the AirPlay session. The iAP2/AirPlay/crypto code comes from nbtplay (docs/carplay.md); without it this
// is a stub. No MFi identity is shipped: the user supplies the files (docs/carplay.md).
namespace carplay {

struct Config {
    WirelessConfig wireless;

    // Where the user's identity.pk8 and certificate.p7b are, and where the
    // pairing identity is kept.
    char dataDirectory[96] = "ux0:data/vita_auto/carplay";
};

// Blocks until cancelled() is true. Returns 0 on a clean stop, otherwise one of the
// codes below when the session could not be brought up (or the core is not compiled in).
constexpr int RUN_FAILED = 1;
constexpr int RUN_ACCESS_POINT_FAILED = 2;
int run(const Config& config, bool (*cancelled)());

} // namespace carplay
