#pragma once

#include "auth.h"
#include "iap2_csm.h"
#include "rt.h"

namespace carplay::core {

constexpr uint16_t kMsgRequestCertificate = 0xAA00;
constexpr uint16_t kMsgAccessoryCertificate = 0xAA01;
constexpr uint16_t kMsgRequestChallengeResponse = 0xAA02;
constexpr uint16_t kMsgAuthenticationResponse = 0xAA03;
constexpr uint16_t kMsgAuthenticationFailed = 0xAA04;
constexpr uint16_t kMsgAuthenticationSucceeded = 0xAA05;

constexpr size_t kMaxChallengeBytes = 128;

class Iap2AuthExchange {
public:
    enum class Status { Idle, InProgress, Succeeded, Failed };

    explicit Iap2AuthExchange(Authenticator &authenticator) : auth_(authenticator) {}

    Status on_frame(const CsmFrame &frame, Bytes &out);

    Status status() const { return status_; }
    const char *failure() const { return failure_; }

private:
    Status fail(const char *reason);

    Authenticator &auth_;
    Status status_ = Status::Idle;
    const char *failure_ = nullptr;
};

} // namespace carplay::core
