#include "iap2_auth.h"

namespace carplay::core {

namespace {

bool emit(Bytes &out, uint16_t message_id, const uint8_t *payload, size_t length) {
    CsmBuilder builder(message_id);
    if (!builder.add(0, payload, length)) return false;
    Bytes frame;
    if (!builder.finish(frame)) return false;
    return out.append(frame.data(), frame.size());
}

} // namespace

Iap2AuthExchange::Status Iap2AuthExchange::fail(const char *reason) {
    status_ = Status::Failed;
    failure_ = reason;
    return status_;
}

Iap2AuthExchange::Status Iap2AuthExchange::on_frame(const CsmFrame &frame, Bytes &out) {
    if (status_ == Status::Failed || status_ == Status::Succeeded) return status_;

    switch (frame.message_id) {
    case kMsgRequestCertificate: {
        status_ = Status::InProgress;
        Bytes certificate;
        AuthResult result = auth_.certificate(certificate);
        if (result == AuthResult::Unavailable) return fail("no authenticator configured");
        if (result != AuthResult::Ok || certificate.empty()) return fail("certificate unavailable");
        if (!emit(out, kMsgAccessoryCertificate, certificate.data(), certificate.size()))
            return fail("could not build certificate reply");
        return status_;
    }
    case kMsgRequestChallengeResponse: {
        status_ = Status::InProgress;
        CsmParam challenge;
        if (!frame.find(0, challenge) || challenge.length == 0 || challenge.length > kMaxChallengeBytes)
            return fail("invalid challenge");
        Bytes signature;
        AuthResult result = auth_.sign_challenge(challenge.payload, challenge.length, signature);
        if (result == AuthResult::Unavailable) return fail("no authenticator configured");
        if (result != AuthResult::Ok || signature.empty()) return fail("signing failed");
        if (!emit(out, kMsgAuthenticationResponse, signature.data(), signature.size()))
            return fail("could not build signature reply");
        return status_;
    }
    case kMsgAuthenticationSucceeded:
        status_ = Status::Succeeded;
        return status_;
    case kMsgAuthenticationFailed:
        return fail("iPhone reported authentication failed");
    default:
        return status_;
    }
}

} // namespace carplay::core
