#include "carplay/airplay_control.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "logger/vita_log.h"

#include "crypto.h"
#include "media.h"

namespace carplay {

const char HID_TOUCH[] = "2a2a2a2a";
const char HID_KNOB[] = "2a2a2a2b";
const char HID_MEDIA[] = "2a2a2a2c";
const char HID_TELEPHONY[] = "2a2a2a2d";

using namespace nbtplay;

namespace {

const char PLIST_TYPE[] = "application/x-apple-binary-plist";
const char PAIRING_TYPE[] = "application/pairing+tlv8";
const char OCTET_TYPE[] = "application/octet-stream";
const char MAIN_DISPLAY_UUID[] = "b7e6c5a0-1111-4000-8000-000000000001";

const int64_t DISPLAY_FEATURE_KNOBS = 0x02;
const int64_t DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH = 0x08;
const int64_t PRIMARY_INPUT_TOUCH = 1;

const uint8_t KNOB_DESCRIPTOR[] = {
    0x05, 0x01, 0x09, 0x08, 0xa1, 0x01,
    0x05, 0x09, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x0c, 0x0a, 0x23, 0x02, 0x0a, 0x24, 0x02, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0,
};

const uint8_t MEDIA_DESCRIPTOR[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
    0x15, 0x00, 0x25, 0x06, 0x05, 0x0c,
    0x0a, 0x00, 0x00, 0x0a, 0xb0, 0x00, 0x0a, 0xb1, 0x00, 0x0a, 0xcd, 0x00,
    0x0a, 0xb5, 0x00, 0x0a, 0xb6, 0x00, 0x0a, 0x9e, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
};

const uint8_t TELEPHONY_DESCRIPTOR[] = {
    0x05, 0x0b, 0x09, 0x07, 0xa1, 0x01,
    0x15, 0x00, 0x25, 0x11, 0x05, 0x0b,
    0x09, 0x00, 0x09, 0x20, 0x09, 0x21, 0x09, 0x26, 0x09, 0x2f,
    0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb2, 0x09, 0xb3, 0x09, 0xb4, 0x09, 0xb5,
    0x09, 0xb6, 0x09, 0xb7, 0x09, 0xb8, 0x09, 0xb9, 0x09, 0xba, 0x09, 0xbb,
    0x05, 0x07, 0x09, 0x2a,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
};

// A touch screen with TOUCH_CONTACTS fingers; each report slot is
// { contact id, tip switch, x (16 bit), y (16 bit) }.
size_t touchDescriptor(uint8_t* out, uint32_t xMax, uint32_t yMax)
{
    size_t n = 0;
    static const uint8_t head[] = {0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01};
    memcpy(out + n, head, sizeof(head));
    n += sizeof(head);
    for (uint32_t i = 0; i < TOUCH_CONTACTS; ++i) {
        const uint8_t finger[] = {
            0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02,
            0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
            0x15, 0x00, 0x25, 0x01, 0x09, 0x33, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
            0x95, 0x07, 0x81, 0x03,
            0x05, 0x01, 0x26, static_cast<uint8_t>(xMax & 0xff), static_cast<uint8_t>((xMax >> 8) & 0xff),
            0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
            0x26, static_cast<uint8_t>(yMax & 0xff), static_cast<uint8_t>((yMax >> 8) & 0xff),
            0x09, 0x31, 0x81, 0x02, 0xc0,
        };
        memcpy(out + n, finger, sizeof(finger));
        n += sizeof(finger);
    }
    out[n++] = 0xc0;
    return n;
}

Plist hidDevice(const char* uid, const char* name, const uint8_t* descriptor, size_t length)
{
    Plist entry = Plist::dict();
    entry.set("hidProductID", Plist::integer(1))
        .set("hidVendorID", Plist::integer(2))
        .set("hidCountryCode", Plist::integer(0))
        .set("uuid", Plist::string(uid))
        .set("name", Plist::string(name))
        .set("displayUUID", Plist::string(MAIN_DISPLAY_UUID))
        .set("hidDescriptor", Plist::data(descriptor, length));
    return entry;
}

Plist resource(int64_t id)
{
    Plist entry = Plist::dict();
    entry.set("resourceID", Plist::integer(id))
        .set("transferType", Plist::integer(1))
        .set("transferPriority", Plist::integer(100))
        .set("takeConstraint", Plist::integer(100))
        .set("borrowConstraint", Plist::integer(100))
        .set("unborrowConstraint", Plist::integer(100));
    return entry;
}

Plist appState(int64_t id, const char* key, Plist&& value)
{
    Plist entry = Plist::dict();
    entry.set("appStateID", Plist::integer(id)).set(key, move(value));
    return entry;
}

Plist modes()
{
    Plist resources = Plist::array();
    resources.push(resource(1)).push(resource(2)); // screen, audio
    Plist states = Plist::array();
    states.push(appState(2, "state", Plist::boolean(false)))
        .push(appState(1, "speechMode", Plist::integer(-1)))
        .push(appState(3, "state", Plist::boolean(false)));
    Plist entry = Plist::dict();
    entry.set("resources", move(resources)).set("appStates", move(states));
    return entry;
}

Plist latency(int64_t type, const char* audioType)
{
    Plist entry = Plist::dict();
    entry.set("type", Plist::integer(type))
        .set("inputLatencyMicros", Plist::integer(0))
        .set("outputLatencyMicros", Plist::integer(0));
    if (audioType != nullptr)
        entry.set("audioType", Plist::string(audioType));
    return entry;
}

Plist audioLatencies()
{
    Plist list = Plist::array();
    list.push(latency(100, nullptr)).push(latency(100, "default")).push(latency(100, "media"))
        .push(latency(100, "telephony")).push(latency(100, "speechRecognition"))
        .push(latency(100, "alert")).push(latency(101, nullptr)).push(latency(101, "default"))
        .push(latency(102, "default"));
    return list;
}

Plist format(int64_t type, const char* audioType, int64_t outputFormats)
{
    Plist entry = Plist::dict();
    entry.set("type", Plist::integer(type))
        .set("audioType", Plist::string(audioType))
        .set("audioOutputFormats", Plist::integer(outputFormats));
    return entry;
}

// PCM plus AAC-LC (48 kHz) for stream 102. No Opus: nothing here decodes it.
Plist audioFormats()
{
    const int64_t aacLc = 0x800000;
    const int64_t pcm = 0x3fc | 0xc000;
    const int64_t pcmMono = 0x154 | 0x4000;
    Plist list = Plist::array();
    list.push(format(100, "compatibility", pcm))
        .push(format(101, "compatibility", pcm))
        .push(format(100, "default", pcm))
        .push(format(100, "alert", pcm))
        .push(format(100, "media", pcm))
        .push(format(100, "telephony", pcmMono))
        .push(format(100, "speechRecognition", pcmMono))
        .push(format(101, "default", pcm))
        .push(format(102, "media", aacLc));
    return list;
}

Plist area(int64_t width, int64_t height)
{
    Plist safe = Plist::dict();
    safe.set("widthPixels", Plist::integer(width))
        .set("heightPixels", Plist::integer(height))
        .set("originXPixels", Plist::integer(0))
        .set("originYPixels", Plist::integer(0))
        .set("drawUIOutsideSafeArea", Plist::boolean(true));
    Plist view = Plist::dict();
    view.set("widthPixels", Plist::integer(width))
        .set("heightPixels", Plist::integer(height))
        .set("originXPixels", Plist::integer(0))
        .set("originYPixels", Plist::integer(0))
        .set("safeArea", move(safe));
    return view;
}

int saneFps(int fps)
{
    if (fps < 30)
        fps = 30;
    if (fps > 60)
        fps = 60;
    return ((fps - 30) + 2) / 5 * 5 + 30;
}

Plist mainDisplay(const InfoConfig& c)
{
    const int64_t heightMm = (static_cast<int64_t>(c.widthMm) * c.height + c.width / 2) / c.width;
    Plist areas = Plist::array();
    areas.push(area(c.width, c.height));
    Plist entry = Plist::dict();
    entry.set("uuid", Plist::string(MAIN_DISPLAY_UUID))
        .set("type", Plist::integer(kStreamMainScreen))
        .set("maxFPS", Plist::integer(saneFps(static_cast<int>(c.maxFps))))
        .set("widthPixels", Plist::integer(c.width))
        .set("heightPixels", Plist::integer(c.height))
        .set("widthPhysical", Plist::integer(c.widthMm))
        .set("heightPhysical", Plist::integer(heightMm))
        .set("features", Plist::integer(DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH | DISPLAY_FEATURE_KNOBS))
        .set("primaryInputDevice", Plist::integer(PRIMARY_INPUT_TOUCH))
        .set("uiAppearanceMode", Plist::integer(0))
        .set("uiAppearanceSetting", Plist::integer(0))
        .set("mapAppearanceMode", Plist::integer(0))
        .set("mapAppearanceSetting", Plist::integer(0))
        .set("viewAreas", move(areas))
        .set("initialViewArea", Plist::integer(0));
    return entry;
}

int streamBit(int type)
{
    switch (type) {
    case kStreamMainAudio: return 0;
    case kStreamAltAudio: return 1;
    case kStreamMainHighAudio: return 2;
    case kStreamMainScreen: return 3;
    case kStreamAltScreen: return 4;
    default: return -1;
    }
}

bool streamType(const Plist& stream, int& type)
{
    int64_t value;
    const Plist* field = stream.get("type");
    if (field == nullptr || !field->to_int(value) || value < 0 || value > 0xffff)
        return false;
    type = static_cast<int>(value);
    return true;
}

bool encodeReply(const Plist& value, Bytes& body) { return bplist_encode(value, body); }

} // namespace

Plist buildInfo(const InfoConfig& c)
{
    Plist info = Plist::dict();
    info.set("sourceVersion", Plist::string(c.sourceVersion))
        .set("features", Plist::integer(CARPLAY_FEATURES))
        .set("statusFlags", Plist::integer(4))
        .set("model", Plist::string(c.model))
        .set("manufacturer", Plist::string(c.manufacturer))
        .set("deviceID", Plist::string(c.deviceId));
    Plist bluetooth = Plist::array();
    if (c.bluetoothMac[0] != '\0')
        bluetooth.push(Plist::string(c.bluetoothMac));
    info.set("bluetoothIDs", move(bluetooth))
        .set("name", Plist::string(c.name))
        .set("rightHandDrive", Plist::boolean(false))
        .set("keepAliveLowPower", Plist::boolean(false))
        .set("keepAliveSendStatsAsBody", Plist::boolean(false))
        .set("modes", modes())
        .set("audioLatencies", audioLatencies())
        .set("audioFormats", audioFormats());
    Plist extended = Plist::array();
    extended.push(Plist::string("vocoderInfo")).push(Plist::string("enhancedRequestCarUI"));
    Plist displays = Plist::array();
    displays.push(mainDisplay(c));
    uint8_t touch[256];
    const size_t touchLength = touchDescriptor(touch, c.width, c.height);
    Plist hid = Plist::array();
    hid.push(hidDevice(HID_TOUCH, "Vita Auto Touchscreen", touch, touchLength))
        .push(hidDevice(HID_KNOB, "Vita Auto Knob", KNOB_DESCRIPTOR, sizeof KNOB_DESCRIPTOR))
        .push(hidDevice(HID_MEDIA, "Vita Auto Media", MEDIA_DESCRIPTOR, sizeof MEDIA_DESCRIPTOR))
        .push(hidDevice(HID_TELEPHONY, "Vita Auto Telephony", TELEPHONY_DESCRIPTOR,
                        sizeof TELEPHONY_DESCRIPTOR));
    info.set("extendedFeatures", move(extended)).set("displays", move(displays)).set("hidDevices", move(hid));
    return info;
}

namespace {

void appendText(char* out, size_t cap, size_t& at, const char* format, ...)
    __attribute__((format(printf, 4, 5)));

void appendText(char* out, size_t cap, size_t& at, const char* format, ...)
{
    if (at + 1 >= cap)
        return;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + at, cap - at, format, args);
    va_end(args);
    if (written > 0)
        at += static_cast<size_t>(written) < cap - at ? static_cast<size_t>(written) : cap - at - 1;
}

void describeInto(const Plist& value, char* out, size_t cap, size_t& at, int depth)
{
    switch (value.type()) {
    case Plist::Type::Null: appendText(out, cap, at, "null"); break;
    case Plist::Type::Bool: appendText(out, cap, at, value.as_bool() ? "true" : "false"); break;
    case Plist::Type::Int: appendText(out, cap, at, "%lld", static_cast<long long>(value.as_int())); break;
    case Plist::Type::Real: appendText(out, cap, at, "%g", value.as_real()); break;
    case Plist::Type::String: appendText(out, cap, at, "\"%s\"", value.as_string()); break;
    case Plist::Type::Data: appendText(out, cap, at, "<%u bytes>", static_cast<unsigned>(value.byte_count())); break;
    case Plist::Type::Array:
    case Plist::Type::Dict: {
        const bool dict = value.type() == Plist::Type::Dict;
        appendText(out, cap, at, dict ? "{" : "[");
        if (depth >= 4) {
            appendText(out, cap, at, "...");
        } else {
            for (size_t i = 0; i < value.size() && at + 1 < cap; ++i) {
                if (i > 0)
                    appendText(out, cap, at, ", ");
                if (dict)
                    appendText(out, cap, at, "%s=", value.key(i));
                describeInto(value.at(i), out, cap, at, depth + 1);
            }
        }
        appendText(out, cap, at, dict ? "}" : "]");
        break;
    }
    }
}

} // namespace

