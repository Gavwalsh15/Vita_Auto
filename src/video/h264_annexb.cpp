#include "video/h264_annexb.h"

#include <vector>

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

class BitWriter {
public:
    void put(uint32_t value, uint32_t count)
    {
        for (uint32_t i = count; i > 0; --i)
            putBit((value >> (i - 1)) & 1u);
    }

    void putUe(uint32_t value)
    {
        const uint64_t code = static_cast<uint64_t>(value) + 1;
        uint32_t length = 0;
        while ((code >> length) != 0)
            ++length;
        put(0, length - 1);
        for (uint32_t i = length; i > 0; --i)
            putBit(static_cast<uint32_t>((code >> (i - 1)) & 1u));
    }

    void finish()
    {
        putBit(1);
        while (used_ != 0)
            putBit(0);
    }

    const std::vector<uint8_t>& bytes() const
    {
        return bytes_;
    }

private:
    void putBit(uint32_t bit)
    {
        if (used_ == 0)
            bytes_.push_back(0);
        bytes_.back() = static_cast<uint8_t>(bytes_.back() | (bit << (7 - used_)));
        used_ = (used_ + 1) & 7u;
    }

    std::vector<uint8_t> bytes_;
    uint32_t used_ = 0;
};

class SpsCopier {
public:
    SpsCopier(const std::vector<uint8_t>& rbsp, BitWriter& out)
        : rbsp_(rbsp), out_(out)
    {
    }

    uint32_t bits(uint32_t count)
    {
        const uint32_t value = readBits(count);
        out_.put(value, count);
        return value;
    }

    uint32_t ue()
    {
        const uint32_t value = readUe();
        out_.putUe(value);
        return value;
    }

    int32_t se()
    {
        const uint32_t value = ue();
        const int32_t magnitude = static_cast<int32_t>((value >> 1) + (value & 1u));
        return (value & 1u) ? magnitude : -magnitude;
    }

    uint32_t readBits(uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (failed_ || position_ >= rbsp_.size() * 8) {
                failed_ = true;
                return 0;
            }
            const uint32_t bit = (rbsp_[position_ >> 3] >> (7 - (position_ & 7))) & 1u;
            ++position_;
            value = (value << 1) | bit;
        }
        return value;
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

    bool failed() const
    {
        return failed_;
    }

private:
    const std::vector<uint8_t>& rbsp_;
    BitWriter& out_;
    size_t position_ = 0;
    bool failed_ = false;
};

