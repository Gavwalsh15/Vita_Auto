#include "carplay/bonjour.h"

#include "logger/vita_log.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace carplay {
namespace {

constexpr uint16_t MDNS_PORT = 5353;
constexpr uint64_t FAST_QUERY_US = 2000000;
constexpr uint64_t SLOW_QUERY_US = 10000000;
constexpr uint64_t FAST_WINDOW_US = 60000000;
constexpr uint64_t INSTANCE_QUERY_US = 1000000;
constexpr uint32_t ANNOUNCES = 3;
constexpr int PROBE_ATTEMPTS = 7;
constexpr uint32_t PROBE_RETRY_MS = 1500;
constexpr uint32_t PROBE_STEP_MS = 100;
constexpr uint32_t CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t REPLY_TIMEOUT_MS = 3000;
constexpr int PROBE_THREAD_PRIORITY = 0x10000100;
constexpr int PROBE_THREAD_STACK = 0x8000;
constexpr size_t NAME_MAX_WIRE = 255;
constexpr uint32_t TTL_HOST = 120;
constexpr uint32_t TTL_OTHER = 4500;

enum RecordType : uint16_t { TypeA = 1, TypePtr = 12, TypeTxt = 16, TypeSrv = 33, TypeAny = 255 };

const uint8_t CONTROL_SERVICE[] = {13, '_', 'c', 'a', 'r', 'p', 'l', 'a', 'y', '-', 'c', 't', 'r', 'l',
                                   4,  '_', 't', 'c', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0};
const uint8_t AIRPLAY_SERVICE[] = {8, '_', 'a', 'i', 'r', 'p', 'l', 'a', 'y',
                                   4, '_', 't', 'c', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0};
const uint8_t LOCAL_DOMAIN[] = {5, 'l', 'o', 'c', 'a', 'l', 0};

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

void putBe16(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

void putBe32(uint8_t* p, uint32_t value)
{
    putBe16(p, static_cast<uint16_t>(value >> 16));
    putBe16(p + 2, static_cast<uint16_t>(value));
}

bool sameName(const uint8_t* a, size_t aLength, const uint8_t* b, size_t bLength)
{
    if (aLength != bLength)
        return false;
    for (size_t i = 0; i < aLength; ++i)
        if (std::tolower(a[i]) != std::tolower(b[i]))
            return false;
    return true;
}

bool isControlInstance(const uint8_t* name, size_t length)
{
    const size_t tail = sizeof(CONTROL_SERVICE);
    return length > tail && sameName(name + length - tail, tail, CONTROL_SERVICE, tail);
}

size_t labelAndDomain(uint8_t* out, const char* label, const uint8_t* suffix, size_t suffixLength)
{
    const size_t length = std::strlen(label);
    out[0] = static_cast<uint8_t>(length);
    std::memcpy(out + 1, label, length);
    std::memcpy(out + 1 + length, suffix, suffixLength);
    return 1 + length + suffixLength;
}

// A name at `offset` (labels, possibly ending in a compression pointer), written
// out uncompressed. `next` is where the record carrying the name continues.
bool readName(const uint8_t* packet, size_t length, size_t offset, uint8_t* out, size_t& outLength, size_t& next)
{
    outLength = 0;
    next = 0;
    bool jumped = false;
    int hops = 0;
    for (;;) {
        if (offset >= length)
            return false;
        const uint8_t c = packet[offset];
        if ((c & 0xc0) == 0xc0) {
            if (offset + 1 >= length)
                return false;
            const size_t target = static_cast<size_t>(((c & 0x3f) << 8) | packet[offset + 1]);
            if (!jumped)
                next = offset + 2;
            jumped = true;
            if (++hops > 16 || target >= length)
                return false;
            offset = target;
            continue;
        }
        if (c & 0xc0)
            return false;
        if (c == 0) {
            if (outLength + 1 > NAME_MAX_WIRE)
                return false;
            out[outLength++] = 0;
            if (!jumped)
                next = offset + 1;
            return true;
        }
        if (offset + 1 + c > length || outLength + 1 + c >= NAME_MAX_WIRE)
            return false;
        std::memcpy(out + outLength, packet + offset, 1u + c);
        outLength += 1u + c;
        offset += 1u + c;
    }
}

void describePacket(const uint8_t* p, size_t length, char* out, size_t capacity)
{
    size_t used = 0;
    out[0] = '\0';
    auto add = [&](char tag, const uint8_t* name, size_t nameLength, unsigned type) {
        char text[96];
        size_t t = 0;
        for (size_t i = 0; i < nameLength && name[i] != 0 && t + 2 < sizeof(text);) {
            const size_t label = name[i++];
            for (size_t j = 0; j < label && i < nameLength && t + 2 < sizeof(text); ++j, ++i)
                text[t++] = static_cast<char>(name[i] >= 32 && name[i] < 127 ? name[i] : '?');
            text[t++] = '.';
        }
        text[t] = '\0';
        const int n = std::snprintf(out + used, capacity - used, "%c %s/%u ", tag, text, type);
        if (n > 0)
            used = std::min(capacity - 1, used + static_cast<size_t>(n));
    };
    const unsigned questions = be16(p + 4);
    const unsigned records = be16(p + 6) + be16(p + 8) + be16(p + 10);
    size_t at = 12;
    uint8_t name[NAME_MAX_WIRE + 1];
    size_t nameLength = 0, next = 0;
    for (unsigned i = 0; i < questions && i < 8; ++i) {
        if (!readName(p, length, at, name, nameLength, next) || next + 4 > length)
            return;
        add('q', name, nameLength, be16(p + next));
        at = next + 4;
    }
    for (unsigned i = 0; i < records && i < 12; ++i) {
        if (!readName(p, length, at, name, nameLength, next) || next + 10 > length)
            return;
        add('a', name, nameLength, be16(p + next));
        at = next + 10 + be16(p + next + 8);
        if (at > length)
            return;
    }
}

// The device id as the number the phone expects in AirPlay-Receiver-Device-ID.
unsigned long long deviceNumber(const char* deviceId)
{
    unsigned long long value = 0;
    for (const char* p = deviceId; *p != '\0'; ++p) {
        if (*p == ':')
            continue;
        const int digit = (*p >= '0' && *p <= '9')   ? *p - '0'
                          : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                          : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10
                                                     : -1;
        if (digit < 0)
            break;
        value = (value << 4) | static_cast<unsigned>(digit);
    }
    return value;
}

// Waits for the socket in short steps, so a stop request is noticed at once.
bool waitSocket(int epoll, uint32_t events, uint32_t timeoutMs, const volatile uint32_t& stop)
{
    for (uint32_t waited = 0; waited < timeoutMs && !stop; waited += PROBE_STEP_MS) {
        SceNetEpollEvent got[1];
        const int count = sceNetEpollWait(epoll, got, 1, PROBE_STEP_MS * 1000);
        if (count > 0)
            return (got[0].events & (events | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) != 0;
    }
    return false;
}

void multicastGroup(SceNetSockaddrIn& to)
{
    to = SceNetSockaddrIn{};
    to.sin_len = sizeof(to);
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(MDNS_PORT);
    sceNetInetPton(SCE_NET_AF_INET, "224.0.0.251", &to.sin_addr);
}

} // namespace

bool Bonjour::begin(const char* localIp, const Service& service)
{
    end();
    SceNetInAddr address{};
    if (sceNetInetPton(SCE_NET_AF_INET, localIp, &address) != 1) {
        vitaLog("carplay-ctrl: '%s' is no IPv4 address", localIp);
        return false;
    }
    local_ = address.s_addr;
    service_ = service;

    instanceNameLength_ = labelAndDomain(instanceName_, service_.name, AIRPLAY_SERVICE, sizeof(AIRPLAY_SERVICE));
    char host[24] = "carplay-";
    size_t length = std::strlen(host);
    for (const char* p = service_.deviceId; *p != '\0' && length < sizeof(host) - 1; ++p)
        if (*p != ':')
            host[length++] = static_cast<char>(std::tolower(*p));
    host[length] = '\0';
    hostNameLength_ = labelAndDomain(hostName_, host, LOCAL_DOMAIN, sizeof(LOCAL_DOMAIN));

    // The TXT record: length-prefixed "key=value" strings. The features are written
    // as the low and high 32 bits, as Bonjour receivers do.
    char features[24];
    std::snprintf(features, sizeof(features), "0x%x,0x%x", static_cast<unsigned>(service_.features & 0xffffffffu),
                  static_cast<unsigned>(service_.features >> 32));
    const char* const entries[][2] = {{"deviceid", service_.deviceId}, {"features", features},
                                      {"flags", "0x4"},                {"model", service_.model},
                                      {"srcvers", service_.sourceVersion}, {"protovers", "1.1"},
                                      {"pi", service_.pairingId},      {"pk", service_.publicKey}};
    txtLength_ = 0;
    for (const auto& entry : entries) {
        const size_t size = std::strlen(entry[0]) + 1 + std::strlen(entry[1]);
        if (size > 255 || txtLength_ + 1 + size > sizeof(txt_))
            continue;
        txt_[txtLength_++] = static_cast<uint8_t>(size);
        txtLength_ += static_cast<size_t>(std::snprintf(reinterpret_cast<char*>(txt_ + txtLength_), size + 1,
                                                        "%s=%s", entry[0], entry[1]));
    }

    begun_ = true;
    return true;
}

void Bonjour::end()
{
    if (!begun_)
        return;
    __atomic_store_n(&stop_, 1u, __ATOMIC_RELEASE);
    joinProbe();
    closeSockets();
    begun_ = false;
    armed_ = false;
    __atomic_store_n(&stop_, 0u, __ATOMIC_RELEASE);
}

void Bonjour::openSockets()
{
    if (querySocket_ >= 0 || listenSocket_ >= 0)
        return;
    const int one = 1;
    // Questions leave from an ordinary port: a responder answers such a "legacy"
    // query straight back to it, which needs no multicast group on this side.
    int sock = sceNetSocket("carplay_mdns_q", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (sock >= 0) {
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
        SceNetInAddr outgoing{};
        outgoing.s_addr = local_;
        const int ifResult =
            sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP, SCE_NET_IP_MULTICAST_IF, &outgoing, sizeof(outgoing));
        SceNetSockaddrIn any{};
        any.sin_len = sizeof(any);
        any.sin_family = SCE_NET_AF_INET;
        const int bound = sceNetBind(sock, reinterpret_cast<SceNetSockaddr*>(&any), sizeof(any));
        if (bound < 0) {
            vitaLog("carplay-ctrl: query socket bind failed 0x%08x", static_cast<unsigned>(bound));
            sceNetSocketClose(sock);
            sock = -1;
        } else if (ifResult < 0) {
            vitaLog("carplay-ctrl: multicast interface not set (0x%08x)", static_cast<unsigned>(ifResult));
        }
    } else {
        vitaLog("carplay-ctrl: query socket failed 0x%08x", static_cast<unsigned>(sock));
    }
    querySocket_ = sock;

    // The multicast group: the phone's questions and announcements, and where ours go.
    sock = sceNetSocket("carplay_mdns_l", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (sock >= 0) {
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEPORT, &one, sizeof(one));
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
        SceNetSockaddrIn any{};
        any.sin_len = sizeof(any);
        any.sin_family = SCE_NET_AF_INET;
        any.sin_port = sceNetHtons(MDNS_PORT);
        int result = sceNetBind(sock, reinterpret_cast<SceNetSockaddr*>(&any), sizeof(any));
        if (result >= 0) {
            SceNetIpMreq group{};
            sceNetInetPton(SCE_NET_AF_INET, "224.0.0.251", &group.imr_multiaddr);
            group.imr_interface.s_addr = local_;
            result = sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP, &group, sizeof(group));
            if (result >= 0)
                sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP, SCE_NET_IP_MULTICAST_IF, &group.imr_interface,
                                 sizeof(group.imr_interface));
        }
        if (result < 0) {
            vitaLog("carplay-ctrl: not in the multicast group (0x%08x): no announcing, unicast answers only",
                    static_cast<unsigned>(result));
            sceNetSocketClose(sock);
            sock = -1;
        }
    }
    listenSocket_ = sock;
}

