// Ported from DiPlay's ScreenStream/ScreenCodec (GPL-3.0).
#pragma once

#include "airplay.h"
#include "crypto.h"

namespace carplay::core {

constexpr size_t kScreenHeaderBytes = 128;

void screen_stream_key(const uint8_t shared_secret[32], uint64_t connection_id, uint8_t key[32]);

class ScreenStreamReceiver {
public:
    explicit ScreenStreamReceiver(MediaHost &sink) : sink_(sink) {}
    ~ScreenStreamReceiver();
    void reset(const uint8_t key[32]);
    bool feed(const uint8_t *data, size_t length);
    uint32_t frames() const { return frames_; }
    uint32_t configs() const { return configs_; }

private:
    bool message(const uint8_t *header, const uint8_t *body, size_t length);

    uint8_t key_[32] = {};
    MediaHost &sink_;
    Bytes pending_;
    uint64_t counter_ = 0;
    uint32_t frames_ = 0, configs_ = 0;
};

void length_prefixed_to_annex_b(uint8_t *data, size_t length);

bool screen_config_record(const uint8_t *body, size_t length, VideoCodec &codec, size_t &offset);

bool avcc_to_annex_b(const uint8_t *record, size_t length, Bytes &out);

} // namespace carplay::core
