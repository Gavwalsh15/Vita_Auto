#include "video/h264_annexb.h"

#include <algorithm>

namespace video {
namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size)
    {
    }

    uint32_t readBits(uint32_t count)
    {
        if (failed_ || count > 32) {
            failed_ = true;
            return 0;
        }
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (bitsRemaining_ == 0) {
                if (!nextByte())
                    return value;
            }
            value = (value << 1) |
                ((currentByte_ >> (bitsRemaining_ - 1)) & 1u);
            --bitsRemaining_;
        }
        return value;
    }

    bool readBool()
    {
        return readBits(1) != 0;
    }

    uint32_t readUe()
    {
        uint32_t zeros = 0;
        while (!failed_ && readBits(1) == 0) {
            if (++zeros >= 32) {
                failed_ = true;
                return 0;
            }
        }
        if (failed_ || zeros == 0)
            return 0;
        return ((1u << zeros) - 1u) + readBits(zeros);
    }

    int32_t readSe()
    {
        const uint32_t value = readUe();
        const uint64_t magnitude = (static_cast<uint64_t>(value) + 1) / 2;
        if (magnitude > INT32_MAX) {
            failed_ = true;
            return 0;
        }
        return (value & 1) ? static_cast<int32_t>(magnitude)
                           : -static_cast<int32_t>(magnitude);
    }

    bool failed() const
    {
        return failed_;
    }


private:
    bool nextByte()
    {
        while (offset_ < size_) {
            const uint8_t value = data_[offset_++];
            if (zeroCount_ >= 2 && value == 0x03) {
                zeroCount_ = 0;
                continue;
            }
            zeroCount_ = value == 0 ? zeroCount_ + 1 : 0;
            currentByte_ = value;
            bitsRemaining_ = 8;
            return true;
        }
        failed_ = true;
        return false;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
    uint32_t zeroCount_ = 0;
    uint8_t currentByte_ = 0;
    uint8_t bitsRemaining_ = 0;
    bool failed_ = false;
};

size_t startCodeSizeAt(const uint8_t* data, size_t size, size_t offset)
{
    if (offset + 3 <= size &&
        data[offset] == 0 &&
        data[offset + 1] == 0 &&
        data[offset + 2] == 1)
        return 3;

    if (offset + 4 <= size &&
        data[offset] == 0 &&
        data[offset + 1] == 0 &&
        data[offset + 2] == 0 &&
        data[offset + 3] == 1)
        return 4;

    return 0;
}

struct SpsInfo {
    uint32_t width = 0, height = 0;
    uint32_t profile = 0, pictureOrderType = 0;
};

bool parseSps(const uint8_t* nal, size_t size, SpsInfo& sps);

size_t findStartCode(
    const uint8_t* data,
    size_t size,
    size_t from,
    size_t& codeSize
)
{
    for (size_t offset = from; offset + 3 <= size; ++offset) {
        codeSize = startCodeSizeAt(data, size, offset);
        if (codeSize != 0)
            return offset;
    }
    codeSize = 0;
    return size;
}

void inspectNal(
    const uint8_t* payload,
    size_t payloadSize,
    H264AccessUnitInfo& info,
    uint32_t& activeWidth,
    uint32_t& activeHeight
)
{
    if (payloadSize == 0)
        return;

    ++info.nalCount;
    const uint8_t nalType = payload[0] & 0x1F;

    if (nalType == 7) {
        info.hasSps = true;
        SpsInfo sps;
        if (parseSps(payload, payloadSize, sps)) {
            activeWidth = sps.width;
            activeHeight = sps.height;
            info.width = sps.width;
            info.height = sps.height;
            info.profile = sps.profile;
            info.pictureOrderType = sps.pictureOrderType;
            info.parsedSps = true;
        }
    } else if (nalType == 8) {
        info.hasPps = true;
    } else if (nalType == 5 || nalType == 1) {
        info.hasSlice = true;
        if (nalType == 5) {
            ++info.idrCount;
            info.hasIdr = true;
        }
    }
}

void skipScalingList(BitReader& reader, uint32_t size)
{
    int32_t lastScale = 8;
    int32_t nextScale = 8;
    for (uint32_t i = 0; i < size && !reader.failed(); ++i) {
        if (nextScale != 0) {
            const int32_t deltaScale = reader.readSe();
            nextScale = static_cast<int32_t>(
                (static_cast<int64_t>(lastScale) + deltaScale + 256) % 256);
        }
        lastScale = nextScale != 0 ? nextScale : lastScale;
    }
}