void Bonjour::closeSockets()
{
    if (querySocket_ >= 0)
        sceNetSocketClose(querySocket_);
    if (listenSocket_ >= 0)
        sceNetSocketClose(listenSocket_);
    querySocket_ = listenSocket_ = -1;
}

void Bonjour::arm(uint64_t nowUs)
{
    if (!begun_)
        return;
    openSockets();
    armed_ = true;
    armedUs_ = nowUs;
    nextQueryUs_ = 0;
    nextInstanceQueryUs_ = 0;
    nextAnnounceUs_ = 0;
    announcesLeft_ = ANNOUNCES;
    lastAddress_ = 0;
    lastPort_ = 0;
    queries_ = answers_ = questions_ = traced_ = 0;
    __atomic_store_n(&accepted_, 0u, __ATOMIC_RELEASE);
    vitaLog("carplay-ctrl: announcing '%s' and looking for the phone's control service", service_.name);
}

void Bonjour::service(uint64_t nowUs)
{
    if (!begun_ || !armed_)
        return;
    drain(querySocket_);
    drain(listenSocket_);
    if (thread_ >= 0 && !__atomic_load_n(&probeRunning_, __ATOMIC_ACQUIRE))
        joinProbe();
    if (announcesLeft_ > 0 && nowUs >= nextAnnounceUs_) {
        announce();
        --announcesLeft_;
        nextAnnounceUs_ = nowUs + (announcesLeft_ == ANNOUNCES - 1 ? 1000000 : 2000000);
    }
    if (accepted() || nowUs < nextQueryUs_)
        return;
    nextQueryUs_ = nowUs + (nowUs - armedUs_ < FAST_WINDOW_US ? FAST_QUERY_US : SLOW_QUERY_US);
    sendServiceQuery();
}

