#include "audio/audio_stream.h"

#include "logger/vita_log.h"

namespace audio {
namespace {

const char* encodingName(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Pcm16Le:
    case Encoding::Pcm16Be: return "PCM";
    case Encoding::AacLc: return "AAC-LC";
    }
    return "?";
}

} // namespace

bool Stream::configure(Encoding encoding, uint32_t sampleRate, uint8_t channels, uint32_t bufferMs)
{
    if (channels != 1 && channels != 2)
        return false;
    if (output_ && encoding == encoding_ && sampleRate == rate_ && channels == channels_ && bufferMs == bufferMs_)
        return true;
    output_.reset(new Output(sampleRate, channels, bufferMs));
    aac_.reset();
    encoding_ = encoding;
    rate_ = sampleRate;
    channels_ = channels;
    bufferMs_ = bufferMs;
    if (encoding == Encoding::AacLc) {
        aac_.reset(new AacDecoder());
        if (!aac_->open(sampleRate, channels))
            vitaLog("audio: %s AAC decoder did not start (0x%08x)", name_,
                    static_cast<unsigned>(aac_->lastError()));
    }
    vitaLog("audio: %s is %s, %u Hz, %u channel(s)", name_, encodingName(encoding),
            static_cast<unsigned>(sampleRate), static_cast<unsigned>(channels));
    return true;
}

void Stream::reset()
{
    output_.reset();
    aac_.reset();
    rate_ = 0;
    channels_ = 0;
    packets_ = 0;
    bytes_ = 0;
    dropped_ = 0;
}

bool Stream::drop(const char* why, int error)
{
    if (++dropped_ <= 3)
        vitaLog("audio: %s packet dropped: %s (0x%08x)", name_, why, static_cast<unsigned>(error));
    return false;
}

bool Stream::push(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0)
        return true;
    if (!output_)
        return drop("no output", 0);
    const int16_t* samples = nullptr;
    size_t count = 0;
    switch (encoding_) {
    case Encoding::Pcm16Le:
        if ((size & 1u) != 0)
            return drop("odd PCM length", static_cast<int>(size));
        samples = reinterpret_cast<const int16_t*>(data);
        count = size / 2;
        break;
    case Encoding::Pcm16Be:
        if ((size & 1u) != 0)
            return drop("odd PCM length", static_cast<int>(size));
        count = size / 2;
        samples_.resize(count);
        for (size_t i = 0; i < count; ++i)
            samples_[i] = static_cast<int16_t>((data[2 * i] << 8) | data[2 * i + 1]);
        samples = samples_.data();
        break;
    case Encoding::AacLc:
        if (!aac_ || !aac_->isOpen())
            return drop("no AAC decoder", aac_ ? aac_->lastError() : 0);
        if (aac_->decode(data, size, samples_) <= 0)
            return drop("AAC decode failed", aac_->lastError());
        samples = samples_.data();
        count = samples_.size();
        break;
    }
    if (!output_->pushPcm16(samples, count))
        return drop("output refused it", output_->lastError());
    ++packets_;
    bytes_ += size;
    return true;
}

void Stream::logStats(const char* prefix) const
{
    const int error = output_ ? output_->lastError() : 0;
    const uint32_t underruns = output_ ? output_->underruns() : 0;
    if (!VITA_AUTO_VERBOSE_LOGGING && dropped_ == 0 && error == 0 && underruns == 0)
        return;
    vitaLog("%s: %s audio %u packets/%lluB dropped=%u underruns=%u error=0x%08x (%s)", prefix, name_,
            static_cast<unsigned>(packets_), static_cast<unsigned long long>(bytes_),
            static_cast<unsigned>(dropped_), static_cast<unsigned>(underruns), static_cast<unsigned>(error),
            output_ ? output_->lastErrorName() : "none");
}

} // namespace audio
