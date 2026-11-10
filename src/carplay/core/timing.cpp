#include "timing.h"

namespace carplay::core {

namespace {

const uint8_t kRequest = 210, kResponse = 211;
const size_t kOriginate = 8, kReceive = 16, kTransmit = 24;
const int64_t kStepThresholdNs = 128000000;
const uint64_t kNanosPerSecond = 1000000000ull;

uint64_t divide(uint64_t value, uint64_t divisor, uint64_t &remainder) {
    uint64_t quotient = 0, rest = 0;
    for (int bit = 63; bit >= 0; --bit) {
        rest = rest << 1 | (value >> bit & 1);
        if (rest >= divisor) {
            rest -= divisor;
            quotient |= 1ull << bit;
        }
    }
    remainder = rest;
    return quotient;
}

void put_ntp(uint8_t *p, uint64_t value) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (56 - 8 * i));
}

uint64_t get_ntp(const uint8_t *p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = value << 8 | p[i];
    return value;
}

void header(uint8_t *packet, uint8_t type) {
    memset(packet, 0, kTimingPacketBytes);
    packet[0] = 0x80;
    packet[1] = type;
    packet[3] = 7; // length in 32-bit words minus one
}

int64_t halve(int64_t value) { return value < 0 ? -static_cast<int64_t>(static_cast<uint64_t>(-value) >> 1) : value >> 1; }
int64_t eighth(int64_t value) { return value < 0 ? -static_cast<int64_t>(static_cast<uint64_t>(-value) >> 3) : value >> 3; }
int64_t magnitude(int64_t value) { return value < 0 ? -value : value; }

} // namespace

uint64_t ntp_from_nanos(int64_t nanos) {
    // Negative times (before the local epoch) wrap like the unsigned NTP value would.
    uint64_t magnitude_ns = nanos < 0 ? static_cast<uint64_t>(-nanos) : static_cast<uint64_t>(nanos), rest = 0;
    uint64_t seconds = divide(magnitude_ns, kNanosPerSecond, rest);
    uint64_t value = seconds << 32 | divide(rest << 32, kNanosPerSecond, rest);
    return nanos < 0 ? 0 - value : value;
}

int64_t nanos_from_ntp_delta(int64_t delta) {
    uint64_t m = static_cast<uint64_t>(magnitude(delta));
    uint64_t nanos = (m >> 32) * kNanosPerSecond + (((m & 0xffffffffu) * kNanosPerSecond) >> 32);
    return delta < 0 ? -static_cast<int64_t>(nanos) : static_cast<int64_t>(nanos);
}

uint64_t TimingClock::synced_ntp(int64_t now_ns) const { return ntp_from_nanos(now_ns + offset_ns_); }

void TimingClock::request(int64_t now_ns, uint8_t packet[kTimingPacketBytes]) {
    header(packet, kRequest);
    pending_t1_ = synced_ntp(now_ns);
    pending_ = true;
    put_ntp(packet + kTransmit, pending_t1_);
}

bool TimingClock::on_packet(const uint8_t *data, size_t length, int64_t now_ns, uint8_t reply[kTimingPacketBytes]) {
    if (length < kTimingPacketBytes) return false;
    if (data[1] == kResponse) {
        response(data, now_ns);
        return false;
    }
    if (data[1] != kRequest) return false;
    header(reply, kResponse);
    memcpy(reply + kOriginate, data + kTransmit, 8);
    put_ntp(reply + kReceive, synced_ntp(now_ns));
    put_ntp(reply + kTransmit, synced_ntp(now_ns));
    return true;
}

void TimingClock::response(const uint8_t *packet, int64_t now_ns) {
    const uint64_t t4 = synced_ntp(now_ns), t1 = get_ntp(packet + kOriginate);
    const uint64_t t2 = get_ntp(packet + kReceive), t3 = get_ntp(packet + kTransmit);
    if (!pending_ || t1 != pending_t1_) return;
    pending_ = false;
    ++responses_;
    const int64_t offset = nanos_from_ntp_delta(halve(static_cast<int64_t>(t2 - t1) + static_cast<int64_t>(t3 - t4)));
    const int64_t rtt = nanos_from_ntp_delta(static_cast<int64_t>(t4 - t1) - static_cast<int64_t>(t3 - t2));
    if (rtt < 0) return;
    if (!pick_valid_ || rtt < pick_rtt_) {
        pick_rtt_ = rtt;
        pick_offset_ = offset;
        pick_valid_ = true;
    }
    if (--pick_count_ > 0) return;
    pick_count_ = 2;
    pick_valid_ = false;
    bool use = true;
    for (unsigned i = 0; i < 8; ++i)
        if (delay_valid_[i] && pick_rtt_ > delays_[i]) use = false;
    delays_[delay_index_] = pick_rtt_;
    delay_valid_[delay_index_] = true;
    delay_index_ = (delay_index_ + 1) & 7;
    if (use) apply(pick_offset_);
}

void TimingClock::apply(int64_t offset_ns) {
    const bool stepping = !synced_ || magnitude(offset_ns) > kStepThresholdNs;
    offset_ns_ += stepping ? offset_ns : eighth(offset_ns);
    if (!stepping) return;
    for (unsigned i = 0; i < 8; ++i) delay_valid_[i] = false;
    delay_index_ = 0;
    pending_ = false;
    synced_ = true;
}

} // namespace carplay::core