void Bonjour::drain(int sock)
{
    if (sock < 0)
        return;
    uint8_t packet[1500];
    for (int i = 0; i < 16; ++i) {
        SceNetSockaddrIn from{};
        unsigned int fromLength = sizeof(from);
        const int got = sceNetRecvfrom(sock, packet, sizeof(packet), 0, reinterpret_cast<SceNetSockaddr*>(&from),
                                       &fromLength);
        if (got <= 0)
            return;
        handlePacket(packet, static_cast<size_t>(got), from.sin_addr.s_addr, sceNetNtohs(from.sin_port));
    }
}

void Bonjour::handlePacket(const uint8_t* p, size_t length, uint32_t from, uint16_t fromPort)
{
    if (length < 12)
        return;
    if (VITA_AUTO_VERBOSE_LOGGING && traced_ < 40 && from != local_) {
        ++traced_;
        char text[400];
        describePacket(p, length, text, sizeof(text));
        vitaLog("carplay-ctrl: mdns %s from %u.%u.%u.%u:%u: %s", (p[2] & 0x80) ? "response" : "query",
                static_cast<unsigned>(from & 0xff), static_cast<unsigned>((from >> 8) & 0xff),
                static_cast<unsigned>((from >> 16) & 0xff), static_cast<unsigned>(from >> 24),
                static_cast<unsigned>(fromPort), text);
    }
    if (p[2] & 0x80)
        parseAnswer(p, length, from);
    else
        answerQuestion(p, length, from, fromPort);
}

