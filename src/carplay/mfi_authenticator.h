#pragma once

#include <cstdint>

#include "auth.h"

namespace carplay {

// The accessory identity the iPhone checks, read from files you supply (nothing is shipped):
//   <dir>/identity.pk8     private key, PKCS#8 DER, P-256
//   <dir>/certificate.p7b  its certificate, sent as stored
// Without both files the session ends at the phone's first authentication request.
class MfiAuthenticator final : public nbtplay::Authenticator {
public:
    ~MfiAuthenticator() override;

    // Reads and self-tests the identity. False (with status() saying why) if a
    // file is missing or the key is not a P-256 key.
    bool load(const char* directory);
    bool ready() const { return ready_; }
    const char* status() const { return status_; }

    const char* name() const override { return ready_ ? "files" : "none"; }
    nbtplay::AuthResult protocol_major(int& major) override;
    nbtplay::AuthResult certificate(nbtplay::Bytes& out) override;
    nbtplay::AuthResult sign_challenge(const uint8_t* challenge, size_t length,
                                       nbtplay::Bytes& out) override;

private:
    struct Key;
    Key* key_ = nullptr;
    nbtplay::Bytes certificate_;
    bool ready_ = false;
    int lastError_ = 0;
    char status_[96] = "identity not loaded";
};

} // namespace carplay
