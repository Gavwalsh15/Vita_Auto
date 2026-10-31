#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "android_auto/aa_tls.h"

// Needed for handshake
#include <psp2/kernel/rng.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "android_auto/aa_credentials.h"
#include "logger/vita_log.h"

namespace android_auto {

namespace {

constexpr char PERSONALIZATION[] = "vita-auto-aa";
constexpr size_t MAX_TLS_BUFFER = 64 * 1024;

// A corrupted USB read can make a record fail authentication. mbedTLS has
// already consumed that record when it returns one of these, and the stream
// may be resynchronised from the AA frame layer.
bool recoverableRecordError(int error)
{
    return error == MBEDTLS_ERR_SSL_INVALID_MAC ||
           error == MBEDTLS_ERR_SSL_INVALID_RECORD ||
           error == MBEDTLS_ERR_SSL_DECODE_ERROR;
}

}

TlsClient::TlsClient()
    : ssl_(new mbedtls_ssl_context)
    , config_(new mbedtls_ssl_config)
    , certificate_(new mbedtls_x509_crt)
    , key_(new mbedtls_pk_context)
    , rng_(new mbedtls_ctr_drbg_context)
{
    inbound_.reserve(MAX_TLS_BUFFER);
    outbound_.reserve(MAX_TLS_BUFFER);
    mbedtls_ssl_init(ssl_);
    mbedtls_ssl_config_init(config_);
    mbedtls_x509_crt_init(certificate_);
    mbedtls_pk_init(key_);
    mbedtls_ctr_drbg_init(rng_);
}

TlsClient::~TlsClient()
{
    mbedtls_ctr_drbg_free(rng_);
    mbedtls_pk_free(key_);
    mbedtls_x509_crt_free(certificate_);
    mbedtls_ssl_config_free(config_);
    mbedtls_ssl_free(ssl_);
    delete rng_;
    delete key_;
    delete certificate_;
    delete config_;
    delete ssl_;
}

int TlsClient::randomCallback(void*, unsigned char* data, size_t length)
{
    return sceKernelGetRandomNumber(data, length) < 0 ? -1 : 0;
}

bool TlsClient::fail(int error)
{
    if (!failed_) {
        lastError_ = error;
        failed_ = true;
    }
    return false;
}

bool TlsClient::start()
{
    if (started_ || failed_)
        return false;

    auto failInit = [this](const char* stage, int error) {
        vitaLog("direct-usb: TLS init failed stage=%s err=0x%08x", stage, error);
        return fail(error);
    };

    int error = mbedtls_ctr_drbg_seed(
        rng_, randomCallback, nullptr,
        reinterpret_cast<const unsigned char*>(PERSONALIZATION),
        sizeof(PERSONALIZATION) - 1
    );
    if (error != 0)
        return failInit("ctr_drbg_seed", error);

    error = mbedtls_x509_crt_parse(
        certificate_,
        reinterpret_cast<const unsigned char*>(HEAD_UNIT_CERTIFICATE),
        sizeof(HEAD_UNIT_CERTIFICATE)
    );
    if (error != 0)
        return failInit("x509_crt_parse", error);

    error = mbedtls_pk_parse_key(
        key_,
        reinterpret_cast<const unsigned char*>(HEAD_UNIT_PRIVATE_KEY),
        sizeof(HEAD_UNIT_PRIVATE_KEY),
        nullptr,
        0,
        mbedtls_ctr_drbg_random,
        rng_
    );
    if (error != 0)
        return failInit("pk_parse_key", error);

    error = mbedtls_ssl_config_defaults(
        config_,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (error != 0)
        return failInit("ssl_config_defaults", error);

    mbedtls_ssl_conf_authmode(config_, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(config_, mbedtls_ctr_drbg_random, rng_);
    mbedtls_ssl_conf_min_tls_version(
        config_, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(
        config_, MBEDTLS_SSL_VERSION_TLS1_2);

    error = mbedtls_ssl_conf_own_cert(config_, certificate_, key_);
    if (error != 0)
        return failInit("ssl_conf_own_cert", error);

    error = mbedtls_ssl_setup(ssl_, config_);
    if (error != 0)
        return failInit("ssl_setup", error);

    mbedtls_ssl_set_bio(ssl_, this, sendCallback, receiveCallback, nullptr);
    started_ = true;
    return advanceHandshake();
}

int TlsClient::sendCallback(void* context, const unsigned char* data, size_t length)
{
    TlsClient* client = static_cast<TlsClient*>(context);

    if (length > MAX_TLS_BUFFER || client->outbound_.size() + length > MAX_TLS_BUFFER)
        return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;

    if (client->outbound_.empty())
        client->outboundHandshake_ = !client->secure_;

    // Do not combine handshake and record output
    if (client->outboundHandshake_ != !client->secure_)
        return MBEDTLS_ERR_SSL_WANT_WRITE;

    client->outbound_.insert(client->outbound_.end(), data, data + length);
    return static_cast<int>(length);
}

int TlsClient::receiveCallback(void* context, unsigned char* data, size_t length)
{
    TlsClient* client = static_cast<TlsClient*>(context);
    const size_t available = client->inbound_.size() - client->inboundOffset_;

    if (available == 0)
        return MBEDTLS_ERR_SSL_WANT_READ;

    const size_t count = std::min(available, length);
    std::memcpy(data, client->inbound_.data() + client->inboundOffset_, count);
    client->inboundOffset_ += count;

    if (client->inboundOffset_ == client->inbound_.size()) {
        client->inbound_.clear();
        client->inboundOffset_ = 0;
    }

    return static_cast<int>(count);
}

bool TlsClient::advanceHandshake()
{
    if (!started_ || secure_ || failed_)
        return !failed_;

    const int result = mbedtls_ssl_handshake(ssl_);

    if (result == 0) {
        secure_ = true;
        lastError_ = 0;
        return true;
    }

    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
        return true;

    return fail(result);
}

bool TlsClient::feed(const uint8_t* data, size_t length)
{
    if (failed_)
        return false;
    if (!started_ || (data == nullptr && length != 0))
        return fail(MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
    if (length > MAX_TLS_BUFFER - (inbound_.size() - inboundOffset_))
        return fail(MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL);

    if (inboundOffset_ > 0) {
        inbound_.erase(inbound_.begin(), inbound_.begin() + inboundOffset_);
        inboundOffset_ = 0;
    }

    if (length != 0)
        inbound_.insert(inbound_.end(), data, data + length);

    return secure_ ? true : advanceHandshake();
}

bool TlsClient::write(const uint8_t* data, size_t length)
{
    if (failed_)
        return false;
    if (!secure_ || data == nullptr || length == 0 || length > INT32_MAX)
        return fail(MBEDTLS_ERR_SSL_BAD_INPUT_DATA);

    const int result = mbedtls_ssl_write(ssl_, data, length);

    if (result == static_cast<int>(length))
        return true;

    return fail(result < 0 ? result : MBEDTLS_ERR_SSL_INTERNAL_ERROR);
}

int TlsClient::read(uint8_t* data, size_t capacity)
{
    if (failed_)
        return -1;
    if (!secure_ || data == nullptr || capacity == 0) {
        fail(MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
        return -1;
    }

    // mbedTLS 3.6 queues a fatal bad_record_mac alert (and advances the
    // outbound sequence) before returning INVALID_MAC. The phone must not see
    // that alert, and it must not see a gap in our record sequence.
    unsigned char savedOutCtr[MBEDTLS_SSL_SEQUENCE_NUMBER_LEN];
    std::memcpy(savedOutCtr, ssl_->cur_out_ctr, sizeof(savedOutCtr));
    const size_t outboundBefore = outbound_.size();

    const int result = mbedtls_ssl_read(ssl_, data, capacity);

    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    if (result < 0) {
        if (recoverableRecordError(result) && cipherIsGcm()) {
            if (outbound_.size() > outboundBefore)
                outbound_.resize(outboundBefore);
            std::memcpy(ssl_->cur_out_ctr, savedOutCtr, sizeof(savedOutCtr));
            ssl_->send_alert = 0;
            ssl_->out_left = 0;
            ssl_->in_left = 0;
            inbound_.clear();
            inboundOffset_ = 0;
            lastError_ = result;
            return ReadRecoverable;
        }
        fail(result);
        return -1;
    }
    return result;
}

void TlsClient::discardInbound()
{
    inbound_.clear();
    inboundOffset_ = 0;
    if (ssl_ != nullptr)
        ssl_->in_left = 0;
}

bool TlsClient::cipherIsGcm() const
{
    const char* const suite = cipherSuite();
    return suite != nullptr && std::strstr(suite, "GCM") != nullptr;
}

const char* TlsClient::cipherSuite() const
{
    return ssl_ != nullptr ? mbedtls_ssl_get_ciphersuite(ssl_) : nullptr;
}

bool TlsClient::resyncToSequence(uint64_t sequence)
{
    if (ssl_ == nullptr || !secure_ || !cipherIsGcm() || ssl_->in_ctr == nullptr)
        return false;

    discardInbound();
    for (unsigned index = 0; index < MBEDTLS_SSL_SEQUENCE_NUMBER_LEN; ++index) {
        ssl_->in_ctr[index] = static_cast<unsigned char>(
            (sequence >> ((MBEDTLS_SSL_SEQUENCE_NUMBER_LEN - 1u - index) * 8u)) & 0xffu
        );
    }
    return true;
}

const uint8_t* TlsClient::outboundData() const
{
    return outbound_.data();
}

size_t TlsClient::outboundSize() const
{
    return outbound_.size();
}

bool TlsClient::outboundIsHandshake() const
{
    return outboundHandshake_;
}

void TlsClient::clearOutbound()
{
    outbound_.clear();
}

bool TlsClient::secure() const
{
    return secure_;
}

int TlsClient::lastError() const
{
    return lastError_;
}

}
