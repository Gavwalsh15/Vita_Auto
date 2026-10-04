#include "video/vita_h264_decoder.h"
#include "app/config.h"

#include <algorithm>
#include <cstring>

#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>
#include <psp2/sysmodule.h>

namespace video {
namespace {

constexpr uint32_t ALIGNMENT = 256 * 1024;
constexpr uint32_t DECODER_ALIGNMENT = 1024 * 1024;
constexpr uint32_t MAX_ES_BUFFER_SIZE = 512 * 1024;
constexpr uint32_t INPUT_PADDING_SIZE = 64;
constexpr uint32_t NUM_REF_FRAMES = 4;
constexpr uint32_t NUM_STREAMS = 1;
constexpr uint32_t RGBA_BYTES_PER_PIXEL = 4;

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void setTimestampUnknown(SceVideodecTimeStamp& timestamp)
{
    timestamp.upper = 0xFFFFFFFFu;
    timestamp.lower = 0xFFFFFFFFu;
}

} // namespace

bool VitaH264Decoder::allocateMemBlock(
    const char* name,
    uint32_t size,
    int memoryType,
    uint32_t alignment,
    int& uid,
    void*& base)
{
    const uint32_t alignedSize = alignUp(size, alignment);
    uid = sceKernelAllocMemBlock(
        name,
        static_cast<SceKernelMemBlockType>(memoryType),
        alignedSize,
        nullptr
    );
    if (uid < 0) {
        lastError_ = uid;
        base = nullptr;
        return false;
    }

    const int result = sceKernelGetMemBlockBase(uid, &base);
    if (result < 0) {
        lastError_ = result;
        sceKernelFreeMemBlock(uid);
        uid = -1;
        base = nullptr;
        return false;
    }

    std::memset(base, 0, alignedSize);
    return true;
}

void VitaH264Decoder::freeMemBlock(int& uid, void*& base)
{
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
    uid = -1;
    base = nullptr;
}

bool VitaH264Decoder::init(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return false;

    if (initialized_ && width_ == width && height_ == height)
        return true;

    shutdown();
    width_ = width;
    height_ = height;
    lastInitStage_ = 1;

    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    if (result == SCE_SYSMODULE_ERROR_INVALID_VALUE) {
        // Some homebrew/runtime combinations reject the public AVCDEC
        // sysmodule ID even though SceVideodec stubs are linked. Continue and
        // let sceVideodecInitLibrary report the real decoder availability.
        libraryLoaded_ = false;
    } else if (result < 0 && result != SCE_SYSMODULE_LOADED) {
        lastError_ = result;
        return false;
    } else {
        libraryLoaded_ = true;
    }
    lastInitStage_ = 2;

    SceVideodecQueryInitInfoHwAvcdec initInfo{};
    initInfo.size = sizeof(initInfo);
    initInfo.horizontal = alignUp(width, 16);
    initInfo.vertical = alignUp(height, 16);
    initInfo.numOfRefFrames = NUM_REF_FRAMES;
    initInfo.numOfStreams = NUM_STREAMS;

    result = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &initInfo);
    if (result < 0 && result != SCE_VIDEODEC_ERROR_ALREADY_USED) {
        lastError_ = result;
        return false;
    }
    libraryInitialized_ = true;
    lastInitStage_ = 3;

    SceAvcdecQueryDecoderInfo query{};
    query.horizontal = initInfo.horizontal;
    query.vertical = initInfo.vertical;
    query.numOfRefFrames = NUM_REF_FRAMES;

    SceAvcdecDecoderInfo decoderInfo{};
    result = sceAvcdecQueryDecoderMemSize(
        SCE_VIDEODEC_TYPE_HW_AVCDEC,
        &query,
        &decoderInfo
    );
    if (result < 0) {
        lastError_ = result;
        return false;
    }
    lastInitStage_ = 4;

    if (!allocateMemBlock(
            "vita_auto_avcdec_frame",
            decoderInfo.frameMemSize,
            SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
            DECODER_ALIGNMENT,
            decoderMemUid_,
            decoderMem_))
        return false;
    lastInitStage_ = 5;

    if (!allocateMemBlock(
            "vita_auto_avcdec_es",
            MAX_ES_BUFFER_SIZE + INPUT_PADDING_SIZE,
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            ALIGNMENT,
            esMemUid_,
            esMem_))
        return false;
    lastInitStage_ = 6;

    // AVCDEC can scale while decoding. Decode the 720p Android Auto stream
    // directly into the Vita's native framebuffer dimensions so no full-size
    // 1280x720 RGBA copy or CPU resize is needed.
    const uint32_t pictureBytes =
        app::config::SCREEN_WIDTH * app::config::SCREEN_HEIGHT * RGBA_BYTES_PER_PIXEL;
    const uint32_t pictureMemBytes = alignUp(pictureBytes, ALIGNMENT);
    if (!allocateMemBlock(
            "vita_auto_avcdec_picture",
            pictureBytes,
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            ALIGNMENT,
            pictureMemUid_,
            pictureMem_))
        return false;