void Bonjour::sendQuery(const uint8_t* name, size_t nameLength, const uint16_t* types, size_t count)
{
    if (querySocket_ < 0 || count == 0 || count > 4)
        return;
    uint8_t packet[12 + 4 * (NAME_MAX_WIRE + 4)] = {};
    putBe16(packet + 4, static_cast<uint16_t>(count));
    size_t at = 12;
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(packet + at, name, nameLength);
        at += nameLength;
        putBe16(packet + at, types[i]);
        putBe16(packet + at + 2, 1); // class IN
        at += 4;
    }
    SceNetSockaddrIn to;
    multicastGroup(to);
    const int sent =
        sceNetSendto(querySocket_, packet, at, 0, reinterpret_cast<SceNetSockaddr*>(&to), sizeof(to));
    if (sent < 0 && queries_ < 3)
        vitaLog("carplay-ctrl: query send failed 0x%08x", static_cast<unsigned>(sent));
}

void Bonjour::sendServiceQuery()
{
    const uint16_t ptr = TypePtr;
    sendQuery(CONTROL_SERVICE, sizeof(CONTROL_SERVICE), &ptr, 1);
    ++queries_;
    if (queries_ == 1 || queries_ % 10 == 0)
        vitaLog("carplay-ctrl: asked %u times, %u answers, %u questions to us, multicast group %s", queries_,
                answers_, questions_, listenSocket_ >= 0 ? "joined" : "not joined");
}

