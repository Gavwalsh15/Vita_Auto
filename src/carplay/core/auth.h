#pragma once

#include "rt.h"

namespace carplay::core {

enum class AuthResult {
    Ok,
    Unavailable,
    Failed,
};

class Authenticator {
public:
    virtual ~Authenticator() = default;

    virtual const char *name() const = 0;
    virtual AuthResult protocol_major(int &major) = 0;
    virtual AuthResult certificate(Bytes &out) = 0;
    virtual AuthResult sign_challenge(const uint8_t *challenge, size_t length, Bytes &out) = 0;
};

} // namespace carplay::core
