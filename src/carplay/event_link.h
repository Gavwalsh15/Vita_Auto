#pragma once

#include <cstring>

#include "carplay/airplay_control.h"

#include "carplay/core/airplay.h"
#include "carplay/core/audio.h"
#include "carplay/core/crypto.h"
#include "carplay/core/events.h"
#include "carplay/core/media.h"
#include "carplay/core/pairing.h"
#include "carplay/core/rtsp.h"
#include "carplay/core/screen.h"
#include "carplay/core/timing.h"

namespace carplay {

using core::Bytes;

class EventLink {
public:
    void start(const uint8_t secret[32])
    {
        encrypted_.clear();
        plain_.clear();
        cseq_ = 0;
        uint8_t readKey[32], writeKey[32];
        core::hkdf_sha512(secret, 32, "Events-Salt", "Events-Read-Encryption-Key", readKey, 32);
        core::hkdf_sha512(secret, 32, "Events-Salt", "Events-Write-Encryption-Key", writeKey, 32);
        cipher_.set_keys(readKey, writeKey);
        ready_ = true;
    }
    bool ready() const { return ready_; }

    bool command(const core::Plist& body, Bytes& out)
    {
        Bytes encoded, wire;
        return ready_ && core::bplist_encode(body, encoded) &&
               core::rtsp_request("POST", "/command", ++cseq_, "application/x-apple-binary-plist",
                                     encoded.data(), encoded.size(), wire) &&
               cipher_.encrypt(wire.data(), wire.size(), out);
    }

    bool hidReport(const char* uid, const uint8_t* report, size_t length, Bytes& out)
    {
        core::Plist body = core::Plist::dict();
        body.set("type", core::Plist::string("hidSendReport"))
            .set("uuid", core::Plist::string(uid))
            .set("hidReport", core::Plist::data(report, length));
        return command(body, out);
    }

    bool iapMessage(const uint8_t* data, size_t length, Bytes& out)
    {
        core::Plist params = core::Plist::dict();
        params.set("data", core::Plist::data(data, length));
        core::Plist body = core::Plist::dict();
        body.set("type", core::Plist::string("iAPSendMessage")).set("params", std::move(params));
        return command(body, out);
    }

    // Answers each request with 200 and swallows responses to our commands.
    bool feed(const uint8_t* data, size_t length, Bytes& out)
    {
        if (!ready_ || !encrypted_.append(data, length) || !cipher_.decrypt(encrypted_, plain_))
            return false;
        for (;;) {
            core::RtspRequest message;
            size_t consumed = 0;
            const core::RtspParse state =
                core::rtsp_parse(plain_.data(), plain_.size(), message, consumed);
            if (state == core::RtspParse::Error)
                return false;
            if (state == core::RtspParse::NeedMore)
                return true;
            plain_.erase_front(consumed);
            if (std::memcmp(message.method, "RTSP/", 5) == 0 || std::memcmp(message.method, "HTTP/", 5) == 0)
                continue;
            logPlistBody("phone event", message.body.data(), message.body.size());
            Bytes wire;
            if (!core::rtsp_response(message, 200, nullptr, nullptr, 0, wire) ||
                !cipher_.encrypt(wire.data(), wire.size(), out))
                return false;
        }
    }

private:
    core::ControlCipher cipher_;
    Bytes encrypted_, plain_;
    unsigned cseq_ = 0;
    bool ready_ = false;
};

} // namespace carplay
