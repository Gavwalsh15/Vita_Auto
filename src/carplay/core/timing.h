#pragma once

#include "rt.h"

namespace carplay::core {

constexpr size_t kTimingPacketBytes = 32;

class TimingClock {
public:
    void request(int64_t now_ns, uint8_t packet[kTimingPacketBytes]);
    bool on_packet(const uint8_t *data, size_t length, int64_t now_ns, uint8_t reply[kTimingPacketBytes]);

    bool synced() const { return synced_; }
    int64_t offset_ns() const { return offset_ns_; }
    uint32_t responses() const { return responses_; }
    uint64_t synced_ntp(int64_t now_ns) const;

private:
    void response(const uint8_t *packet, int64_t now_ns);
    void apply(int64_t offset_ns);

    int64_t offset_ns_ = 0;
    uint64_t pending_t1_ = 0;
    bool pending_ = false, synced_ = false;
    int64_t delays_[8] = {};
    bool delay_valid_[8] = {};
    unsigned delay_index_ = 0, pick_count_ = 2;
    int64_t pick_rtt_ = 0, pick_offset_ = 0;
    bool pick_valid_ = false;
    uint32_t responses_ = 0;
};

uint64_t ntp_from_nanos(int64_t nanos);
int64_t nanos_from_ntp_delta(int64_t delta);

} // namespace carplay::core
