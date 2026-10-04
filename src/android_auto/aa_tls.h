#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct mbedtls_ssl_context;
struct mbedtls_ssl_config;
struct mbedtls_x509_crt;
struct mbedtls_pk_context;
struct mbedtls_ctr_drbg_context;

namespace android_auto {

// A TLS 1.2 client.
// AA wraps the resulting TLS records in its own frames; 
// callers feed and drain raw bytes.
class TlsClient {
public:
    TlsClient();
    ~TlsClient();

    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    bool start();
    bool feed(const uint8_t* data, size_t length);
    // Writes are all-or-fail; the session ends if TLS cannot accept the batch.
    bool write(const uint8_t* data, size_t length);
    int read(uint8_t* data, size_t capacity);

    const uint8_t* outboundData() const;
    size_t outboundSize() const;
    bool outboundIsHandshake() const;
    void clearOutbound();
    bool secure() const;
    int lastError() const;

private:
    bool fail(int error);
    static int sendCallback(void* context, const unsigned char* data, size_t length);
    static int receiveCallback(void* context, unsigned char* data, size_t length);
    static int randomCallback(void* context, unsigned char* data, size_t length);
    bool advanceHandshake();

    mbedtls_ssl_context* ssl_;
    mbedtls_ssl_config* config_;
    mbedtls_x509_crt* certificate_;
    mbedtls_pk_context* key_;
    mbedtls_ctr_drbg_context* rng_;
    std::vector<uint8_t> inbound_;
    size_t inboundOffset_ = 0;
    std::vector<uint8_t> outbound_;
    bool outboundHandshake_ = true;
    bool started_ = false;
    bool secure_ = false;
    bool failed_ = false;
    int lastError_ = 0;
};

}
