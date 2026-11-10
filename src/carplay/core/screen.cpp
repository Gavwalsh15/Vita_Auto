#include "screen.h"

namespace carplay::core {

namespace {

const uint8_t kOpFrame = 0, kOpConfig = 1;
const size_t kMaxBody = 8 * 1024 * 1024;
const size_t kSenderTimeOffset = 8;

uint32_t le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

uint32_t be32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           static_cast<uint32_t>(p[3]);
}

// Unsigned decimal without 64-bit division (no compiler runtime helper needed).
size_t decimal(uint64_t value, char *out) {
    static const uint64_t kPowers[] = {10000000000000000000ull, 1000000000000000000ull, 100000000000000000ull,
                                       10000000000000000ull, 1000000000000000ull, 100000000000000ull,
                                       10000000000000ull, 1000000000000ull, 100000000000ull, 10000000000ull,
                                       1000000000ull, 100000000ull, 10000000ull, 1000000ull, 100000ull, 10000ull,
                                       1000ull, 100ull, 10ull, 1ull};
    size_t count = 0;
    for (uint64_t power : kPowers) {
        char digit = '0';
        while (value >= power) {
            value -= power;
            ++digit;
        }
        if (digit != '0' || count || power == 1) out[count++] = digit;
    }
    out[count] = 0;
    return count;
}

// NTP 32.32 seconds to nanoseconds: whole seconds * 1e9 + fraction * 1e9 / 2^32.
uint64_t ntp_to_nanos(uint64_t ntp) {
    uint64_t seconds = ntp >> 32, fraction = ntp & 0xffffffffu;
    return seconds * 1000000000ull + ((fraction * 1000000000ull) >> 32);
}

void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

} // namespace

void screen_stream_key(const uint8_t shared_secret[32], uint64_t connection_id, uint8_t key[32]) {
    char salt[16 + 21] = "DataStream-Salt";
    decimal(connection_id, salt + 15);
    hkdf_sha512(shared_secret, 32, salt, "DataStream-Output-Encryption-Key", key, 32);
}

void length_prefixed_to_annex_b(uint8_t *data, size_t length) {
    if (length < 4 || (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)) return;
    size_t at = 0;
    while (at + 4 <= length) {
        uint32_t nal = be32(data + at);
        at += 4;
        if (nal == 0 || nal > length - at) return;
        at += nal;
    }
    if (at != length) return;
    for (at = 0; at + 4 <= length;) {
        uint32_t nal = be32(data + at);
        data[at] = data[at + 1] = data[at + 2] = 0;
        data[at + 3] = 1;
        at += 4 + nal;
    }
}

bool screen_config_record(const uint8_t *body, size_t length, VideoCodec &codec, size_t &offset) {
    for (size_t i = 4; i + 4 <= length; ++i) {
        if (memcmp(body + i, "avcC", 4) == 0 || memcmp(body + i, "hvcC", 4) == 0) {
            codec = body[i] == 'a' ? VideoCodec::H264 : VideoCodec::H265;
            offset = i + 4;
            return true;
        }
    }
    // A bare avcC record: version 1, then the SPS count and an SPS NAL (type 7).
    if (length >= 9 && body[0] == 1 && (body[5] & 0x1f) >= 1 && 8u + (body[6] << 8 | body[7]) <= length &&
        (body[8] & 0x1f) == 7) {
        codec = VideoCodec::H264;
        offset = 0;
        return true;
    }
    return false;
}

bool avcc_to_annex_b(const uint8_t *record, size_t length, Bytes &out) {
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    if (length < 6 || record[0] != 1) return false;
    size_t at = 5;
    // SPS list (count in the low 5 bits), then the PPS list (count byte).
    for (int list = 0; list < 2; ++list) {
        if (at >= length) return false;
        unsigned count = list == 0 ? record[at] & 0x1f : record[at];
        ++at;
        for (unsigned i = 0; i < count; ++i) {
            if (length - at < 2) return false;
            size_t nal = static_cast<size_t>(record[at]) << 8 | record[at + 1];
            at += 2;
            if (length - at < nal || !out.append(kStart, 4) || !out.append(record + at, nal)) return false;
            at += nal;
        }
    }
    return true;
}

ScreenStreamReceiver::~ScreenStreamReceiver() { memset(key_, 0, sizeof key_); }

void ScreenStreamReceiver::reset(const uint8_t key[32]) {
    memcpy(key_, key, sizeof key_);
    pending_.clear();
    counter_ = 0;
}

bool ScreenStreamReceiver::feed(const uint8_t *data, size_t length) {
    if (!pending_.append(data, length)) return false;
    size_t at = 0;
    while (pending_.size() - at >= kScreenHeaderBytes) {
        const uint8_t *header = pending_.data() + at;
        size_t body = le32(header);
        if (body > kMaxBody) return false;
        if (pending_.size() - at - kScreenHeaderBytes < body) break;
        if (!message(header, header + kScreenHeaderBytes, body)) return false;
        at += kScreenHeaderBytes + body;
    }
    Bytes rest;
    if (!rest.assign(pending_.data() + at, pending_.size() - at)) return false;
    pending_ = std::move(rest);
    return true;
}

bool ScreenStreamReceiver::message(const uint8_t *header, const uint8_t *body, size_t length) {
    if (header[4] == kOpConfig) {
        VideoCodec codec;
        size_t offset;
        if (!screen_config_record(body, length, codec, offset)) return true;
        ++configs_;
        sink_.on_config(codec, body + offset, length - offset);
        return true;
    }
    if (header[4] != kOpFrame) return true;
    if (length < kAeadTagBytes) return true;
    uint8_t nonce[12];
    nonce_counter(counter_++, nonce);
    Bytes plain;
    if (!aead_open(key_, nonce, header, kScreenHeaderBytes, body, length, plain)) return false;
    length_prefixed_to_annex_b(plain.data(), plain.size());
    uint64_t ntp = 0;
    for (int i = 7; i >= 0; --i) ntp = ntp << 8 | header[kSenderTimeOffset + i];
    ++frames_;
    sink_.on_frame(plain.data(), plain.size(), ntp_to_nanos(ntp));
    return true;
}

} // namespace carplay::core
