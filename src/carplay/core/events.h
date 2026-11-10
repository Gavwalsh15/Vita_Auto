// Ported from DiPlay's AirPlayHid (GPL-3.0).
#pragma once

#include "rt.h"

namespace carplay::core {

struct KnobState {
    bool select = false, home = false, back = false;
    int8_t x = 0, y = 0, wheel = 0;
};

void knob_report(const KnobState &state, uint8_t report[4]);

} // namespace carplay::core
