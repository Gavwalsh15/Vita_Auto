#include "events.h"

namespace carplay::core {

void knob_report(const KnobState &state, uint8_t report[4]) {
    report[0] = static_cast<uint8_t>((state.select ? 0x01 : 0) | (state.home ? 0x02 : 0) | (state.back ? 0x04 : 0));
    report[1] = static_cast<uint8_t>(state.x);
    report[2] = static_cast<uint8_t>(state.y);
    report[3] = static_cast<uint8_t>(state.wheel);
}

} // namespace carplay::core