    // Make AVCDEC's output directly readable by GXM. Mapping failure is
    // non-fatal: the renderer retains its established copy fallback.
    pictureGpuMapped_ = sceGxmMapMemory(
        pictureMem_,
        pictureMemBytes,
        SCE_GXM_MEMORY_ATTRIB_READ
    ) >= 0;

    lastInitStage_ = 7;

    std::memset(&decoder_, 0, sizeof(decoder_));
    decoder_.frameBuf.pBuf = decoderMem_;
    // Match the proven C decoder: AVCDEC must see the aligned physical
    // allocation size, not only the smaller raw query result.
    decoder_.frameBuf.size = alignUp(
        decoderInfo.frameMemSize,
        DECODER_ALIGNMENT
    );

    result = sceAvcdecCreateDecoder(
        SCE_VIDEODEC_TYPE_HW_AVCDEC,
        &decoder_,
        &query
    );
    if (result < 0) {
        lastError_ = result;
        return false;
    }
    lastInitStage_ = 8;

    std::memset(&picture_, 0, sizeof(picture_));
    picture_.size = sizeof(SceAvcdecPicture);
    picture_.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    // SceAvcdec framePitch is measured in pixels. These are the exact
    // output fields used by established Vita hardware-decoder clients.
    picture_.frame.framePitch = app::config::SCREEN_WIDTH;
    picture_.frame.frameWidth = app::config::SCREEN_WIDTH;
    picture_.frame.frameHeight = app::config::SCREEN_HEIGHT;
    picture_.frame.pPicture[0] = pictureMem_;
    picture_.frame.pPicture[1] = nullptr;
    picturePtr_ = &picture_;

    latestFrame_ = {};
    latestFrame_.width = app::config::SCREEN_WIDTH;
    latestFrame_.height = app::config::SCREEN_HEIGHT;
    latestFrame_.pitch = app::config::SCREEN_WIDTH;
    latestFrame_.gpuMapped = pictureGpuMapped_;
    submittedAccessUnits_ = 0;
    initialized_ = true;
    lastError_ = 0;
    lastInitStage_ = 9;
    return true;
}

bool VitaH264Decoder::submitAccessUnit(const uint8_t* data, size_t size)
{
    if (!initialized_ || data == nullptr || size == 0)
        return false;
    if (size > MAX_ES_BUFFER_SIZE) {
        lastError_ = SCE_AVCDEC_ERROR_ES_BUFFER_FULL;
        return false;
    }

    std::memcpy(esMem_, data, size);
    std::memset(
        static_cast<uint8_t*>(esMem_) + size,
        0,
        INPUT_PADDING_SIZE
    );

    SceAvcdecAu au{};
    setTimestampUnknown(au.pts);
    setTimestampUnknown(au.dts);
    au.es.pBuf = esMem_;
    au.es.size = static_cast<uint32_t>(size);

    SceAvcdecArrayPicture output{};
    output.numOfElm = 1;
    output.numOfOutput = 0;
    output.pPicture = &picturePtr_;

    const int result = sceAvcdecDecode(&decoder_, &au, &output);
    lastOutputCount_ = output.numOfOutput;
    if (result < 0) {
        lastError_ = result;
        return false;
    }

    ++submittedAccessUnits_;
#if VITA_AUTO_VERBOSE_LOGGING
    outputPictures_ += output.numOfOutput;
#endif

    if (output.numOfOutput > 0 && output.pPicture != nullptr && output.pPicture[0] != nullptr) {
        const SceAvcdecFrame& frame = output.pPicture[0]->frame;
        latestFrame_.pixels = static_cast<const uint8_t*>(frame.pPicture[0]);
        latestFrame_.width = frame.frameWidth;
        latestFrame_.height = frame.frameHeight;
        latestFrame_.pitch = frame.framePitch;
        latestFrame_.gpuMapped = frame.pPicture[0] == pictureMem_ && pictureGpuMapped_;
    }

    lastError_ = 0;
    return true;
}

bool VitaH264Decoder::hasFrame() const
{
    return latestFrame_.pixels != nullptr;
}

const DecodedFrame& VitaH264Decoder::latestFrame() const
{
    return latestFrame_;
}

void VitaH264Decoder::shutdown()
{
    if (initialized_)
        sceAvcdecDeleteDecoder(&decoder_);

    if (pictureGpuMapped_ && pictureMem_ != nullptr)
        sceGxmUnmapMemory(pictureMem_);
    pictureGpuMapped_ = false;
    freeMemBlock(pictureMemUid_, pictureMem_);
    freeMemBlock(esMemUid_, esMem_);
    freeMemBlock(decoderMemUid_, decoderMem_);

    if (libraryInitialized_)
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    if (libraryLoaded_)
        sceSysmoduleUnloadModule(SCE_SYSMODULE_AVCDEC);

    initialized_ = false;
    libraryInitialized_ = false;
    libraryLoaded_ = false;
    width_ = 0;
    height_ = 0;
    submittedAccessUnits_ = 0;
    lastInitStage_ = 0;
#if VITA_AUTO_VERBOSE_LOGGING
    outputPictures_ = 0;
#endif
    lastOutputCount_ = 0;
    decoder_ = {};
    picture_ = {};
    picturePtr_ = nullptr;
    latestFrame_ = {};
}

} // namespace video