void describePlist(const Plist& value, char* out, size_t cap)
{
    size_t at = 0;
    if (cap == 0)
        return;
    out[0] = 0;
    describeInto(value, out, cap, at, 0);
}

// The log keeps about 250 characters a line, so a long plist is written as several.
static void logChunked(const char* what, const char* text)
{
    const size_t chunk = 170;
    const size_t length = strlen(text);
    for (size_t at = 0, part = 0; at < length || part == 0; at += chunk, ++part) {
        char piece[chunk + 1];
        const size_t take = length - at < chunk ? length - at : chunk;
        memcpy(piece, text + at, take);
        piece[take] = 0;
        vitaLog("carplay-airplay: %s [%u]: %s", what, static_cast<unsigned>(part), piece);
        if (take == 0)
            break;
    }
}

void logPlistBody(const char* what, const uint8_t* data, size_t size)
{
    if (!VITA_AUTO_VERBOSE_LOGGING)
        return;
    Plist plist;
    if (size == 0 || !bplist_decode(data, size, plist)) {
        vitaLog("carplay-airplay: %s: %u bytes, not a plist", what, static_cast<unsigned>(size));
        return;
    }
    static char text[2048];
    describePlist(plist, text, sizeof(text));
    logChunked(what, text);
}

bool Control::streamOpen(int type) const
{
    const int bit = streamBit(type);
    return bit >= 0 && (openStreams_ >> bit & 1);
}

