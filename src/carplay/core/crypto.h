#pragma once

#include "rt.h"

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

namespace carplay::core {

constexpr size_t kSha512Bytes = 64;

class Sha512 {
public:
    Sha512();
    ~Sha512();
    Sha512(const Sha512 &) = delete;
    Sha512 &operator=(const Sha512 &) = delete;
    void update(const uint8_t *data, size_t length);
    void update(const char *text) { update(reinterpret_cast<const uint8_t *>(text), strlen(text)); }
    void finish(uint8_t digest[kSha512Bytes]);

private:
    mbedtls_sha512_context context_;
};

void sha512(const uint8_t *data, size_t length, uint8_t digest[kSha512Bytes]);
void hkdf_sha512(const uint8_t *ikm, size_t ikm_length, const char *salt, const char *info, uint8_t *out,
                 size_t length);

constexpr size_t kAeadTagBytes = 16;
bool aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_length,
               const uint8_t *plain, size_t length, Bytes &out);
bool aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_length,
               const uint8_t *sealed, size_t length, Bytes &out);
// AirPlay nonces: four zero bytes, then an 8-byte label ("PV-Msg02") or a little-endian counter.
void nonce_label(const char *label, uint8_t nonce[12]);
void nonce_counter(uint64_t counter, uint8_t nonce[12]);

constexpr size_t kSha1Bytes = 20, kSha256Bytes = 32;

class Sha1 {
public:
    Sha1();
    ~Sha1();
    Sha1(const Sha1 &) = delete;
    Sha1 &operator=(const Sha1 &) = delete;
    void update(const uint8_t *data, size_t length);
    void update(const char *text) { update(reinterpret_cast<const uint8_t *>(text), strlen(text)); }
    void finish(uint8_t digest[kSha1Bytes]);

private:
    mbedtls_sha1_context context_;
};

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    void update(const uint8_t *data, size_t length);
    void update(const char *text) { update(reinterpret_cast<const uint8_t *>(text), strlen(text)); }
    void finish(uint8_t digest[kSha256Bytes]);

private:
    mbedtls_sha256_context context_;
};

// AES-128 in CTR mode with a big-endian 128-bit counter, as MFiSAP uses it.
void aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t length, uint8_t *out);

bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public(uint8_t out[32], const uint8_t scalar[32]);

void ed25519_public(uint8_t public_key[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const uint8_t *message, size_t length);
bool ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32], const uint8_t *message,
                    size_t length);

bool equal_bytes(const uint8_t *a, const uint8_t *b, size_t length);

class Random {
public:
    virtual ~Random() = default;
    virtual bool fill(uint8_t *out, size_t length) = 0;
};

} // namespace carplay::core
