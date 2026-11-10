#pragma once

#include <cstring>

#include "carplay/airplay_control.h"

#include "airplay.h"
#include "audio.h"
#include "crypto.h"
#include "events.h"
#include "media.h"
#include "pairing.h"
#include "rtsp.h"
#include "screen.h"
#include "timing.h"

namespace carplay {

using nbtplay::Bytes;

// Encrypted event channel: HID reports and iAP2 tunnel commands. nbtplay's
// EventChannel only exposes HID reports.
class EventLink {
public:
    void start(const uint8_t secret[32])
    {
        encrypted_.clear();
        plain_.clear();
        cseq_ = 0;
        uint8_t readKey[32], writeKey[32];
        nbtplay::hkdf_sha512(secret, 32, "Events-Salt", "Events-Read-Encryption-Key", readKey, 32);
        nbtplay::hkdf_sha512(secret, 32, "Events-Salt", "Events-Write-Encryption-Key", writeKey, 32);
        cipher_.set_keys(readKey, writeKey);
        ready_ = true;
    }
    bool ready() const { return ready_; }

    bool command(const nbtplay::Plist& body, Bytes& out)
    {
        Bytes encoded, wire;
        return ready_ && nbtplay::bplist_encode(body, encoded) &&
               nbtplay::rtsp_request("POST", "/command", ++cseq_, "application/x-apple-binary-plist",
                                     encoded.data(), encoded.size(), wire) &&
               cipher_.encrypt(wire.data(), wire.size(), out);
    }

    bool hidReport(const char* uid, const uint8_t* report, size_t length, Bytes& out)
    {
        nbtplay::Plist body = nbtplay::Plist::dict();
        body.set("type", nbtplay::Plist::string("hidSendReport"))
            .set("uuid", nbtplay::Plist::string(uid))
            .set("hidReport", nbtplay::Plist::data(report, length));
        return command(body, out);
    }

    bool iapMessage(const uint8_t* data, size_t length, Bytes& out)
    {
        nbtplay::Plist params = nbtplay::Plist::dict();
        params.set("data", nbtplay::Plist::data(data, length));
        nbtplay::Plist body = nbtplay::Plist::dict();
        body.set("type", nbtplay::Plist::string("iAPSendMessage")).set("params", nbtplay::move(params));
        return command(body, out);
    }

    // Answers each request with 200 and swallows responses to our commands.
    bool feed(const uint8_t* data, size_t length, Bytes& out)
    {
        if (!ready_ || !encrypted_.append(data, length) || !cipher_.decrypt(encrypted_, plain_))
            return false;
        for (;;) {
            nbtplay::RtspRequest message;
            size_t consumed = 0;
            const nbtplay::RtspParse state =
                nbtplay::rtsp_parse(plain_.data(), plain_.size(), message, consumed);
            if (state == nbtplay::RtspParse::Error)
                return false;
            if (state == nbtplay::RtspParse::NeedMore)
                return true;
            plain_.erase_front(consumed);
            if (std::memcmp(message.method, "RTSP/", 5) == 0 || std::memcmp(message.method, "HTTP/", 5) == 0)
                continue;
            logPlistBody("phone event", message.body.data(), message.body.size());
            Bytes wire;
            if (!nbtplay::rtsp_response(message, 200, nullptr, nullptr, 0, wire) ||
                !cipher_.encrypt(wire.data(), wire.size(), out))
                return false;
        }
    }

private:
    nbtplay::ControlCipher cipher_;
    Bytes encrypted_, plain_;
    unsigned cseq_ = 0;
    bool ready_ = false;
};

} // namespace carplay