void Control::markStream(int type, bool open)
{
    const int bit = streamBit(type);
    if (bit < 0)
        return;
    if (open)
        openStreams_ |= 1u << bit;
    else
        openStreams_ &= ~(1u << bit);
}

bool Control::feed(const uint8_t* data, size_t length, Bytes& out)
{
    if (security_.channel_encrypted()) {
        Bytes plain;
        if (!encrypted_.append(data, length) || !security_.decrypt(encrypted_, plain))
            return false;
        if (!pending_.append(plain.data(), plain.size()))
            return false;
    } else if (!pending_.append(data, length)) {
        return false;
    }
    for (;;) {
        RtspRequest request;
        size_t consumed = 0;
        const RtspParse parsed = rtsp_parse(pending_.data(), pending_.size(), request, consumed);
        if (parsed == RtspParse::Error)
            return false;
        if (parsed == RtspParse::NeedMore)
            return true;
        Bytes rest;
        if (!rest.assign(pending_.data() + consumed, pending_.size() - consumed))
            return false;
        pending_ = move(rest);

        // The response that completes pair-verify still goes out in the clear;
        // encryption starts with the next request.
        const bool encrypted = security_.channel_encrypted();
        Bytes body, wire;
        const char* contentType = nullptr;
        const int status = handle(request, body, contentType);
        vitaLogInfo("carplay-airplay: %s %s -> %d (%u in, %u out)%s", request.method, request.path, status,
                static_cast<unsigned>(request.body.size()), static_cast<unsigned>(body.size()),
                encrypted ? " [enc]" : "");
        if (!rtsp_response(request, status, contentType, body.data(), body.size(), wire))
            return false;
        if (!encrypted) {
            if (!out.append(wire.data(), wire.size()))
                return false;
        } else if (!security_.encrypt(wire.data(), wire.size(), out)) {
            return false;
        }
    }
}