void Bonjour::parseAnswer(const uint8_t* p, size_t length, uint32_t from)
{
    const unsigned questions = be16(p + 4);
    const unsigned records = be16(p + 6) + be16(p + 8) + be16(p + 10);
    size_t at = 12;
    uint8_t name[NAME_MAX_WIRE + 1];
    size_t nameLength = 0, next = 0;
    for (unsigned i = 0; i < questions; ++i) {
        if (!readName(p, length, at, name, nameLength, next))
            return;
        at = next + 4;
        if (at > length)
            return;
    }

    bool sawSrv = false, hasAddress = false;
    uint16_t port = 0;
    uint32_t address = 0;
    uint8_t instance[NAME_MAX_WIRE + 1];
    size_t instanceLength = 0;

    for (unsigned i = 0; i < records && i < 64; ++i) {
        if (!readName(p, length, at, name, nameLength, next))
            return;
        at = next;
        if (at + 10 > length)
            return;
        const uint16_t type = be16(p + at);
        const size_t rdLength = be16(p + at + 8);
        const size_t rdata = at + 10;
        if (rdata + rdLength > length)
            return;
        at = rdata + rdLength;
        switch (type) {
        case TypePtr:
            if (sameName(name, nameLength, CONTROL_SERVICE, sizeof(CONTROL_SERVICE))) {
                size_t next2 = 0;
                if (!readName(p, length, rdata, instance, instanceLength, next2))
                    instanceLength = 0;
            }
            break;
        case TypeSrv:
            if (rdLength >= 7 && isControlInstance(name, nameLength)) {
                port = be16(p + rdata + 4);
                sawSrv = true;
            }
            break;
        case TypeA:
            // A self-assigned address is no use on this network.
            if (rdLength == 4 && !hasAddress && !(p[rdata] == 169 && p[rdata + 1] == 254)) {
                std::memcpy(&address, p + rdata, 4);
                hasAddress = true;
            }
            break;
        default: break;
        }
    }

    if (sawSrv && port != 0) {
        ++answers_;
        startProbe(hasAddress ? address : from, port);
    } else if (instanceLength > 0) {
        ++answers_;
        // The answer named the service but did not carry its SRV: ask for it.
        const uint64_t now = sceKernelGetSystemTimeWide();
        if (now >= nextInstanceQueryUs_) {
            nextInstanceQueryUs_ = now + INSTANCE_QUERY_US;
            const uint16_t types[2] = {TypeSrv, TypeTxt};
            sendQuery(instance, instanceLength, types, 2);
        }
    }
}

void Bonjour::startProbe(uint32_t address, uint16_t port)
{
    if (address == lastAddress_ && port == lastPort_)
        return;
    if (__atomic_load_n(&probeRunning_, __ATOMIC_ACQUIRE))
        return; // heard again on the next announcement
    joinProbe();
    probeAddress_ = address;
    probePort_ = port;
    char host[16] = {};
    sceNetInetNtop(SCE_NET_AF_INET, &address, host, sizeof(host));
    vitaLog("carplay-ctrl: found the phone's control service at %s:%u", host, static_cast<unsigned>(port));
    thread_ = sceKernelCreateThread("carplay_ctrl_probe", threadEntry, PROBE_THREAD_PRIORITY,
                                    PROBE_THREAD_STACK, 0, 0, nullptr);
    if (thread_ < 0) {
        vitaLog("carplay-ctrl: probe thread failed 0x%08x", static_cast<unsigned>(thread_));
        thread_ = -1;
        return;
    }
    lastAddress_ = address;
    lastPort_ = port;
    __atomic_store_n(&probeRunning_, 1u, __ATOMIC_RELEASE);
    Bonjour* self = this;
    if (sceKernelStartThread(thread_, sizeof(self), &self) < 0) {
        vitaLog("carplay-ctrl: probe thread did not start");
        __atomic_store_n(&probeRunning_, 0u, __ATOMIC_RELEASE);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
    }
}

void Bonjour::joinProbe()
{
    if (thread_ < 0)
        return;
    sceKernelWaitThreadEnd(thread_, nullptr, nullptr);
    sceKernelDeleteThread(thread_);
    thread_ = -1;
}

int Bonjour::threadEntry(SceSize, void* argp)
{
    Bonjour* self = *static_cast<Bonjour**>(argp);
    const int result = self->probeMain();
    __atomic_store_n(&self->probeRunning_, 0u, __ATOMIC_RELEASE);
    return result;
}

