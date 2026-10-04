#pragma once

#include "inputs/input_events.h"

namespace inputs {

using ButtonSender = bool (*)(const ButtonEvent& event);

void initControllerInput();
void stopControllerInput();
void pollControllerInput(ButtonSender sender);

} // namespace inputs
