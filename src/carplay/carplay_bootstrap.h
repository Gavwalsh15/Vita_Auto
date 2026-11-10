#pragma once

#include <cstddef>

#include "app/app_errors.h"
#include "carplay/audio_offer.h"
#include "transport/wireless_config.h"

// Wireless Apple CarPlay: the Vita dials a paired iPhone over Bluetooth, runs the iAP2
// exchange that ends in the wireless handoff (Wi-Fi network and AirPlay receiver address), then serves
// the AirPlay session. The iAP2/AirPlay/crypto code is in carplay. No MFi identity is shipped: the
// user supplies the files (docs/carplay.md).
namespace carplay {

struct Config {
    WirelessConfig wireless;
    AudioOffer audioOffer = AudioOffer::Full;

    // Where the user's identity.pk8 and certificate.p7b are, and where the
    // pairing identity is kept.
    char dataDirectory[96] = "ux0:data/vita_auto/carplay";
};

app::AppError run(const Config& config, bool (*cancelled)());

} // namespace carplay
