// Ported from DiPlay's Iap2CsmFramer / Iap2Wire
#pragma once

#include "rt.h"

namespace carplay::core {

constexpr uint16_t kCsmStart = 0x4040;
constexpr size_t kCsmHeaderBytes = 6;
constexpr size_t kCsmParamHeaderBytes = 4;
constexpr size_t kCsmMaxFrameBytes = 0xffff;

class CsmGroup {
public:
    bool add(uint16_t id, const uint8_t *payload, size_t length);
    bool add_void(uint16_t id) { return add(id, nullptr, 0); }
    bool add_u8(uint16_t id, uint8_t value) { return add(id, &value, 1); }
    bool add_u16(uint16_t id, uint16_t value);
    bool add_u32(uint16_t id, uint32_t value);
    bool add_string(uint16_t id, const char *text); 
    bool add_u16_list(uint16_t id, const uint16_t *values, size_t count);
    bool add_group(uint16_t id, const CsmGroup &group);

    bool ok() const { return ok_; }
    const Bytes &bytes() const { return bytes_; }

private:
    Bytes bytes_;
    bool ok_ = true;
};

class CsmBuilder : public CsmGroup {
public:
    explicit CsmBuilder(uint16_t message_id) : message_id_(message_id) {}

    bool finish(Bytes &frame) const;

private:
    uint16_t message_id_;
};

struct CsmParam {
    uint16_t id;
    const uint8_t *payload;
    size_t length;
};

class CsmFrame {
public:
    uint16_t message_id = 0;
    Bytes body;

    bool next_param(size_t &cursor, CsmParam &param) const;
    bool find(uint16_t id, CsmParam &param) const;
};

class CsmParser {
public:
    bool offer(const uint8_t *bytes, size_t length);
    bool next(CsmFrame &frame);

private:
    Bytes buffer_;
    size_t head_ = 0;
};

} // namespace carplay::core
