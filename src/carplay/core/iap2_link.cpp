// Port of DiPlay's Iap2LinkEngine
#include "iap2_link.h"

namespace carplay::core {

const uint8_t kIap2Marker[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};

namespace {

constexpr size_t kSessionDescriptorBytes = 3;
constexpr size_t kSynchronizationFixedBytes = 10;
constexpr uint8_t kSynchronizationVersion = 1;
constexpr uint8_t kLinkStartHigh = 0xff;
constexpr uint8_t kLinkStartLow = 0x5a;
constexpr uint8_t kControlSynchronize = 0x80;
constexpr uint8_t kControlAcknowledgement = 0x40;
constexpr uint8_t kControlExtendedAcknowledgement = 0x20;
constexpr uint8_t kControlReset = 0x10;
constexpr uint8_t kInitialSequence = 99;
constexpr int64_t kMarkerResendMs = 1000;
constexpr int64_t kSynchronizationResendMs = 500;
constexpr uint16_t kDefaultRetransmissionTimeoutMs = 4000;
constexpr uint8_t kDefaultMaxRetransmissions = 4;
constexpr uint8_t kDefaultMaxAcknowledgements = 3;
constexpr int kAckSlack = 10;

uint16_t read_u16(const uint8_t *bytes) { return static_cast<uint16_t>((bytes[0] << 8) | bytes[1]); }

bool checksum_valid(const uint8_t *bytes, size_t length) {
    unsigned sum = 0;
    for (size_t i = 0; i < length; ++i) sum += bytes[i];
    return (sum & 0xff) == 0;
}

uint8_t checksum(const uint8_t *bytes, size_t length) {
    unsigned sum = 0;
    for (size_t i = 0; i < length; ++i) sum += bytes[i];
    return static_cast<uint8_t>(-sum);
}

int sequence_distance(uint8_t sequence, uint8_t previous) { return (sequence - previous) & 0xff; }

uint8_t next_sequence(uint8_t sequence) { return static_cast<uint8_t>(sequence + 1); }

void arm_min(int64_t &result, int64_t deadline) {
    if (deadline >= 0 && (result < 0 || deadline < result)) result = deadline;
}

} // namespace

bool Iap2SyncPayload::encode(Bytes &out) const {
    uint8_t fixed[kSynchronizationFixedBytes] = {
        kSynchronizationVersion,
        max_outgoing,
        static_cast<uint8_t>(max_length >> 8),
        static_cast<uint8_t>(max_length),
        static_cast<uint8_t>(retransmission_timeout_ms >> 8),
        static_cast<uint8_t>(retransmission_timeout_ms),
        static_cast<uint8_t>(acknowledgement_timeout_ms >> 8),
        static_cast<uint8_t>(acknowledgement_timeout_ms),
        max_retransmissions,
        max_acknowledgements,
    };
    if (!out.assign(fixed, sizeof fixed)) return false;
    for (uint8_t i = 0; i < session_count; ++i) {
        const uint8_t descriptor[kSessionDescriptorBytes] = {sessions[i].id, sessions[i].kind, sessions[i].version};
        if (!out.append(descriptor, sizeof descriptor)) return false;
    }
    return true;
}

bool Iap2SyncPayload::decode(const uint8_t *bytes, size_t length, Iap2SyncPayload &sync) {
    if (length < kSynchronizationFixedBytes || bytes[0] != kSynchronizationVersion) return false;
    Iap2SyncPayload decoded;
    decoded.max_outgoing = bytes[1];
    decoded.max_length = read_u16(bytes + 2);
    decoded.retransmission_timeout_ms = read_u16(bytes + 4);
    decoded.acknowledgement_timeout_ms = read_u16(bytes + 6);
    decoded.max_retransmissions = bytes[8];
    decoded.max_acknowledgements = bytes[9];
    for (size_t offset = kSynchronizationFixedBytes;
         offset + kSessionDescriptorBytes <= length && decoded.session_count < kIap2MaxSessions;
         offset += kSessionDescriptorBytes) {
        decoded.sessions[decoded.session_count++] = {bytes[offset], bytes[offset + 1], bytes[offset + 2]};
    }
    sync = decoded;
    return true;
}

bool Iap2LinkConfig::valid() const {
    return maximum_queued_packets >= 1 && maximum_queued_packets <= kIap2MaxPackets &&
           maximum_out_of_order_packets >= 1 && maximum_out_of_order_packets <= kIap2MaxPackets &&
           maximum_pending_output_bytes >= 1 && maximum_pending_events >= 1 &&
           maximum_pending_events <= kIap2MaxPackets;
}

Iap2Link *Iap2Link::create(const Iap2LinkConfig &config) {
    if (!config.valid()) return nullptr;
    void *memory = malloc(sizeof(Iap2Link));
    return memory ? new (memory) Iap2Link(config) : nullptr;
}

void Iap2Link::destroy(Iap2Link *link) {
    if (!link) return;
    link->~Iap2Link();
    free(link);
}

Iap2Link::Iap2Link(const Iap2LinkConfig &config) : config_(config), sent_sequence_(kInitialSequence) {
    peer_sync_ = local_sync();
}

int64_t Iap2Link::next_deadline() const {
    int64_t result = -1;
    arm_min(result, marker_deadline_ms_);
    arm_min(result, synchronization_deadline_ms_);
    arm_min(result, send_acknowledgement_deadline_ms_);
    arm_min(result, receive_acknowledgement_deadline_ms_);
    return result;
}

void Iap2Link::start(bool wired_initiator, int64_t now_ms) {
    if (state_ != State::Idle) return;
    state_ = State::Detecting;
    if (!append_output(kIap2Marker, sizeof kIap2Marker)) return;
    marker_deadline_ms_ = now_ms + kMarkerResendMs;
    if (wired_initiator && state_ != State::Dead) enter_negotiating(now_ms);
}

void Iap2Link::feed(const uint8_t *bytes, size_t length, int64_t now_ms) {
    if (state_ == State::Dead || length == 0) return;
    size_t offset = 0;
    while (offset < length && state_ != State::Dead) {
        size_t room = receive_room();
        if (room == 0) {
            if (!parse_available(now_ms)) {
                die("iAP2 inbound frame exceeds 65535 bytes");
                return;
            }
            continue;
        }
        size_t count = std::min(room, length - offset);
        memcpy(receive_ + receive_tail_, bytes + offset, count);
        receive_tail_ += count;
        offset += count;
        while (parse_available(now_ms) && state_ != State::Dead) {
        }
    }
}

void Iap2Link::advance_time(int64_t now_ms) {
    while (state_ != State::Dead) {
        int64_t deadline = next_deadline();
        if (deadline < 0 || deadline > now_ms) return;
        if (marker_deadline_ms_ >= 0 && marker_deadline_ms_ <= now_ms) {
            marker_deadline_ms_ = -1;
            if (state_ == State::Detecting && append_output(kIap2Marker, sizeof kIap2Marker))
                marker_deadline_ms_ = now_ms + kMarkerResendMs;
        } else if (synchronization_deadline_ms_ >= 0 && synchronization_deadline_ms_ <= now_ms) {
            synchronization_deadline_ms_ = -1;
            if (state_ == State::Negotiating) {
                send_synchronization();
                if (state_ != State::Dead) synchronization_deadline_ms_ = now_ms + kSynchronizationResendMs;
            }
        } else if (send_acknowledgement_deadline_ms_ >= 0 && send_acknowledgement_deadline_ms_ <= now_ms) {
            send_acknowledgement_deadline_ms_ = -1;
            if (state_ == State::Normal) {
                has_last_acknowledged_received_ = true;
                last_acknowledged_received_ = last_received_in_order_;
                send_acknowledgement();
            }
        } else if (receive_acknowledgement_deadline_ms_ >= 0 && receive_acknowledgement_deadline_ms_ <= now_ms) {
            receive_acknowledgement_deadline_ms_ = -1;
            retransmit_due_packet(now_ms);
        }
    }
}

bool Iap2Link::send_control(const uint8_t *bytes, size_t length, int64_t now_ms) {
    return send_session(kIap2ControlSessionId, bytes, length, now_ms);
}

bool Iap2Link::send_session(uint8_t session_id, const uint8_t *bytes, size_t length, int64_t now_ms) {
    if (length > kIap2MaxPayloadBytes) return false;
    if ((peer_sync_received_ || state_ == State::Normal) && !peer_payload_acceptable(length)) return false;
    Packet packet;
    packet.session_id = session_id;
    if (!packet.payload.assign(bytes, length)) return false;
    send_packet(std::move(packet), now_ms);
    return true;
}

bool Iap2Link::poll_event(Iap2Event &event) {
    if (events_.empty()) return false;
    event = events_.take(0);
    return true;
}

bool Iap2Link::parse_available(int64_t now_ms) {
    if (state_ == State::Dead) return false;
    const uint8_t *head = receive_ + receive_head_;
    if (state_ == State::Detecting) {
        if (receive_size() < sizeof kIap2Marker) return false;
        if (memcmp(head, kIap2Marker, sizeof kIap2Marker) != 0) {
            die("iAP2 marker was not received");
            return false;
        }
        receive_discard(sizeof kIap2Marker);
        enter_negotiating(now_ms);
        return true;
    }

    if (awaiting_payload_) {
        size_t with_checksum = pending_header_.length - kIap2LinkHeaderBytes;
        if (receive_size() < with_checksum) return false;
        awaiting_payload_ = false;
        Header header = pending_header_;
        if (checksum_valid(head, with_checksum)) {
            Bytes payload;
            if (!payload.assign(head, with_checksum - kIap2LinkChecksumBytes)) {
                die("out of memory");
                return false;
            }
            receive_discard(with_checksum);
            process_frame(header, payload.data(), payload.size(), true, now_ms);
        } else {
            receive_discard(with_checksum);
        }
        return true;
    }

    bool discarded = false;
    while (receive_size() >= 2 && (receive_[receive_head_] != kLinkStartHigh ||
                                   receive_[receive_head_ + 1] != kLinkStartLow)) {
        receive_discard(1);
        discarded = true;
    }
    if (receive_size() < kIap2LinkHeaderBytes) return discarded;
    const uint8_t *raw = receive_ + receive_head_;
    bool header_ok = checksum_valid(raw, kIap2LinkHeaderBytes);
    Header header = {read_u16(raw + 2), raw[4], raw[5], raw[6], raw[7]};
    receive_discard(kIap2LinkHeaderBytes);
    if (!header_ok || header.length < kIap2LinkHeaderBytes) return true;
    if (header.length == kIap2LinkHeaderBytes) {
        process_frame(header, nullptr, 0, false, now_ms);
    } else {
        pending_header_ = header;
        awaiting_payload_ = true;
    }
    return true;
}

void Iap2Link::enter_negotiating(int64_t now_ms) {
    state_ = State::Negotiating;
    marker_deadline_ms_ = -1;
    send_synchronization();
    if (state_ != State::Dead) synchronization_deadline_ms_ = now_ms + kSynchronizationResendMs;
}

void Iap2Link::process_frame(const Header &header, const uint8_t *payload, size_t length, bool has_payload,
                             int64_t now_ms) {
    if (header.control & kControlReset) {
        die("peer sent iAP2 reset");
        return;
    }
    if (header.control & kControlSynchronize) {
        Iap2SyncPayload sync;
        if (Iap2SyncPayload::decode(payload, has_payload ? length : 0, sync))
            handle_synchronization(sync, header.sequence);
    }
    if (header.control & kControlAcknowledgement) {
        accumulated_acknowledgements_ += 1;
        handle_acknowledgement(header.acknowledgement, now_ms);
    }
    if ((header.control & kControlExtendedAcknowledgement) && has_payload)
        handle_extended_acknowledgement(payload, length);
    if ((header.control & static_cast<uint8_t>(~kControlAcknowledgement)) == 0 && has_payload) {
        Packet packet;
        packet.sequence = header.sequence;
        packet.session_id = header.session_id;
        if (!packet.payload.assign(payload, length)) {
            die("out of memory");
            return;
        }
        handle_data(std::move(packet), now_ms);
    }
    if (state_ != State::Dead && peer_sync_.max_acknowledgements > 0 &&
        accumulated_acknowledgements_ >= peer_sync_.max_acknowledgements) {
        accumulated_acknowledgements_ = 0;
        has_last_acknowledged_received_ = true;
        last_acknowledged_received_ = last_received_in_order_;
        send_acknowledgement();
    }
}

void Iap2Link::handle_synchronization(const Iap2SyncPayload &sync, uint8_t sequence) {
    if (state_ != State::Negotiating) return;
    peer_sync_ = sync;
    peer_sync_received_ = true;
    last_received_in_order_ = sequence;
    has_last_acknowledged_received_ = true;
    last_acknowledged_received_ = sequence;
    send_acknowledgement();
}

int Iap2Link::distance_from_sent_ack(uint8_t sequence) const {
    return has_last_sent_acknowledged_ ? sequence_distance(sequence, last_sent_acknowledged_) : 0;
}

void Iap2Link::handle_acknowledgement(uint8_t acknowledgement, int64_t now_ms) {
    if (state_ == State::Negotiating) {
        bool queued_fits = true;
        for (size_t i = 0; i < queued_.size(); ++i)
            if (!peer_payload_acceptable(queued_[i].payload.size())) queued_fits = false;
        if (!peer_sync_received_ || !peer_limits_usable() || !queued_fits) {
            die("peer iAP2 maxLength cannot carry pending control packets");
            return;
        }
        state_ = State::Normal;
        synchronization_deadline_ms_ = -1;
        set_writable(true);
    }
    has_last_sent_acknowledged_ = true;
    last_sent_acknowledged_ = acknowledgement;

    bool rearmed = false;
    while (!unacknowledged_.empty()) {
        Packet &first = unacknowledged_.front();
        int distance = distance_from_sent_ack(first.sequence);
        if (distance > 0 && distance <= peer_sync_.max_acknowledgements + kAckSlack) {
            receive_acknowledgement_deadline_ms_ = first.deadline_ms;
            rearmed = true;
            break;
        }
        unacknowledged_.take(0);
    }
    if (!rearmed) receive_acknowledgement_deadline_ms_ = -1;

    while (distance_from_sent_ack(sent_sequence_) < peer_sync_.max_outgoing && !queued_.empty()) {
        send_packet(queued_.take(0), now_ms);
        set_writable(true);
        if (state_ == State::Dead) return;
    }
}

void Iap2Link::handle_extended_acknowledgement(const uint8_t *missing, size_t count) {
    if (state_ != State::Normal) return;
    for (size_t i = 0; i < unacknowledged_.size(); ++i) {
        Packet &packet = unacknowledged_[i];
        bool listed = false;
        for (size_t m = 0; m < count; ++m)
            if (missing[m] == packet.sequence) listed = true;
        if (!listed) continue;
        packet.retransmissions += 1;
        if (packet.retransmissions == peer_sync_.max_retransmissions) {
            die("iAP2 packet was not acknowledged");
            return;
        }
        send_data(packet.sequence, packet.session_id, packet.payload);
        send_acknowledgement_deadline_ms_ = -1;
        receive_acknowledgement_deadline_ms_ = packet.deadline_ms;
    }
}

void Iap2Link::handle_data(Packet &&packet, int64_t now_ms) {
    int distance = sequence_distance(packet.sequence, last_received_in_order_);
    if (distance == 0 || distance > peer_sync_.max_outgoing + kAckSlack) {
        send_acknowledgement();
        return;
    }
    for (size_t i = 0; i < out_of_order_.size(); ++i)
        if (out_of_order_[i].sequence == packet.sequence) return;
    if (out_of_order_.size() >= config_.maximum_out_of_order_packets) {
        die("iAP2 out-of-order packet limit exceeded");
        return;
    }
    uint8_t sequence_received = packet.sequence;
    out_of_order_.push_back(std::move(packet));
    if (distance > 1) {
        if (distance >= peer_sync_.max_outgoing) {
            uint8_t missing[256];
            size_t count = 0;
            uint8_t sequence = last_received_in_order_;
            while (sequence_distance(sequence_received, sequence) > 1) {
                sequence = next_sequence(sequence);
                missing[count++] = sequence;
            }
            send_acknowledgement_deadline_ms_ = -1;
            send_extended_acknowledgement(missing, count);
        }
        return;
    }

    uint8_t base = last_received_in_order_;
    out_of_order_.sort([base](const Packet &a, const Packet &b) {
        return sequence_distance(a.sequence, base) < sequence_distance(b.sequence, base);
    });
    while (!out_of_order_.empty() &&
           sequence_distance(out_of_order_.front().sequence, last_received_in_order_) == 1) {
        Packet in_order = out_of_order_.take(0);
        last_received_in_order_ = in_order.sequence;
        Iap2Event event;
        event.type = in_order.session_id == kIap2ControlSessionId ? Iap2Event::Type::Control
                                                                   : Iap2Event::Type::Session;
        event.session_id = in_order.session_id;
        event.data = std::move(in_order.payload);
        enqueue_event(std::move(event));
        if (state_ == State::Dead) return;
    }

    if (peer_sync_.max_acknowledgements == 0) return;
    int window = peer_sync_.max_outgoing - config_.max_outgoing_delta;
    if (window < 1) window = 1;
    int since_ack = has_last_acknowledged_received_
                        ? sequence_distance(last_received_in_order_, last_acknowledged_received_)
                        : 0;
    if (since_ack >= window) {
        send_acknowledgement_deadline_ms_ = -1;
        has_last_acknowledged_received_ = true;
        last_acknowledged_received_ = last_received_in_order_;
        send_acknowledgement();
    } else {
        send_acknowledgement_deadline_ms_ = now_ms + peer_sync_.acknowledgement_timeout_ms;
    }
}

void Iap2Link::send_packet(Packet &&packet, int64_t now_ms) {
    if (state_ != State::Normal || distance_from_sent_ack(sent_sequence_) > peer_sync_.max_outgoing) {
        if (queued_.size() >= config_.maximum_queued_packets) {
            die("iAP2 outbound queue limit exceeded");
        } else {
            queued_.push_back(std::move(packet));
            set_writable(false);
        }
        return;
    }
    sent_sequence_ = next_sequence(sent_sequence_);
    packet.sequence = sent_sequence_;
    packet.retransmissions = 0;
    packet.deadline_ms = now_ms + peer_sync_.retransmission_timeout_ms;
    send_acknowledgement_deadline_ms_ = -1;
    send_data(packet.sequence, packet.session_id, packet.payload);
    has_last_acknowledged_received_ = true;
    last_acknowledged_received_ = last_received_in_order_;
    if (peer_sync_.max_retransmissions > 0) {
        int64_t deadline = packet.deadline_ms;
        if (!unacknowledged_.push_back(std::move(packet))) {
            die("iAP2 unacknowledged packet limit exceeded");
            return;
        }
        receive_acknowledgement_deadline_ms_ = deadline;
    } else {
        has_last_sent_acknowledged_ = true;
        last_sent_acknowledged_ = sent_sequence_;
    }
}

void Iap2Link::retransmit_due_packet(int64_t now_ms) {
    if (state_ != State::Normal || unacknowledged_.empty()) return;
    size_t next = 0;
    for (size_t i = 1; i < unacknowledged_.size(); ++i)
        if (unacknowledged_[i].deadline_ms < unacknowledged_[next].deadline_ms) next = i;
    int64_t second = -1;
    for (size_t i = 0; i < unacknowledged_.size(); ++i)
        if (i != next) arm_min(second, unacknowledged_[i].deadline_ms);
    Packet &packet = unacknowledged_[next];
    packet.deadline_ms = now_ms + peer_sync_.retransmission_timeout_ms;
    packet.retransmissions += 1;
    if (packet.retransmissions == peer_sync_.max_retransmissions) {
        die("iAP2 packet was not acknowledged");
        return;
    }
    send_data(packet.sequence, packet.session_id, packet.payload);
    receive_acknowledgement_deadline_ms_ = second >= 0 ? second : packet.deadline_ms;
}

void Iap2Link::send_synchronization() {
    Bytes encoded;
    if (!local_sync().encode(encoded)) {
        die("out of memory");
        return;
    }
    write_packet(encoded.data(), encoded.size(), true, sent_sequence_, kControlSynchronize, 0);
}

void Iap2Link::send_acknowledgement() { write_packet(nullptr, 0, false, sent_sequence_, kControlAcknowledgement, 0); }

void Iap2Link::send_extended_acknowledgement(const uint8_t *missing, size_t count) {
    write_packet(missing, count, true, sent_sequence_, kControlExtendedAcknowledgement, 0);
}

void Iap2Link::send_data(uint8_t sequence, uint8_t session_id, const Bytes &payload) {
    write_packet(payload.data(), payload.size(), true, sequence, kControlAcknowledgement, session_id);
}

void Iap2Link::write_packet(const uint8_t *payload, size_t length, bool has_payload, uint8_t sequence,
                            uint8_t control, uint8_t session_id) {
    if (state_ == State::Dead) return;
    accumulated_acknowledgements_ = 0;
    size_t total = has_payload ? length + kIap2LinkHeaderBytes + kIap2LinkChecksumBytes : kIap2LinkHeaderBytes;
    uint8_t header[kIap2LinkHeaderBytes] = {
        kLinkStartHigh,
        kLinkStartLow,
        static_cast<uint8_t>(total >> 8),
        static_cast<uint8_t>(total),
        control,
        sequence,
        last_received_in_order_,
        session_id,
        0,
    };
    header[8] = checksum(header, kIap2LinkHeaderBytes - kIap2LinkChecksumBytes);
    if (!append_output(header, sizeof header) || !has_payload) return;
    uint8_t sum = checksum(payload, length);
    if (!append_output(payload, length)) return;
    append_output(&sum, 1);
}

void Iap2Link::set_writable(bool value) {
    if (writable_ == value) return;
    writable_ = value;
    Iap2Event event;
    event.type = Iap2Event::Type::Writable;
    event.writable = value;
    enqueue_event(std::move(event));
}

void Iap2Link::die(const char *reason) {
    if (state_ == State::Dead) return;
    marker_deadline_ms_ = synchronization_deadline_ms_ = -1;
    send_acknowledgement_deadline_ms_ = receive_acknowledgement_deadline_ms_ = -1;
    state_ = State::Dead;
    writable_ = false;
    output_.clear();
    events_.clear();
    Iap2Event event;
    event.type = Iap2Event::Type::Dead;
    event.reason = reason;
    events_.push_back(std::move(event));
}

bool Iap2Link::append_output(const uint8_t *bytes, size_t length) {
    if (state_ == State::Dead) return false;
    if (length > config_.maximum_pending_output_bytes - output_.size()) {
        die("iAP2 pending output limit exceeded");
        return false;
    }
    if (!output_.append(bytes, length)) {
        die("out of memory");
        return false;
    }
    return true;
}

void Iap2Link::enqueue_event(Iap2Event &&event) {
    if (state_ == State::Dead) return;
    if (events_.size() >= config_.maximum_pending_events) {
        die("iAP2 pending event limit exceeded");
        return;
    }
    events_.push_back(std::move(event));
}

bool Iap2Link::peer_limits_usable() const {
    return peer_sync_.max_length > kIap2LinkHeaderBytes + kIap2LinkChecksumBytes;
}

bool Iap2Link::peer_payload_acceptable(size_t length) const {
    return peer_limits_usable() && length <= peer_sync_.max_length - kIap2LinkHeaderBytes - kIap2LinkChecksumBytes;
}

Iap2SyncPayload Iap2Link::local_sync() const {
    Iap2SyncPayload sync;
    bool zero = config_.zero_acknowledgements;
    sync.max_outgoing = config_.max_outgoing;
    sync.max_length = kIap2MaxWireFrameBytes;
    sync.retransmission_timeout_ms = zero ? 0 : kDefaultRetransmissionTimeoutMs;
    sync.acknowledgement_timeout_ms = zero ? 0 : config_.acknowledgement_timeout_ms;
    sync.max_retransmissions = zero ? 0 : kDefaultMaxRetransmissions;
    sync.max_acknowledgements = zero ? 0 : kDefaultMaxAcknowledgements;
    sync.session_count = 3;
    sync.sessions[0] = {kIap2ControlSessionId, 0, config_.control_session_version};
    sync.sessions[1] = {kIap2ExternalAccessorySessionId, 2, 1};
    sync.sessions[2] = {kIap2FileTransferSessionId, 1, 2};
    return sync;
}

size_t Iap2Link::receive_room() {
    if (receive_head_ != 0) {
        size_t size = receive_size();
        if (size) memmove(receive_, receive_ + receive_head_, size);
        receive_head_ = 0;
        receive_tail_ = size;
    }
    return sizeof receive_ - receive_tail_;
}

void Iap2Link::receive_discard(size_t count) {
    receive_head_ += count;
    if (receive_head_ == receive_tail_) receive_head_ = receive_tail_ = 0;
}

} // namespace carplay::core
