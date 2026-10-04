#include "android_auto/aa_direct_messages.h"

namespace android_auto {

uint64_t readBe64(const uint8_t* data)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value = (value << 8) | data[index];
    return value;
}

uint32_t tlsPayloadHash(const std::vector<uint8_t>& data)
{
    uint32_t hash = 2166136261u;
    for (const uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

TlsRecordInspection inspectTlsRecords(const std::vector<uint8_t>& data)
{
    TlsRecordInspection result;
    size_t offset = 0;

    while (offset < data.size()) {
        if (data.size() - offset < 5)
            return result;

        const uint8_t contentType = data[offset];
        const uint16_t version =
            (static_cast<uint16_t>(data[offset + 1]) << 8) |
            static_cast<uint16_t>(data[offset + 2]);
        const size_t recordLength =
            (static_cast<size_t>(data[offset + 3]) << 8) |
            static_cast<size_t>(data[offset + 4]);
        if (contentType < 0x14 || contentType > 0x17 ||
            version != 0x0303 || recordLength < 8 ||
            recordLength > data.size() - offset - 5)
        {
            return result;
        }

        const uint64_t sequence = readBe64(data.data() + offset + 5);
        if (!result.hasSequence) {
            result.hasSequence = true;
            result.firstSequence = sequence;
        } else if (sequence != result.lastSequence + 1) {
            result.internallyContiguous = false;
        }
        result.lastSequence = sequence;
        ++result.recordCount;
        offset += 5 + recordLength;
    }

    result.layoutValid = offset == data.size() && result.recordCount != 0;
    return result;
}

void writeBe64(uint8_t* out, uint64_t value)
{
    constexpr unsigned WIDTH = 8;
    for (unsigned index = 0; index < WIDTH; ++index) {
        out[index] = static_cast<uint8_t>(
            (value >> ((WIDTH - 1u - index) * 8u)) & 0xffu
        );
    }
}

void writeVarint(std::vector<uint8_t>& out, uint64_t value)
{
    writeVarintBytes(value, [&out](uint8_t byte) {
        out.push_back(byte);
        return true;
    });
}

void writeVarintField(std::vector<uint8_t>& out, uint8_t field, uint64_t value)
{
    out.push_back(static_cast<uint8_t>(field << 3));
    writeVarint(out, value);
}

void writeBytesField(
    std::vector<uint8_t>& out,
    uint8_t field,
    const std::vector<uint8_t>& value
) {
    out.push_back(static_cast<uint8_t>((field << 3) | 2));
    writeVarint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

uint32_t readFirstVarintField(
    const uint8_t* data,
    size_t length,
    uint8_t field,
    uint32_t fallback
) {
    size_t offset = 0;
    auto readVarint = [&](uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (offset >= length)
                return false;
            const uint8_t byte = data[offset++];
            if (shift == 63 && byte > 1)
                return false;
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0)
                return true;
        }
        return false;
    };
    if (data == nullptr)
        return fallback;
    while (offset < length) {
        uint64_t key = 0;
        if (!readVarint(key) || key > UINT32_MAX || (key >> 3) == 0)
            return fallback;
        const uint32_t gotField = static_cast<uint32_t>(key >> 3);
        const uint8_t wireType = static_cast<uint8_t>(key & 0x07);
        if (wireType != 0)
            return fallback;

        uint64_t value = 0;
        if (!readVarint(value))
            return fallback;

        if (gotField == field)
            return value <= UINT32_MAX ? static_cast<uint32_t>(value) : fallback;
    }
    return fallback;
}

std::vector<uint8_t> statusOk()
{
    std::vector<uint8_t> out;
    writeVarintField(out, 1, 0); // Field 1: status OK.
    return out;
}

std::vector<uint8_t> avSetupOk()
{
    std::vector<uint8_t> out;
    writeVarintField(out, 1, 2); // Config::STATUS_READY / head-unit sink.
    writeVarintField(out, 2, 1); // Allow one unacknowledged media frame.
    writeVarintField(out, 3, 0); // Accept advertised configuration index 0.
    return out;
}

std::vector<uint8_t> videoFocus(bool unsolicited)
{
    std::vector<uint8_t> out;
    writeVarintField(out, 1, 1); // Field 1: focused/active.
    writeVarintField(out, 2, unsolicited ? 1 : 0); // Field 2: unsolicited indication flag.
    return out;
}

std::vector<uint8_t> audioFocus(const std::vector<uint8_t>& request)
{
    const uint32_t requestType = readFirstVarintField(
        request.data(),
        request.size(),
        1, // Field 1: requested focus type.
        0 // Default to normal focus handling when field is absent.
    );
    const uint32_t focusState = requestType == 4 // Type 4 maps to transient/loss focus state.
        ? 3
        : (requestType == 2 || requestType == 3 ? 2 : 1); // Types 2/3 map to duck/transient; fallback is gain.
    std::vector<uint8_t> out;
    writeVarintField(out, 1, focusState); // Field 1: granted focus state.
    return out;
}

std::vector<uint8_t> sensorEvent(uint32_t sensorType)
{
    std::vector<uint8_t> inner;
    writeVarintField(inner, 1, sensorType == 7 ? 1 : 0); // Field 1: basic sensor value.

    std::vector<uint8_t> out;
    if (sensorType == 13) // Sensor 13: day/night sensor.
        writeBytesField(out, 13, inner); // Field 13: matching payload.
    else if (sensorType == 10) // Sensor 10: driving/status sensor.
        writeBytesField(out, 10, inner); // Field 10: matching sensor payload.
    else if (sensorType == 7) // Sensor 7: parking-brake sensor.
        writeBytesField(out, 7, inner); // Field 7: matching sensor payload.
    return out;
}

} // namespace android_auto
