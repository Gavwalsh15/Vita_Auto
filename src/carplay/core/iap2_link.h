// Ported from DiPlay's Iap2LinkEngine (GPL-3.0). FF 5A packets, SYN/ACK/EAK.
#pragma once

#include "rt.h"

namespace carplay::core {

constexpr uint8_t kIap2ControlSessionId = 10;
constexpr uint8_t kIap2ExternalAccessorySessionId = 11;
constexpr uint8_t kIap2FileTransferSessionId = 12;

constexpr size_t kIap2LinkHeaderBytes = 9;
constexpr size_t kIap2LinkChecksumBytes = 1;
constexpr size_t kIap2MaxWireFrameBytes = 0xffff;
constexpr size_t kIap2MaxPayloadBytes = kIap2MaxWireFrameBytes - kIap2LinkHeaderBytes - kIap2LinkChecksumBytes;
constexpr size_t kIap2MaxSessions = 8;
constexpr size_t kIap2MaxPackets = 256;

extern const uint8_t kIap2Marker[6];

struct Iap2SessionDescriptor {
    uint8_t id;
    uint8_t kind;
    uint8_t version;
};

struct Iap2SyncPayload {
    uint8_t max_outgoing = 0;
    uint16_t max_length = 0;
    uint16_t retransmission_timeout_ms = 0;
    uint16_t acknowledgement_timeout_ms = 0;
    uint8_t max_retransmissions = 0;
    uint8_t max_acknowledgements = 0;
    uint8_t session_count = 0;
    Iap2SessionDescriptor sessions[kIap2MaxSessions] = {};

    bool encode(Bytes &out) const;
    static bool decode(const uint8_t *bytes, size_t length, Iap2SyncPayload &sync);
};

struct Iap2LinkConfig {
    uint8_t max_outgoing = 30;
    uint8_t max_outgoing_delta = 0;
    uint16_t acknowledgement_timeout_ms = 500;
    bool zero_acknowledgements = false;
    uint8_t control_session_version = 1;
    uint16_t maximum_queued_packets = 64;
    uint16_t maximum_out_of_order_packets = 64;
    uint32_t maximum_pending_output_bytes = 1048576;
    uint16_t maximum_pending_events = 256;

    bool valid() const;
};

struct Iap2Event {
    enum class Type { None, Control, Session, Writable, Dead };
    Type type = Type::None;
    uint8_t session_id = 0;
    Bytes data;
    bool writable = false;
    const char *reason = nullptr;
};

class Iap2Link {
public:
    enum class State { Idle, Detecting, Negotiating, Normal, Dead };

    static Iap2Link *create(const Iap2LinkConfig &config = Iap2LinkConfig());
    static void destroy(Iap2Link *link);

    State state() const { return state_; }
    bool writable() const { return writable_; }

    void start(bool wired_initiator, int64_t now_ms);
    void feed(const uint8_t *bytes, size_t length, int64_t now_ms);
    void advance_time(int64_t now_ms);
    int64_t next_deadline() const;

    bool send_control(const uint8_t *bytes, size_t length, int64_t now_ms);
    bool send_session(uint8_t session_id, const uint8_t *bytes, size_t length, int64_t now_ms);

    const uint8_t *output(size_t &length) const {
        length = output_.size();
        return output_.data();
    }
    void consume_output() { output_.clear(); }

    bool poll_event(Iap2Event &event);

private:
    struct Header {
        uint16_t length;
        uint8_t control;
        uint8_t sequence;
        uint8_t acknowledgement;
        uint8_t session_id;
    };
    struct Packet {
        uint8_t sequence = 0;
        uint8_t session_id = 0;
        Bytes payload;
        int retransmissions = 0;
        int64_t deadline_ms = 0;
    };

    explicit Iap2Link(const Iap2LinkConfig &config);

    bool parse_available(int64_t now_ms);
    void enter_negotiating(int64_t now_ms);
    void process_frame(const Header &header, const uint8_t *payload, size_t length, bool has_payload,
                       int64_t now_ms);
    void handle_synchronization(const Iap2SyncPayload &sync, uint8_t sequence);
    void handle_acknowledgement(uint8_t acknowledgement, int64_t now_ms);
    void handle_extended_acknowledgement(const uint8_t *missing, size_t count);
    void handle_data(Packet &&packet, int64_t now_ms);
    void send_packet(Packet &&packet, int64_t now_ms);
    void retransmit_due_packet(int64_t now_ms);
    void send_synchronization();
    void send_acknowledgement();
    void send_extended_acknowledgement(const uint8_t *missing, size_t count);
    void send_data(uint8_t sequence, uint8_t session_id, const Bytes &payload);
    void write_packet(const uint8_t *payload, size_t length, bool has_payload, uint8_t sequence,
                      uint8_t control, uint8_t session_id);
    void set_writable(bool value);
    void die(const char *reason);
    bool append_output(const uint8_t *bytes, size_t length);
    void enqueue_event(Iap2Event &&event);
    bool peer_limits_usable() const;
    bool peer_payload_acceptable(size_t length) const;
    Iap2SyncPayload local_sync() const;
    int distance_from_sent_ack(uint8_t sequence) const;

    size_t receive_size() const { return receive_tail_ - receive_head_; }
    size_t receive_room();
    void receive_discard(size_t count);

    Iap2LinkConfig config_;
    State state_ = State::Idle;
    Iap2SyncPayload peer_sync_;
    bool peer_sync_received_ = false;
    uint8_t sent_sequence_;
    bool has_last_sent_acknowledged_ = false;
    uint8_t last_sent_acknowledged_ = 0;
    uint8_t last_received_in_order_ = 0;
    bool has_last_acknowledged_received_ = false;
    uint8_t last_acknowledged_received_ = 0;
    int accumulated_acknowledgements_ = 0;
    bool writable_ = false;

    FixedList<Packet, kIap2MaxPackets> unacknowledged_;
    FixedList<Packet, kIap2MaxPackets> queued_;
    FixedList<Packet, kIap2MaxPackets> out_of_order_;
    FixedList<Iap2Event, kIap2MaxPackets> events_;
    Bytes output_;

    bool awaiting_payload_ = false;
    Header pending_header_ = {};

    int64_t marker_deadline_ms_ = -1;
    int64_t synchronization_deadline_ms_ = -1;
    int64_t send_acknowledgement_deadline_ms_ = -1;
    int64_t receive_acknowledgement_deadline_ms_ = -1;

    size_t receive_head_ = 0;
    size_t receive_tail_ = 0;
    uint8_t receive_[kIap2MaxWireFrameBytes];
};

} // namespace carplay::core
