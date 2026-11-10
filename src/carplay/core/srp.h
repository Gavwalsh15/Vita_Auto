#pragma once

#include "crypto.h"

namespace carplay::core {

constexpr size_t kSrpBytes = 384;
constexpr size_t kSrpSaltBytes = 16;

class SrpServer {
public:
    bool start(Random &random, const char *user, const char *password);
    const uint8_t *salt() const { return salt_; }
    const uint8_t *public_key() const { return b_public_; }
    bool verify(const uint8_t *a, size_t a_length, const uint8_t *proof, size_t proof_length,
                uint8_t server_proof[kSha512Bytes]);
    const uint8_t *session_key() const { return key_; }
    ~SrpServer();

private:
    uint8_t salt_[kSrpSaltBytes] = {};
    uint8_t user_hash_[kSha512Bytes] = {};
    uint8_t verifier_[kSrpBytes] = {};
    uint8_t b_private_[32] = {};
    uint8_t b_public_[kSrpBytes] = {};
    uint8_t key_[kSha512Bytes] = {};
};

} // namespace carplay::core
