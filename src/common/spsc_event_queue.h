#pragma once

#include <cstddef>
#include <cstdint>

namespace common {

// Single producer/single consumer. One slot is reserved to distinguish full
// from empty, so Capacity includes that slot. Reset only while both are idle.
template <typename Event, size_t Capacity>
class SpscEventQueue {
    static_assert(Capacity >= 4, "SpscEventQueue capacity is too small");

public:
    void reset()
    {
        __atomic_store_n(&head_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&tail_, 0u, __ATOMIC_RELAXED);
    }

    bool push(const Event& event)
    {
        const uint32_t head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const uint32_t next = increment(head);
        if (next == __atomic_load_n(&tail_, __ATOMIC_ACQUIRE))
            return false;

        events_[head] = event;
        __atomic_store_n(&head_, next, __ATOMIC_RELEASE);
        return true;
    }

    bool pop(Event& event)
    {
        const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        if (tail == __atomic_load_n(&head_, __ATOMIC_ACQUIRE))
            return false;

        event = events_[tail];
        __atomic_store_n(&tail_, increment(tail), __ATOMIC_RELEASE);
        return true;
    }

private:
    static uint32_t increment(uint32_t index)
    {
        ++index;
        return index == Capacity ? 0 : index;
    }

    Event events_[Capacity]{};
    volatile uint32_t head_ = 0;
    volatile uint32_t tail_ = 0;
};

} // namespace common