bool profileHasChromaFormat(uint32_t profileIdc)
{
    switch (profileIdc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

void copyScalingList(SpsCopier& c, uint32_t size)
{
    int32_t lastScale = 8;
    int32_t nextScale = 8;
    for (uint32_t i = 0; i < size && !c.failed(); ++i) {
        if (nextScale != 0) {
            const int32_t delta = c.se();
            nextScale = static_cast<int32_t>(
                (static_cast<int64_t>(lastScale) + delta + 256) % 256);
        }
        lastScale = nextScale != 0 ? nextScale : lastScale;
    }
}

void copyHrd(SpsCopier& c)
{
    const uint32_t cpbCount = c.ue() + 1;
    c.bits(4); // bit_rate_scale
    c.bits(4); // cpb_size_scale
    for (uint32_t i = 0; i < cpbCount && i < 32 && !c.failed(); ++i) {
        c.ue();
        c.ue();
        c.bits(1);
    }
    c.bits(20); // four 5-bit length fields
}

bool copyVuiUntilRestriction(SpsCopier& c)
{
    if (c.bits(1)) {                // aspect_ratio_info_present_flag
        if (c.bits(8) == 255) {     // Extended_SAR
            c.bits(16);
            c.bits(16);
        }
    }
    if (c.bits(1))                  // overscan_info_present_flag
        c.bits(1);
    if (c.bits(1)) {                // video_signal_type_present_flag
        c.bits(3);
        c.bits(1);
        if (c.bits(1))              // colour_description_present_flag
            c.bits(24);
    }
    if (c.bits(1)) {                // chroma_loc_info_present_flag
        c.ue();
        c.ue();
    }
    if (c.bits(1)) {                // timing_info_present_flag
        c.bits(32);
        c.bits(32);
        c.bits(1);
    }
    const bool nalHrd = c.bits(1) != 0;
    if (nalHrd)
        copyHrd(c);
    const bool vclHrd = c.bits(1) != 0;
    if (vclHrd)
        copyHrd(c);
    if (nalHrd || vclHrd)
        c.bits(1);                  // low_delay_hrd_flag
    c.bits(1);                      // pic_struct_present_flag
    return c.readBits(1) != 0;
}

enum class SpsRewrite { Failed, AlreadyRestricted, Skipped, Rewritten };

SpsRewrite rewriteSps(
    const uint8_t* nal,
    size_t size,
    std::vector<uint8_t>& out,
    SpsRewriteInfo& info)
{
    if (size < 5)
        return SpsRewrite::Failed;

    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    uint32_t zeros = 0;
    for (size_t i = 1; i < size; ++i) {
        if (zeros >= 2 && nal[i] == 0x03) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        rbsp.push_back(nal[i]);
    }

    BitWriter writer;
    SpsCopier c(rbsp, writer);
    const uint32_t profile = c.bits(8);
    c.bits(16);     // constraint flags, level_idc
    c.ue();         // seq_parameter_set_id
    if (profileHasChromaFormat(profile)) {
        const uint32_t chromaFormat = c.ue();
        if (chromaFormat > 3)
            return SpsRewrite::Failed;
        if (chromaFormat == 3)
            c.bits(1);
        c.ue();     // bit depths
        c.ue();
        c.bits(1);  // qpprime_y_zero_transform_bypass_flag
        if (c.bits(1)) {
            const uint32_t listCount = chromaFormat == 3 ? 12 : 8;
            for (uint32_t i = 0; i < listCount && !c.failed(); ++i) {
                if (c.bits(1))
                    copyScalingList(c, i < 6 ? 16 : 64);
            }
        }
    }
    c.ue();         // log2_max_frame_num_minus4
    const uint32_t pocType = c.ue();
    if (pocType > 2)
        return SpsRewrite::Failed;
    if (pocType == 0) {
        c.ue();
    } else if (pocType == 1) {
        c.bits(1);
        c.se();
        c.se();
        const uint32_t cycle = c.ue();
        if (cycle > 255)
            return SpsRewrite::Failed;
        for (uint32_t i = 0; i < cycle && !c.failed(); ++i)
            c.se();
    }
    const uint32_t refFrames = c.ue();
    c.bits(1);      // gaps_in_frame_num_value_allowed_flag
    c.ue();         // pic_width_in_mbs_minus1
    c.ue();         // pic_height_in_map_units_minus1
    if (!c.bits(1)) // frame_mbs_only_flag
        c.bits(1);  // mb_adaptive_frame_field_flag
    c.bits(1);      // direct_8x8_inference_flag
    if (c.bits(1)) {                      // frame_cropping_flag
        c.ue();
        c.ue();
        c.ue();
        c.ue();
    }

    const bool hasVui = c.readBits(1) != 0;
    writer.put(1, 1); 
    bool restricted = false;
    if (hasVui)
        restricted = copyVuiUntilRestriction(c);
    else
        writer.put(0, 8);
    if (c.failed())
        return SpsRewrite::Failed;

    info.maxNumRefFrames = refFrames;
    info.hadVui = hasVui;
    if (restricted) {
        info.hadRestriction = true;
        return SpsRewrite::AlreadyRestricted;
    }
    // Declaring zero reordering is only true when pictures cannot be reordered:
    // Baseline has no B-frames and picture order count type 2 forbids them. On
    // anything else the rewrite could make the decoder show frames out of order.
    if (profile != 66 && pocType != 2)
        return SpsRewrite::Skipped;

    writer.put(1, 1); // bitstream_restriction_flag
    writer.put(1, 1); // motion_vectors_over_pic_boundaries_flag
    writer.putUe(0);  // max_bytes_per_pic_denom
    writer.putUe(0);  // max_bits_per_mb_denom
    writer.putUe(16); // log2_max_mv_length_horizontal
    writer.putUe(16); // log2_max_mv_length_vertical
    writer.putUe(0);  // max_num_reorder_frames: pictures leave in decode order
    writer.putUe(refFrames); // max_dec_frame_buffering: no more than it must hold
    writer.finish();

    out.clear();
    out.push_back(nal[0]);
    zeros = 0;
    for (const uint8_t byte : writer.bytes()) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return SpsRewrite::Rewritten;
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

SpsRewriteInfo rewriteSpsForLowLatency(std::vector<uint8_t>& accessUnit)
{
    SpsRewriteInfo info;
    const uint8_t* data = accessUnit.data();
    const size_t size = accessUnit.size();
    std::vector<uint8_t> out;
    std::vector<uint8_t> replacement;
    size_t copied = 0;
    size_t search = 0;

    for (;;) {
        size_t codeSize = 0;
        const size_t start = findStartCode(data, size, search, codeSize);
        if (start == size)
            break;
        const size_t payload = start + codeSize;
        size_t nextCodeSize = 0;
        const size_t end = findStartCode(data, size, payload, nextCodeSize);
        size_t nalEnd = end;
        while (nalEnd > payload && data[nalEnd - 1] == 0)
            --nalEnd; // trailing_zero_8bits

        if (nalEnd > payload && (data[payload] & 0x1F) == 7) {
            ++info.spsCount;
            switch (rewriteSps(data + payload, nalEnd - payload, replacement, info)) {
            case SpsRewrite::Rewritten:
                out.insert(out.end(), data + copied, data + payload);
                out.insert(out.end(), replacement.begin(), replacement.end());
                copied = nalEnd;
                ++info.rewritten;
                break;
            case SpsRewrite::Failed:
                ++info.failed;
                break;
            case SpsRewrite::Skipped:
                ++info.skipped;
                break;
            case SpsRewrite::AlreadyRestricted:
                break;
            }
        }
        if (end == size)
            break;
        search = end;
    }

    if (info.rewritten != 0) {
        out.insert(out.end(), data + copied, data + size);
        accessUnit.swap(out);
    }
    return info;
}

} // namespace video
