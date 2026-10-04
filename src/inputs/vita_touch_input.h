#pragma once

#include "inputs/input_events.h"

namespace inputs {

using TouchSender = bool (*)(const TouchEvent& event);

void initTouchInput();
void stopTouchInput();
void pollTouchInput(TouchSender sender);

} // namespace inputs
