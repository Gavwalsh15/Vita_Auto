#include "iap2_csm.h"

namespace carplay::core {

namespace {

uint16_t read_u16(const uint8_t *bytes) { return static_cast<uint16_t>((bytes[0] << 8) | bytes[1]); }

void put_u16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

} // namespace

bool CsmGroup::add(uint16_t id, const uint8_t *payload, size_t length) {
    if (!ok_) return false;
    if (bytes_.size() + kCsmParamHeaderBytes + length > kCsmMaxFrameBytes - kCsmHeaderBytes) return ok_ = false;
    uint8_t header[kCsmParamHeaderBytes];
    put_u16(header, static_cast<uint16_t>(kCsmParamHeaderBytes + length));
    put_u16(header + 2, id);
    return ok_ = bytes_.append(header, sizeof header) && bytes_.append(payload, length);
}

bool CsmGroup::add_u16(uint16_t id, uint16_t value) {
    uint8_t bytes[2];
    put_u16(bytes, value);
    return add(id, bytes, sizeof bytes);
}

bool CsmGroup::add_u32(uint16_t id, uint32_t value) {
    uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    return add(id, bytes, sizeof bytes);
}

bool CsmGroup::add_string(uint16_t id, const char *text) {
    return add(id, reinterpret_cast<const uint8_t *>(text), strlen(text) + 1);
}

bool CsmGroup::add_u16_list(uint16_t id, const uint16_t *values, size_t count) {
    Bytes payload;
    for (size_t i = 0; i < count; ++i) {
        uint8_t be[2];
        put_u16(be, values[i]);
        if (!payload.append(be, sizeof be)) return ok_ = false;
    }
    return add(id, payload.data(), payload.size());
}

bool CsmGroup::add_group(uint16_t id, const CsmGroup &group) {
    if (!group.ok()) return ok_ = false;
    return add(id, group.bytes().data(), group.bytes().size());
}

bool CsmBuilder::finish(Bytes &frame) const {
    if (!ok()) return false;
    uint8_t header[kCsmHeaderBytes];
    put_u16(header, kCsmStart);
    put_u16(header + 2, static_cast<uint16_t>(kCsmHeaderBytes + bytes().size()));
    put_u16(header + 4, message_id_);
    Bytes out;
    if (!out.append(header, sizeof header) || !out.append(bytes().data(), bytes().size())) return false;
    frame = std::move(out);
    return true;
}

bool CsmFrame::next_param(size_t &cursor, CsmParam &param) const {
    if (cursor + kCsmParamHeaderBytes > body.size()) return false;
    uint16_t length = read_u16(body.data() + cursor);
    if (length < kCsmParamHeaderBytes || length > body.size() - cursor) return false;
    param.id = read_u16(body.data() + cursor + 2);
    param.payload = body.data() + cursor + kCsmParamHeaderBytes;
    param.length = length - kCsmParamHeaderBytes;
    cursor += length;
    return true;
}

bool CsmFrame::find(uint16_t id, CsmParam &param) const {
    size_t cursor = 0;
    CsmParam candidate;
    while (next_param(cursor, candidate)) {
        if (candidate.id == id) {
            param = candidate;
            return true;
        }
    }
    return false;
}

bool CsmParser::offer(const uint8_t *bytes, size_t length) {
    if (head_ > 0 && head_ == buffer_.size()) {
        buffer_.clear();
        head_ = 0;
    }
    return buffer_.append(bytes, length);
}

bool CsmParser::next(CsmFrame &frame) {
    for (;;) {
        while (buffer_.size() - head_ >= 2 && !(buffer_[head_] == 0x40 && buffer_[head_ + 1] == 0x40)) ++head_;
        size_t available = buffer_.size() - head_;
        if (available < kCsmHeaderBytes) break;
        const uint8_t *start = buffer_.data() + head_;
        uint16_t length = read_u16(start + 2);
        if (length < kCsmHeaderBytes) {
            ++head_;
            continue;
        }
        if (available < length) break;
        CsmFrame complete;
        complete.message_id = read_u16(start + 4);
        if (!complete.body.assign(start + kCsmHeaderBytes, length - kCsmHeaderBytes)) return false;
        head_ += length;
        frame = std::move(complete);
        return true;
    }
    if (head_ > 0) {
        size_t remaining = buffer_.size() - head_;
        Bytes rest;
        if (rest.assign(buffer_.data() + head_, remaining)) {
            buffer_ = std::move(rest);
            head_ = 0;
        }
    }
    return false;
}

} // namespace carplay::core
