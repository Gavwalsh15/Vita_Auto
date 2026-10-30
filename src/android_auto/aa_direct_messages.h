#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace android_auto {

struct TlsRecordInspection {
    bool layoutValid = false;
    bool hasSequence = false;
    bool internallyContiguous = true;
    uint32_t recordCount = 0;
    uint64_t firstSequence = 0;
    uint64_t lastSequence = 0;
};

uint32_t tlsPayloadHash(const std::vector<uint8_t>& data);
uint64_t readBe64(const uint8_t* data);
TlsRecordInspection inspectTlsRecords(const uint8_t* data, size_t length);
TlsRecordInspection inspectTlsRecords(const std::vector<uint8_t>& data);

void writeBe64(uint8_t* out, uint64_t value);

template <typename ByteWriter>
bool writeVarintBytes(uint64_t value, ByteWriter writeByte)
{
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        if (!writeByte(byte))
            return false;
    } while (value != 0);
    return true;
}

void writeVarint(std::vector<uint8_t>& out, uint64_t value);
void writeVarintField(std::vector<uint8_t>& out, uint8_t field, uint64_t value);
void writeBytesField(
    std::vector<uint8_t>& out,
    uint8_t field,
    const std::vector<uint8_t>& value
);

template <size_t Capacity>
class FixedWriter {
public:
    bool byte(uint8_t value)
    {
        if (size_ >= Capacity)
            return false;
        data_[size_++] = value;
        return true;
    }

    bool varint(uint64_t value)
    {
        return writeVarintBytes(value, [this](uint8_t part) {
            return byte(part);
        });
    }

    bool varintField(uint8_t field, uint64_t value)
    {
        return byte(static_cast<uint8_t>(field << 3)) && varint(value);
    }

    bool bytesField(
        uint8_t field,
        const uint8_t* value,
        size_t length
    ) {
        if ((value == nullptr && length != 0) ||
            !byte(static_cast<uint8_t>((field << 3) | 2)) ||
            !varint(length) || size_ + length > Capacity)
        {
            return false;
        }
        if (length != 0)
            std::memcpy(data_ + size_, value, length);
        size_ += length;
        return true;
    }

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    uint8_t data_[Capacity]{};
    size_t size_ = 0;
};

uint32_t readFirstVarintField(
    const uint8_t* data,
    size_t length,
    uint8_t field,
    uint32_t fallback = 0
);

std::vector<uint8_t> statusOk();
std::vector<uint8_t> avSetupOk();
std::vector<uint8_t> videoFocus(bool unsolicited = false);
std::vector<uint8_t> audioFocus(const std::vector<uint8_t>& request);
std::vector<uint8_t> sensorEvent(uint32_t sensorType);

} // namespace android_auto
