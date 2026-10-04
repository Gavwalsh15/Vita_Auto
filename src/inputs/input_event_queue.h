#pragma once

#include "common/spsc_event_queue.h"
#include "inputs/input_events.h"

namespace inputs {

// Platform sampling produces events; the USB session consumes them.
template <size_t Capacity>
using InputEventQueue = common::SpscEventQueue<InputEvent, Capacity>;

} // namespace inputs
