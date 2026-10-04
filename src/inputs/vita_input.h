#pragma once

#include <cstdint>

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
DeliveryStats deliveryStats();

} // namespace inputs
