#include "audio/aac_decoder.h"

#include "logger/vita_log.h"

#include <psp2/sysmodule.h>

#include <cstring>

namespace audio {

namespace {

constexpr size_t ADTS_HEADER = 7;
constexpr size_t ES_CAPACITY = 0x800;                           // one access unit plus its header
constexpr size_t PCM_CAPACITY = SCE_AUDIODEC_AAC_MAX_SAMPLES * 2 * sizeof(int16_t);

uint8_t* alignUp(std::vector<uint8_t>& storage, size_t size)
{
    storage.assign(size + SCE_AUDIODEC_ALIGNMENT_SIZE, 0);
    const uintptr_t base = reinterpret_cast<uintptr_t>(storage.data());
    return reinterpret_cast<uint8_t*>((base + SCE_AUDIODEC_ALIGNMENT_SIZE - 1) &
                                      ~static_cast<uintptr_t>(SCE_AUDIODEC_ALIGNMENT_SIZE - 1));
}

int samplingIndex(uint32_t rate)
{
    switch (rate) {
    case 96000: return 0;
    case 88200: return 1;
    case 64000: return 2;
    case 48000: return 3;
    case 44100: return 4;
    case 32000: return 5;
    case 24000: return 6;
    case 22050: return 7;
    case 16000: return 8;
    case 12000: return 9;
    case 11025: return 10;
    case 8000: return 11;
    default: return -1;
    }
}

} // namespace

AacDecoder::~AacDecoder()
{
    close();
}

bool AacDecoder::create(uint32_t infoSize)
{
    std::memset(&info_, 0, sizeof(info_));
    info_.aac.size = infoSize;
    info_.aac.isAdts = 1;
    info_.aac.ch = channels_;
    info_.aac.samplingRate = sampleRate_;

    std::memset(&ctrl_, 0, sizeof(ctrl_));
    ctrl_.size = sizeof(SceAudiodecCtrl);
    ctrl_.pEs = es_;
    ctrl_.maxEsSize = SCE_AUDIODEC_AAC_MAX_ES_SIZE;
    ctrl_.pPcm = pcm_;
    ctrl_.maxPcmSize = static_cast<SceUInt32>(PCM_CAPACITY);
    ctrl_.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    ctrl_.pInfo = &info_;

    const int result = sceAudiodecCreateDecoder(&ctrl_, SCE_AUDIODEC_TYPE_AAC);
    if (result < 0) {
        lastError_ = result;
        return false;
    }
    return true;
}

bool AacDecoder::open(uint32_t sampleRate, uint8_t channels)
{
    close();
    if (samplingIndex(sampleRate) < 0 || (channels != 1 && channels != 2)) {
        lastError_ = -1;
        return false;
    }
    sampleRate_ = sampleRate;
    channels_ = channels;
    es_ = alignUp(esStorage_, ES_CAPACITY);
    pcm_ = alignUp(pcmStorage_, PCM_CAPACITY);

    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);
    if (result < 0 && result != SCE_SYSMODULE_LOADED) {
        // The decoder may already be resident; the library init below decides.
        vitaLog("aac: AudioCodec module load returned 0x%08x", static_cast<unsigned>(result));
    } else {
        moduleLoaded_ = true;
    }

    SceAudiodecInitParam init;
    std::memset(&init, 0, sizeof(init));
    init.size = sizeof(SceAudiodecInitStreamParam);
    init.aac.totalStreams = 1;
    result = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &init);
    if (result < 0 && result != static_cast<int>(SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED)) {
        lastError_ = result;
        vitaLog("aac: sceAudiodecInitLibrary failed 0x%08x", static_cast<unsigned>(result));
        close();
        return false;
    }
    libraryUp_ = true;

    // The info block's size field is the codec's own struct in the documentation
    // and the union in some callers; the decoder accepts one of them.
    if (!create(sizeof(SceAudiodecInfoAac)) && !create(sizeof(SceAudiodecInfo))) {
        vitaLog("aac: sceAudiodecCreateDecoder failed 0x%08x", static_cast<unsigned>(lastError_));
        close();
        return false;
    }
    open_ = true;
    return true;
}

void AacDecoder::close()
{
    if (open_) {
        sceAudiodecDeleteDecoder(&ctrl_);
        open_ = false;
    }
    if (libraryUp_) {
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
        libraryUp_ = false;
    }
    if (moduleLoaded_) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_AUDIOCODEC);
        moduleLoaded_ = false;
    }
}

int AacDecoder::decode(const uint8_t* unit, size_t length, std::vector<int16_t>& out)
{
    out.clear();
    if (!open_ || length == 0 || length + ADTS_HEADER > ES_CAPACITY)
        return -1;
    const size_t frame = length + ADTS_HEADER;
    const int index = samplingIndex(sampleRate_);
    es_[0] = 0xFF;
    es_[1] = 0xF1; // MPEG-4, no CRC
    es_[2] = static_cast<uint8_t>((1 << 6) | (index << 2) | ((channels_ >> 2) & 1)); // LC profile
    es_[3] = static_cast<uint8_t>(((channels_ & 3) << 6) | ((frame >> 11) & 3));
    es_[4] = static_cast<uint8_t>((frame >> 3) & 0xFF);
    es_[5] = static_cast<uint8_t>(((frame & 7) << 5) | 0x1F);
    es_[6] = 0xFC;
    std::memcpy(es_ + ADTS_HEADER, unit, length);

    ctrl_.inputEsSize = static_cast<SceUInt32>(frame);
    ctrl_.outputPcmSize = 0;
    const int result = sceAudiodecDecode(&ctrl_);
    if (result < 0) {
        lastError_ = result;
        return -1;
    }
    const size_t samples = ctrl_.outputPcmSize / sizeof(int16_t);
    const int16_t* pcm = reinterpret_cast<const int16_t*>(pcm_);
    out.assign(pcm, pcm + samples);
    return static_cast<int>(samples);
}

} // namespace audio