int Control::handle(const RtspRequest& request, Bytes& body, const char*& contentType)
{
    ++requests_;
    const size_t length = min_of(strlen(request.path), sizeof lastPath_ - 1);
    memcpy(lastPath_, request.path, length);
    lastPath_[length] = 0;
    contentType = nullptr;
    const char* method = request.method;

    if (strcmp(method, "SETUP") == 0) {
        const int status = handleSetup(request, body);
        if (status == 200)
            contentType = PLIST_TYPE;
        return status;
    }
    if (strcmp(method, "RECORD") == 0) {
        media_.session_active();
        return 200;
    }
    if (strcmp(method, "TEARDOWN") == 0)
        return handleTeardown(request);
    if (strcmp(method, "SET_PARAMETER") == 0)
        return 200;

    if (request.path_ends_with("/pair-setup") || request.path_ends_with("/pair-verify")) {
        const bool setup = request.path_ends_with("/pair-setup");
        const int status = setup ? security_.pair_setup(request.body, body)
                                 : security_.pair_verify(request.body, body);
        if (status == 200)
            contentType = PAIRING_TYPE;
        else
            body.clear();
        return status;
    }
    if (request.path_ends_with("/auth-setup")) {
        const int status = security_.auth_setup(request.body, body);
        if (status == 200)
            contentType = OCTET_TYPE;
        else
            body.clear();
        return status;
    }
    if (request.path_ends_with("/info")) {
        if (!encodeReply(buildInfo(config_), body))
            return 500;
        contentType = PLIST_TYPE;
        return 200;
    }
    if (request.path_ends_with("/command")) {
        logPlistBody("phone command", request.body.data(), request.body.size());
        Plist command;
        if (bplist_decode(request.body.data(), request.body.size(), command)) {
            const Plist* type = command.get("type");
            if (type != nullptr && type->is(Plist::Type::String) &&
                strcmp(type->as_string(), "disableBluetooth") == 0)
                bluetoothDisableRequested_ = true;
        }
    }
    return 200; // /command and /feedback are accepted
}