// GET /ctrl-int/1/connect until a 2xx. Alternates the device id format per attempt.
int Bonjour::probeMain()
{
    char host[16] = {};
    sceNetInetNtop(SCE_NET_AF_INET, &probeAddress_, host, sizeof(host));
    const unsigned long long number = deviceNumber(service_.deviceId);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%012llX", number);

    for (int attempt = 1; attempt <= PROBE_ATTEMPTS && !__atomic_load_n(&stop_, __ATOMIC_ACQUIRE); ++attempt) {
        const bool decimal = (attempt & 1) != 0;
        char id[24];
        if (decimal)
            std::snprintf(id, sizeof(id), "%llu", number);
        else
            std::snprintf(id, sizeof(id), "%s", hex);
        char request[320];
        const int requestLength = std::snprintf(
            request, sizeof(request),
            "GET /ctrl-int/1/connect HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: AirPlay/%s\r\n"
            "AirPlay-Receiver-Device-ID: %s\r\nConnection: close\r\n\r\n",
            host, static_cast<unsigned>(probePort_), service_.sourceVersion, id);

        char line[64] = {};
        int status = 0;
        const int sock = sceNetSocket("carplay_ctrl", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
        const int epoll = sock >= 0 ? sceNetEpollCreate("carplay_ctrl", 0) : -1;
        const char* stage = "socket";
        if (sock >= 0 && epoll >= 0) {
            const int one = 1;
            sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
            // Writable first (the connect finishing); readable once the request is out.
            SceNetEpollEvent want{};
            want.events = SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
            sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_ADD, sock, &want);

            SceNetSockaddrIn to{};
            to.sin_len = sizeof(to);
            to.sin_family = SCE_NET_AF_INET;
            to.sin_port = sceNetHtons(probePort_);
            to.sin_addr.s_addr = probeAddress_;
            stage = "connect";
            int result = sceNetConnect(sock, reinterpret_cast<SceNetSockaddr*>(&to), sizeof(to));
            if (result == static_cast<int>(SCE_NET_ERROR_EINPROGRESS) ||
                result == static_cast<int>(SCE_NET_ERROR_EAGAIN)) {
                result = waitSocket(epoll, SCE_NET_EPOLLOUT, CONNECT_TIMEOUT_MS, stop_) ? 0 : -1;
                if (result == 0) {
                    int error = 0;
                    unsigned int errorLength = sizeof(error);
                    sceNetGetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &error, &errorLength);
                    if (error != 0)
                        result = -error;
                }
            }
            if (result >= 0) {
                stage = "send";
                int sent = sceNetSend(sock, request, static_cast<size_t>(requestLength), 0);
                if (sent == static_cast<int>(SCE_NET_ERROR_EAGAIN))
                    sent = sceNetSend(sock, request, static_cast<size_t>(requestLength), 0);
                if (sent > 0) {
                    stage = "reply";
                    want.events = SCE_NET_EPOLLIN | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
                    sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_MOD, sock, &want);
                    if (waitSocket(epoll, SCE_NET_EPOLLIN, REPLY_TIMEOUT_MS, stop_)) {
                        const int got = sceNetRecv(sock, line, sizeof(line) - 1, 0);
                        if (got > 0) {
                            line[got] = '\0';
                            char* end = std::strpbrk(line, "\r\n");
                            if (end != nullptr)
                                *end = '\0';
                            const char* space = std::strchr(line, ' ');
                            status = space != nullptr ? std::atoi(space + 1) : 0;
                        }
                    }
                }
            }
        }
        if (epoll >= 0)
            sceNetEpollDestroy(epoll);
        if (sock >= 0)
            sceNetSocketClose(sock);

        if (status != 0)
            vitaLog("carplay-ctrl: GET /ctrl-int/1/connect (try %d, id as %s) -> %s", attempt,
                    decimal ? "number" : "hex", line);
        else
            vitaLog("carplay-ctrl: GET /ctrl-int/1/connect (try %d, id as %s) failed at %s", attempt,
                    decimal ? "number" : "hex", stage);
        if (status >= 200 && status < 300) {
            __atomic_store_n(&accepted_, 1u, __ATOMIC_RELEASE);
            return 0;
        }
        for (uint32_t waited = 0; waited < PROBE_RETRY_MS && !__atomic_load_n(&stop_, __ATOMIC_ACQUIRE);
             waited += PROBE_STEP_MS)
            sceKernelDelayThread(PROBE_STEP_MS * 1000);
    }
    return -1;
}