bool parseSps(
    const uint8_t* nal,
    size_t size,
    SpsInfo& sps)
{
    if (size < 2)
        return false;

    BitReader reader(nal + 1, size - 1);

    const uint32_t profileIdc = reader.readBits(8);
    sps.profile = profileIdc;
    reader.readBits(8); // constraints
    reader.readBits(8); // level_idc
    reader.readUe();    // sps_id

    uint32_t chromaFormatIdc = 1;
    bool separateColourPlaneFlag = false;
    switch (profileIdc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        chromaFormatIdc = reader.readUe();
        if (chromaFormatIdc > 3)
            return false;
        if (chromaFormatIdc == 3)
            separateColourPlaneFlag = reader.readBool();
        reader.readUe();   // bit_depth_luma_minus8
        reader.readUe();   // bit_depth_chroma_minus8
        reader.readBool(); // qpprime_y_zero_transform_bypass_flag
        if (reader.readBool()) {
            const uint32_t scalingCount = chromaFormatIdc == 3 ? 12 : 8;
            for (uint32_t i = 0; i < scalingCount && !reader.failed(); ++i) {
                if (reader.readBool())
                    skipScalingList(reader, i < 6 ? 16 : 64);
            }
        }
        break;
    default:
        break;
    }

    reader.readUe(); // log2_max_frame_num_minus4
    const uint32_t picOrderCntType = reader.readUe();
    sps.pictureOrderType = picOrderCntType;
    if (picOrderCntType > 2)
        return false;
    if (picOrderCntType == 0) {
        reader.readUe();
    } else if (picOrderCntType == 1) {
        reader.readBool();
        reader.readSe();
        reader.readSe();
        const uint32_t cycleCount = reader.readUe();
        for (uint32_t i = 0; i < cycleCount && !reader.failed(); ++i)
            reader.readSe();
    }

    reader.readUe(); // max_num_ref_frames
    reader.readBool(); // gaps_in_frame_num_value_allowed_flag
    const uint32_t picWidthInMbsMinus1 = reader.readUe();
    const uint32_t picHeightInMapUnitsMinus1 = reader.readUe();
    const bool frameMbsOnlyFlag = reader.readBool();
    if (!frameMbsOnlyFlag)
        reader.readBool();
    reader.readBool();

    uint32_t cropLeft = 0;
    uint32_t cropRight = 0;
    uint32_t cropTop = 0;
    uint32_t cropBottom = 0;
    if (reader.readBool()) {
        cropLeft = reader.readUe();
        cropRight = reader.readUe();
        cropTop = reader.readUe();
        cropBottom = reader.readUe();
    }

    uint32_t cropUnitX = 1;
    uint32_t cropUnitY = 2 - static_cast<uint32_t>(frameMbsOnlyFlag);
    if (!(chromaFormatIdc == 0 || (chromaFormatIdc == 3 && separateColourPlaneFlag))) {
        const uint32_t subWidthC = chromaFormatIdc == 1 || chromaFormatIdc == 2 ? 2 : 1;
        const uint32_t subHeightC = chromaFormatIdc == 1 ? 2 : 1;
        cropUnitX = subWidthC;
        cropUnitY = subHeightC * (2 - static_cast<uint32_t>(frameMbsOnlyFlag));
    }

    const uint64_t codedWidth = (static_cast<uint64_t>(picWidthInMbsMinus1) + 1) * 16;
    const uint64_t codedHeight = (2 - static_cast<uint32_t>(frameMbsOnlyFlag)) *
        (static_cast<uint64_t>(picHeightInMapUnitsMinus1) + 1) * 16;
    const uint64_t cropX = (static_cast<uint64_t>(cropLeft) + cropRight) * cropUnitX;
    const uint64_t cropY = (static_cast<uint64_t>(cropTop) + cropBottom) * cropUnitY;
    if (reader.failed() || codedWidth > UINT32_MAX - 15u ||
        codedHeight > UINT32_MAX - 15u || cropX >= codedWidth || cropY >= codedHeight)
        return false;
    sps.width = static_cast<uint32_t>(codedWidth - cropX);
    sps.height = static_cast<uint32_t>(codedHeight - cropY);
    return true;
}

} // namespace

H264AccessUnitInfo H264AnnexBParser::inspectAccessUnit(const uint8_t* data, size_t size)
{
    H264AccessUnitInfo info;
    info.accessUnitCount = ++accessUnitCount_;
    info.bytes = static_cast<uint32_t>(std::min<size_t>(size, 0xFFFFFFFFu));

    size_t search = 0;
    for (;;) {
        size_t codeSize = 0;
        const size_t start = findStartCode(data, size, search, codeSize);
        if (start == size)
            break;
        const size_t payload = start + codeSize;
        size_t nextCodeSize = 0;
        const size_t end = findStartCode(data, size, payload, nextCodeSize);
        inspectNal(
            data + payload,
            end - payload,
            info,
            activeWidth_,
            activeHeight_
        );
        if (end == size)
            break;
        search = end;
    }

    if (info.width == 0)
        info.width = activeWidth_;
    if (info.height == 0)
        info.height = activeHeight_;

    return info;
}

} // namespace video