int Control::handleSetup(const RtspRequest& request, Bytes& body)
{
    Plist setup;
    if (!bplist_decode(request.body.data(), request.body.size(), setup) || !setup.is(Plist::Type::Dict))
        return 400;
    {
        static char text[2048];
        describePlist(setup, text, sizeof(text));
        logChunked("SETUP request", text);
    }
    Plist reply = Plist::dict();
    const Plist* streams = setup.get("streams");
    if (streams != nullptr && streams->is(Plist::Type::Array)) {
        Plist accepted = Plist::array();
        for (size_t i = 0; i < streams->size(); ++i) {
            const Plist& stream = streams->at(i);
            int type;
            if (!stream.is(Plist::Type::Dict) || !streamType(stream, type))
                continue;
            Plist entry = Plist::dict();
            uint32_t dataPort = 0, controlPort = 0;
            if (type == kStreamMainScreen || type == kStreamAltScreen) {
                if (!media_.open_screen(type, stream, dataPort))
                    continue;
                entry.set("type", Plist::integer(type)).set("dataPort", Plist::integer(dataPort));
            } else if (type == kStreamMainAudio || type == kStreamAltAudio || type == kStreamMainHighAudio) {
                if (!media_.open_audio(type, stream, dataPort, controlPort))
                    continue;
                entry.set("type", Plist::integer(type))
                    .set("dataPort", Plist::integer(dataPort))
                    .set("controlPort", Plist::integer(controlPort));
                int64_t connection;
                const Plist* id = stream.get("streamConnectionID");
                entry.set("streamConnectionID",
                          Plist::integer(id != nullptr && id->to_int(connection) ? connection : 0));
            } else if (type == kStreamData) {
                int64_t seed = 0;
                const Plist* field = stream.get("seed");
                if (field == nullptr || !field->to_int(seed)) {
                    vitaLog("carplay-airplay: iAP2 data stream without a seed");
                    continue;
                }
                if (!tunnel_.openTunnel(static_cast<uint64_t>(seed), dataPort))
                    continue;
                tunnelOpen_ = true;
                entry.set("type", Plist::integer(kStreamData))
                    .set("streamID", Plist::integer(1))
                    .set("dataPort", Plist::integer(dataPort));
                int64_t connection;
                const Plist* id = stream.get("streamConnectionID");
                if (id != nullptr && id->to_int(connection))
                    entry.set("streamConnectionID", Plist::integer(connection));
                accepted.push(move(entry));
                continue;
            } else {
                vitaLog("carplay-airplay: stream type %d not supported", type);
                continue;
            }
            markStream(type, true);
            accepted.push(move(entry));
        }
        reply.set("streams", move(accepted));
        return encodeReply(reply, body) ? 200 : 500;
    }

    int64_t peerTiming = 0;
    const Plist* timing = setup.get("timingPort");
    if (timing == nullptr || !timing->to_int(peerTiming) || peerTiming < 0 || peerTiming > 65535)
        peerTiming = 0;
    uint32_t timingPort = 0, eventPort = 0;
    if (!media_.open_session(static_cast<uint32_t>(peerTiming), timingPort, eventPort))
        return 500;
    // iAPChannel makes the phone open the iAP2 data stream (type 130), the second
    // iAP2 exchange that wireless CarPlay runs over Wi-Fi. DiPlay enables it;
    // without it the phone never opens that stream.
    Plist features = Plist::array();
    features.push(Plist::string("iAPChannel")).push(Plist::string("viewAreas"));
    reply.set("timingPort", Plist::integer(timingPort)).set("eventPort", Plist::integer(eventPort));
    // A phone that asks for low-power keep-alive wants a port to send its datagrams to.
    const Plist* lowPower = setup.get("keepAliveLowPower");
    uint32_t keepAlivePort = 0;
    if (lowPower != nullptr && lowPower->is(Plist::Type::Bool) && lowPower->as_bool() &&
        tunnel_.openKeepAlive(keepAlivePort))
        reply.set("keepAlivePort", Plist::integer(keepAlivePort));
    reply.set("enabledFeatures", move(features));
    return encodeReply(reply, body) ? 200 : 500;
}

int Control::handleTeardown(const RtspRequest& request)
{
    Plist teardown;
    bool named = false;
    if (bplist_decode(request.body.data(), request.body.size(), teardown)) {
        const Plist* streams = teardown.get("streams");
        for (size_t i = 0; streams != nullptr && streams->is(Plist::Type::Array) && i < streams->size(); ++i) {
            int type;
            if (!streams->at(i).is(Plist::Type::Dict) || !streamType(streams->at(i), type))
                continue;
            named = true;
            if (type == kStreamData && tunnelOpen_) {
                tunnelOpen_ = false;
                tunnel_.closeTunnel();
            } else if (streamOpen(type)) {
                markStream(type, false);
                media_.close_stream(type);
            }
        }
    }
    if (!named) { // no streams named: the whole session ends
        const int all[] = {kStreamMainAudio, kStreamAltAudio, kStreamMainHighAudio, kStreamMainScreen,
                           kStreamAltScreen};
        for (int type : all) {
            if (streamOpen(type)) {
                markStream(type, false);
                media_.close_stream(type);
            }
        }
        if (tunnelOpen_) {
            tunnelOpen_ = false;
            tunnel_.closeTunnel();
        }
    }
    return 200;
}

} // namespace carplay