void Bonjour::answerQuestion(const uint8_t* p, size_t length, uint32_t from, uint16_t fromPort)
{
    const unsigned questions = be16(p + 4);
    size_t at = 12;
    uint8_t name[NAME_MAX_WIRE + 1];
    size_t nameLength = 0, next = 0;
    bool ptr = false, instance = false, host = false;
    for (unsigned i = 0; i < questions && i < 8; ++i) {
        if (!readName(p, length, at, name, nameLength, next) || next + 4 > length)
            return;
        const uint16_t type = be16(p + next);
        at = next + 4;
        if (sameName(name, nameLength, AIRPLAY_SERVICE, sizeof(AIRPLAY_SERVICE))) {
            ptr = ptr || type == TypePtr || type == TypeAny;
        } else if (sameName(name, nameLength, instanceName_, instanceNameLength_)) {
            instance = instance || type == TypeSrv || type == TypeTxt || type == TypeAny;
        } else if (sameName(name, nameLength, hostName_, hostNameLength_)) {
            host = host || type == TypeA || type == TypeAny;
        }
    }
    if (!ptr && !instance && !host)
        return;
    ++questions_;
    // A question from another port than 5353 is a plain resolver: it gets the
    // answer by unicast, with its id and question repeated.
    const bool legacy = fromPort != MDNS_PORT;
    if (legacy)
        sendRecords(ptr, instance, host, from, fromPort, p + 12, at - 12, questions, be16(p));
    else
        sendRecords(ptr, instance, host, 0, 0, nullptr, 0, 0, 0);
}

void Bonjour::sendRecords(bool ptr, bool instance, bool host, uint32_t to, uint16_t toPort, const uint8_t* echo,
                          size_t echoLength, unsigned echoCount, uint16_t id)
{
    if (listenSocket_ < 0)
        return;
    uint8_t packet[1024] = {};
    putBe16(packet, id);
    putBe16(packet + 2, 0x8400); // response, authoritative
    putBe16(packet + 4, static_cast<uint16_t>(echoCount));
    size_t at = 12;
    if (echoLength > 0 && echoLength < 256) {
        std::memcpy(packet + at, echo, echoLength);
        at += echoLength;
    }
    unsigned count = 0;
    auto record = [&](const uint8_t* owner, size_t ownerLength, uint16_t type, bool flush, uint32_t ttl,
                      const uint8_t* rdata, size_t rdLength) {
        if (at + ownerLength + 10 + rdLength > sizeof(packet))
            return;
        std::memcpy(packet + at, owner, ownerLength);
        at += ownerLength;
        putBe16(packet + at, type);
        putBe16(packet + at + 2, flush ? 0x8001 : 1);
        putBe32(packet + at + 4, ttl);
        putBe16(packet + at + 8, static_cast<uint16_t>(rdLength));
        std::memcpy(packet + at + 10, rdata, rdLength);
        at += 10 + rdLength;
        ++count;
    };

    if (ptr)
        record(AIRPLAY_SERVICE, sizeof(AIRPLAY_SERVICE), TypePtr, false, TTL_OTHER, instanceName_,
               instanceNameLength_);
    if (ptr || instance) {
        uint8_t srv[6 + sizeof(hostName_)] = {};
        putBe16(srv + 4, service_.port); // priority 0, weight 0
        std::memcpy(srv + 6, hostName_, hostNameLength_);
        record(instanceName_, instanceNameLength_, TypeSrv, true, TTL_HOST, srv, 6 + hostNameLength_);

        record(instanceName_, instanceNameLength_, TypeTxt, true, TTL_OTHER, txt_, txtLength_);
    }
    if (ptr || instance || host)
        record(hostName_, hostNameLength_, TypeA, true, TTL_HOST, reinterpret_cast<const uint8_t*>(&local_), 4);

    putBe16(packet + 6, static_cast<uint16_t>(count));
    SceNetSockaddrIn dest;
    if (to == 0) {
        multicastGroup(dest);
    } else {
        dest = SceNetSockaddrIn{};
        dest.sin_len = sizeof(dest);
        dest.sin_family = SCE_NET_AF_INET;
        dest.sin_port = sceNetHtons(toPort);
        dest.sin_addr.s_addr = to;
    }
    sceNetSendto(listenSocket_, packet, at, 0, reinterpret_cast<SceNetSockaddr*>(&dest), sizeof(dest));
}

void Bonjour::announce() { sendRecords(true, false, false, 0, 0, nullptr, 0, 0, 0); }

} // namespace carplay
