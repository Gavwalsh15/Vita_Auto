#pragma once

#include <cstdint>

#include "inputs/input_events.h"

namespace android_auto { class DirectSession; }

namespace inputs {

struct DeliveryStats {
    uint32_t dequeued = 0;
    uint32_t delivered = 0;
    uint32_t touchDelivered = 0;
    uint32_t buttonDelivered = 0;
    uint32_t rotarySteps = 0;
    uint32_t movesCoalesced = 0;
    uint32_t maxQueueUs = 0;
};

// Main thread initialize before IO starts
void init();
void stop();
bool startWorker();
void stopWorker();
// Only the worker OR the main-thread may produce events
bool sample();

// IO thread owns the session and consumes queued events. Readiness should be atomic.
void setReady(bool ready);
bool drain(android_auto::DirectSession& session);
// The same queue for a session that is not Android Auto: every event goes to the
// sink in order (a run of moves is reduced to the latest). Stops and returns
// false at the first event the sink refuses.
using EventSink = bool (*)(const InputEvent& event);
bool drainTo(EventSink sink);
DeliveryStats deliveryStats();

} // namespace inputs
