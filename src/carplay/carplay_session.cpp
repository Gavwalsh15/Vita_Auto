#include "carplay/carplay_session_impl.h"

#include "inputs/vita_input.h"
#include "logger/vita_log.h"
#include "transport/net_socket.h"

#include <psp2/kernel/rng.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace carplay {
namespace {

int listenOn(const char* name, uint16_t port) { return net_socket::listenTcp(name, port, 2); }

int udpOn(const char* name, uint16_t port) { return net_socket::bindUdp(name, port, 128 * 1024); }

// Fixed ports, so a restarted session finds the same listeners.
constexpr uint16_t EVENT_PORT = 7011;
constexpr uint16_t SCREEN_PORT = 7100;
constexpr uint16_t TIMING_PORT = 7010;
constexpr uint16_t AUDIO_CONTROL_PORT = 7201;
constexpr uint16_t TUNNEL_PORT = 7130;
constexpr uint16_t KEEPALIVE_PORT = 7012;
constexpr uint16_t AUDIO_PORTS[3] = {7200, 7202, 7203};

constexpr size_t RX_LIMIT_PER_PASS = 256 * 1024;
constexpr uint64_t TIMING_INTERVAL_US = 1000000;
constexpr uint64_t SUMMARY_INTERVAL_US = 10000000;

// The iAP2 tunnel carries "comm" packages: a 32-byte header (size big endian at 0,
// type at 16) and the iAP2 link bytes.
constexpr size_t PACKAGE_HEADER = 32;
constexpr size_t PACKAGE_TYPE_OFFSET = 16;
constexpr uint32_t PACKAGE_COMM = 0x636f6d6d;
constexpr uint32_t MAX_PACKAGE = 4 * 1024 * 1024;

uint32_t readBe32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

} // namespace

bool Session::Impl::begin()
{
    // The pairing identity: kept in a file so a phone that paired once is
    // recognised after a restart. A damaged or missing file means a new identity.
    bool loaded = false;
    if (config.statePath[0] != '\0') {
        if (FILE* file = std::fopen(config.statePath, "rb")) {
            Bytes data;
            uint8_t chunk[512];
            size_t got;
            while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && data.size() < 16 * 1024)
                data.append(chunk, got);
            std::fclose(file);
            loaded = core::load_pairing_state(data.data(), data.size(), identity, store);
        }
    }
    if (!loaded) {
        if (!identity.generate()) {
            vitaLog("carplay: could not create the pairing identity");
            return false;
        }
        saveState();
        vitaLog("carplay: created a new pairing identity (every iPhone pairs again)");
    } else {
        vitaLog("carplay: pairing identity loaded, %u paired phone(s)",
                static_cast<unsigned>(store.size()));
    }
    identity.public_key_hex(publicKeyHex);

    epoll = sceNetEpollCreate("vita_auto_carplay", 0);
    if (epoll < 0) {
        vitaLog("carplay: epoll create failed 0x%08x", static_cast<unsigned>(epoll));
        return false;
    }

    std::snprintf(deviceId, sizeof(deviceId), "%s", config.handoff.deviceId);
    info.name = config.handoff.name;
    info.deviceId = deviceId;
    // /info's bluetoothIDs points at this buffer for the whole run (each phone's
    // control channel copies the pointer), so setBluetoothAddress() rewrites the
    // text in place. The AirPlay device id stands in until the real one is known.
    if (config.handoff.btAddressKnown)
        formatMac(bluetoothAddress, config.handoff.btAddress, true);
    else
        std::snprintf(bluetoothAddress, sizeof(bluetoothAddress), "%s", deviceId);
    info.bluetoothMac = bluetoothAddress;
    info.sourceVersion = config.handoff.sourceVersion;
    info.audioOffer = config.audioOffer;

    constexpr int64_t NTP_TO_UNIX_SECONDS = 2208988800ll;
    const int64_t unixSeconds = static_cast<int64_t>(std::time(nullptr));
    timingBaseNs = unixSeconds > 0
        ? (unixSeconds + NTP_TO_UNIX_SECONDS) * 1000000000ll - static_cast<int64_t>(nowUs()) * 1000
        : 0;

    const int control = listenOn("carplay_control", config.handoff.airplayPort);
    if (control < 0)
        return false;
    watch(ControlListen, control);
    begun = true;
    vitaLog("carplay: AirPlay receiver listening on %s:%u (device %s, audio offer %s)", config.handoff.ip,
            static_cast<unsigned>(config.handoff.airplayPort), deviceId, audioOfferName(config.audioOffer));
    return true;
}

void Session::Impl::end()
{
    closeConnection("shutdown");
    closeSlot(ControlListen);
    if (epoll >= 0) {
        sceNetEpollDestroy(epoll);
        epoll = -1;
    }
    begun = false;
}

void Session::Impl::saveState()
{
    if (config.statePath[0] == '\0')
        return;
    Bytes image;
    if (!core::save_pairing_state(identity, store, image))
        return;
    if (FILE* file = std::fopen(config.statePath, "wb")) {
        std::fwrite(image.data(), 1, image.size(), file);
        std::fclose(file);
    } else {
        vitaLog("carplay: could not save the pairing state");
    }
}

void Session::Impl::closeConnection(const char* why)
{
    if (conn == nullptr)
        return;
    vitaLog("carplay: session ended (%s): requests=%u frames=%u configs=%u audio=%u", why,
            conn->control.requests(), conn->media.frames(), conn->media.configs(),
            conn->media.audio_packets());
    if (conn->security.paired() && !conn->saved)
        saveState();
    for (Slot slot : {ControlConn, EventListen, Event, ScreenListen, Screen, TunnelListen, Tunnel, UdpTiming,
                      UdpAudioMain, UdpAudioAlt, UdpAudioHigh, UdpAudioControl, UdpKeepAlive})
        closeSlot(slot);
    conn.reset();
    for (audio::Stream& stream : audioStreams) {
        stream.logStats("carplay");
        stream.reset();
    }
    audioUnsupported = 0;
    knob = core::KnobState{};
    inputs::setReady(false);
}

void Session::Impl::watch(Slot slot, int fd)
{
    closeSlot(slot);
    fds[slot] = fd;
    SceNetEpollEvent want{};
    want.events = SCE_NET_EPOLLIN | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
    want.data.u32 = static_cast<uint32_t>(slot);
    sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_ADD, fd, &want);
}

void Session::Impl::closeSlot(Slot slot)
{
    if (fds[slot] < 0)
        return;
    if (epoll >= 0)
        sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_DEL, fds[slot], nullptr);
    sceNetSocketClose(fds[slot]);
    fds[slot] = -1;
    tx[slot].clear();
}

void Session::Impl::queue(Slot slot, const uint8_t* data, size_t size)
{
    if (fds[slot] < 0 || size == 0)
        return;
    tx[slot].insert(tx[slot].end(), data, data + size);
}

void Session::Impl::queue(Slot slot, const Bytes& data) { queue(slot, data.data(), data.size()); }

void Session::Impl::flush()
{
    for (int i = 0; i < SlotCount; ++i) {
        std::vector<uint8_t>& pending = tx[i];
        if (fds[i] < 0 || pending.empty())
            continue;
        size_t offset = 0;
        while (offset < pending.size()) {
            const int written = sceNetSend(fds[i], pending.data() + offset, pending.size() - offset, 0);
            if (written == static_cast<int>(SCE_NET_ERROR_EAGAIN))
                break;
            if (written < 0) {
                vitaLog("carplay: send on slot %d failed 0x%08x", i, static_cast<unsigned>(written));
                pending.clear();
                offset = 0;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (offset > 0)
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void Session::Impl::acceptOn(Slot listen, Slot connection, const char* what)
{
    SceNetSockaddrIn peer{};
    unsigned int length = sizeof(peer);
    const int accepted = sceNetAccept(fds[listen], reinterpret_cast<SceNetSockaddr*>(&peer), &length);
    if (accepted < 0)
        return;
    const int one = 1;
    sceNetSetsockopt(accepted, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    sceNetSetsockopt(accepted, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));
    if (connection == Screen) {
        const int buffer = 256 * 1024;
        sceNetSetsockopt(accepted, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &buffer, sizeof(buffer));
    }
    watch(connection, accepted);
    if (connection == ControlConn)
        conn->peer = peer.sin_addr;
    vitaLog("carplay: phone connected to the %s port", what);
}

void Session::Impl::handleSlot(Slot slot)
{
    switch (slot) {
    case ControlListen:
        // A new control connection means a new session: drop the old one first.
        if (conn != nullptr)
            closeConnection("new connection");
        conn.reset(new Conn(*this));
        acceptOn(ControlListen, ControlConn, "AirPlay control");
        break;
    case EventListen:
        acceptOn(EventListen, Event, "event");
        if (conn != nullptr && fds[Event] >= 0)
            conn->events.start(conn->security.shared_secret());
        break;
    case ScreenListen:
        acceptOn(ScreenListen, Screen, "screen");
        if (conn != nullptr && fds[Screen] >= 0)
            conn->media.screen_connected();
        break;
    case TunnelListen:
        acceptOn(TunnelListen, Tunnel, "iAP2 tunnel");
        break;
    case ControlConn:
    case Event:
    case Screen:
    case Tunnel:
        readStream(slot);
        break;
    default:
        readDatagrams(slot);
        break;
    }
}

void Session::Impl::readStream(Slot slot)
{
    size_t total = 0;
    while (fds[slot] >= 0 && conn != nullptr && total < RX_LIMIT_PER_PASS) {
        const int got = sceNetRecv(fds[slot], rx.data(), rx.size(), 0);
        if (got == static_cast<int>(SCE_NET_ERROR_EAGAIN))
            return;
        if (got <= 0) {
            vitaLog("carplay: slot %d closed (recv 0x%08x)", static_cast<int>(slot),
                    static_cast<unsigned>(got));
            if (slot == ControlConn) {
                closeConnection("control closed");
            } else if (slot == Screen) {
                // The phone ended the video; the control connection decides
                // whether the whole session is over.
                closeSlot(Screen);
            } else if (slot == Tunnel) {
                conn->tunnel.reset();
                closeSlot(Tunnel);
            } else {
                closeSlot(slot);
            }
            return;
        }
        total += static_cast<size_t>(got);
        switch (slot) {
        case ControlConn: onControlData(rx.data(), static_cast<size_t>(got)); break;
        case Event: onEventData(rx.data(), static_cast<size_t>(got)); break;
        case Screen: onScreenData(rx.data(), static_cast<size_t>(got)); break;
        case Tunnel: onTunnelData(rx.data(), static_cast<size_t>(got)); break;
        default: break;
        }
    }
}

void Session::Impl::readDatagrams(Slot slot)
{
    for (int i = 0; i < 64 && fds[slot] >= 0 && conn != nullptr; ++i) {
        SceNetSockaddrIn from{};
        unsigned int fromLength = sizeof(from);
        const int got = sceNetRecvfrom(fds[slot], rx.data(), rx.size(), 0,
                                       reinterpret_cast<SceNetSockaddr*>(&from), &fromLength);
        if (got <= 0)
            return;
        const size_t size = static_cast<size_t>(got);
        switch (slot) {
        case UdpTiming: {
            uint8_t reply[core::kTimingPacketBytes];
            if (conn->media.feed_timing(rx.data(), size, timingNowNs(), reply))
                sceNetSendto(fds[UdpTiming], reply, sizeof(reply), 0,
                             reinterpret_cast<SceNetSockaddr*>(&from), sizeof(from));
            break;
        }
        case UdpAudioMain: conn->media.feed_audio(core::MediaChannel::AudioMain, rx.data(), size); break;
        case UdpAudioAlt: conn->media.feed_audio(core::MediaChannel::AudioAlt, rx.data(), size); break;
        case UdpAudioHigh:
            conn->media.feed_audio(core::MediaChannel::AudioMainHigh, rx.data(), size);
            break;
        default: break; // RTCP on the audio control port is not used
        }
    }
}

void Session::Impl::onControlData(const uint8_t* data, size_t size)
{
    Bytes out;
    const bool ok = conn->control.feed(data, size, out);
    queue(ControlConn, out);
    if (conn->security.paired() && !conn->saved) {
        conn->saved = true;
        saveState();
        vitaLog("carplay: phone paired; identity saved (%u paired)", static_cast<unsigned>(store.size()));
    }
    if (!ok) {
        vitaLog("carplay: control protocol error after '%s'", conn->control.lastPath());
        flush();
        closeConnection("protocol error");
    }
}

void Session::Impl::onEventData(const uint8_t* data, size_t size)
{
    Bytes out;
    if (!conn->events.feed(data, size, out)) {
        vitaLog("carplay: event channel error");
        closeSlot(Event);
        return;
    }
    queue(Event, out);
}

void Session::Impl::onScreenData(const uint8_t* data, size_t size)
{
    if (!conn->media.feed_screen(data, size)) {
        vitaLog("carplay: screen stream error (frames=%u)", conn->media.frames());
        closeSlot(Screen);
    }
}

void Session::Impl::onTunnelData(const uint8_t* data, size_t size)
{
    if (!conn->tunnel)
        return;
    if (!conn->tunnelEncrypted.append(data, size) ||
        !conn->tunnelCipher.decrypt(conn->tunnelEncrypted, conn->tunnelPlain)) {
        vitaLog("carplay: iAP2 tunnel decrypt failed");
        conn->tunnel.reset();
        closeSlot(Tunnel);
        return;
    }
    const int64_t now = static_cast<int64_t>(nowUs() / 1000);
    size_t offset = 0;
    const uint8_t* plain = conn->tunnelPlain.data();
    const size_t length = conn->tunnelPlain.size();
    while (length - offset >= PACKAGE_HEADER) {
        const uint32_t packageSize = readBe32(plain + offset);
        if (packageSize < PACKAGE_HEADER || packageSize > MAX_PACKAGE) {
            offset = length;
            break;
        }
        if (length - offset < packageSize)
            break;
        if (readBe32(plain + offset + PACKAGE_TYPE_OFFSET) == PACKAGE_COMM)
            conn->tunnel->feed(plain + offset + PACKAGE_HEADER, packageSize - PACKAGE_HEADER, now);
        offset += packageSize;
    }
    conn->tunnelPlain.erase_front(offset);
    pumpTunnel();
}

void Session::Impl::pumpTunnel()
{
    if (conn == nullptr || !conn->tunnel)
        return;
    conn->tunnel->tick(static_cast<int64_t>(nowUs() / 1000));
    std::vector<uint8_t> output;
    conn->tunnel->takeOutput(output);
    for (size_t offset = 0; offset < output.size();) {
        const size_t count = output.size() - offset < 1024 ? output.size() - offset : 1024;
        Bytes out;
        if (conn->events.iapMessage(output.data() + offset, count, out))
            queue(Event, out);
        offset += count;
    }
}

void Session::Impl::sendTiming(uint64_t now)
{
    if (conn == nullptr || fds[UdpTiming] < 0 || now < conn->nextTimingUs)
        return;
    conn->nextTimingUs = now + TIMING_INTERVAL_US;
    const uint32_t port = conn->media.peer_timing_port();
    if (port == 0 || conn->peer.s_addr == 0)
        return;
    uint8_t packet[core::kTimingPacketBytes];
    conn->media.timing_request(timingNowNs(), packet);
    SceNetSockaddrIn to{};
    to.sin_len = sizeof(to);
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(static_cast<uint16_t>(port));
    to.sin_addr = conn->peer;
    sceNetSendto(fds[UdpTiming], packet, sizeof(packet), 0, reinterpret_cast<SceNetSockaddr*>(&to),
                 sizeof(to));
}

void Session::Impl::summary(uint64_t now)
{
    if (conn == nullptr || now < nextSummaryUs)
        return;
    nextSummaryUs = now + SUMMARY_INTERVAL_US;
    uint32_t played = 0, dropped = 0;
    for (const audio::Stream& stream : audioStreams) {
        played += stream.packets();
        dropped += stream.dropped();
    }
    dropped += audioUnsupported;
    vitaLog("carplay: paired=%d verified=%d recording=%d events=%d screen: configs=%u frames=%u "
            "submitted=%u audio=%u (played %u, dropped %u) hid=%u clock=%s (%u answers) tunnel=%s",
            conn->security.paired(), conn->security.verified(), conn->media.recording(),
            conn->events.ready(), conn->media.configs(), conn->media.frames(), videoFrames,
            conn->media.audio_packets(), played, dropped, hidSent,
            conn->media.clock().synced() ? "synced" : "no",
            static_cast<unsigned>(conn->media.clock().responses()),
            conn->tunnel ? (conn->tunnel->authSucceeded() ? "authenticated" : "up") : "none");
}

void Session::Impl::service(uint32_t waitUs)
{
    if (!begun)
        return;
    SceNetEpollEvent events[8];
    const int count = sceNetEpollWait(epoll, events, 8, waitUs);
    for (int i = 0; i < count; ++i) {
        const uint32_t slot = events[i].data.u32;
        if (slot < SlotCount && fds[slot] >= 0)
            handleSlot(static_cast<Slot>(slot));
    }
    const uint64_t now = nowUs();
    sendTiming(now);
    pumpTunnel();
    drainInput();
    flush();
    summary(now);
}

bool core::random_bytes(uint8_t* out, size_t length)
{
    return sceKernelGetRandomNumber(out, length) >= 0;
}

bool Session::Impl::open(core::MediaChannel channel, uint32_t& port)
{
    int fd = -1;
    Slot slot = Event;
    switch (channel) {
    case core::MediaChannel::Event:
        slot = EventListen; port = EVENT_PORT; fd = listenOn("carplay_event", EVENT_PORT); break;
    case core::MediaChannel::Screen:
        slot = ScreenListen; port = SCREEN_PORT; fd = listenOn("carplay_screen", SCREEN_PORT); break;
    case core::MediaChannel::Timing:
        slot = UdpTiming; port = TIMING_PORT; fd = udpOn("carplay_timing", TIMING_PORT); break;
    case core::MediaChannel::AudioMain:
        slot = UdpAudioMain; port = AUDIO_PORTS[0]; fd = udpOn("carplay_audio0", AUDIO_PORTS[0]); break;
    case core::MediaChannel::AudioAlt:
        slot = UdpAudioAlt; port = AUDIO_PORTS[1]; fd = udpOn("carplay_audio1", AUDIO_PORTS[1]); break;
    case core::MediaChannel::AudioMainHigh:
        slot = UdpAudioHigh; port = AUDIO_PORTS[2]; fd = udpOn("carplay_audio2", AUDIO_PORTS[2]); break;
    case core::MediaChannel::AudioControl:
        slot = UdpAudioControl; port = AUDIO_CONTROL_PORT; fd = udpOn("carplay_audioctl", AUDIO_CONTROL_PORT); break;
    }
    if (fd < 0)
        return false;
    watch(slot, fd);
    return true;
}

void Session::Impl::close(core::MediaChannel channel)
{
    switch (channel) {
    case core::MediaChannel::Event: closeSlot(EventListen); closeSlot(Event); break;
    case core::MediaChannel::Screen: closeSlot(ScreenListen); closeSlot(Screen); break;
    case core::MediaChannel::Timing: closeSlot(UdpTiming); break;
    case core::MediaChannel::AudioMain: closeSlot(UdpAudioMain); break;
    case core::MediaChannel::AudioAlt: closeSlot(UdpAudioAlt); break;
    case core::MediaChannel::AudioMainHigh: closeSlot(UdpAudioHigh); break;
    case core::MediaChannel::AudioControl: closeSlot(UdpAudioControl); break;
    }
}

bool Session::Impl::open_tunnel(uint64_t seed, uint32_t& port)
{
    if (conn == nullptr)
        return false;
    uint8_t key[32];
    core::screen_stream_key(conn->security.shared_secret(), seed, key);
    conn->tunnelCipher.set_keys(key, key);
    std::memset(key, 0, sizeof(key));
    conn->tunnelEncrypted.clear();
    conn->tunnelPlain.clear();

    const int listener = listenOn("carplay_tunnel", TUNNEL_PORT);
    if (listener < 0)
        return false;
    watch(TunnelListen, listener);

    Handoff handoff = config.handoff;
    std::snprintf(handoff.publicKeyHex, sizeof(handoff.publicKeyHex), "%s", publicKeyHex);
    conn->tunnel.reset(new Iap2Endpoint(handoff, auth, Iap2Role::Tunnel));
    if (!conn->tunnel->start(static_cast<int64_t>(nowUs() / 1000))) {
        conn->tunnel.reset();
        return false;
    }
    port = TUNNEL_PORT;
    vitaLog("carplay: iAP2 data stream set up on port %u", static_cast<unsigned>(TUNNEL_PORT));
    return true;
}

void Session::Impl::close_tunnel()
{
    if (conn != nullptr)
        conn->tunnel.reset();
    closeSlot(TunnelListen);
    closeSlot(Tunnel);
}

bool Session::Impl::open_keep_alive(uint32_t& port)
{
    const int sock = udpOn("carplay_keepalive", KEEPALIVE_PORT);
    if (sock < 0)
        return false;
    watch(UdpKeepAlive, sock);
    port = KEEPALIVE_PORT;
    return true;
}

Session::Session(const SessionConfig& config, core::Authenticator& authenticator)
    : impl_(new Impl(config, authenticator))
{
}

Session::~Session()
{
    impl_->end();
    delete impl_;
}

bool Session::begin() { return impl_->begin(); }
void Session::end() { impl_->end(); }
const char* Session::publicKeyHex() const { return impl_->publicKeyHex; }
const char* Session::pairingId() const { return impl_->identity.pairing_id; }

void Session::setBluetoothAddress(const uint8_t address[6])
{
    Handoff& handoff = impl_->config.handoff; // the tunnel's identification reads it from here
    std::memcpy(handoff.btAddress, address, sizeof(handoff.btAddress));
    handoff.btAddressKnown = true;
    formatMac(impl_->bluetoothAddress, address, true);
}
void Session::service(uint32_t waitUs) { impl_->service(waitUs); }
bool Session::phoneConnected() const { return impl_->conn != nullptr && impl_->fds[ControlConn] >= 0; }
bool Session::bluetoothNoLongerNeeded() const
{
    const Impl::Conn* conn = impl_->conn.get();
    return conn != nullptr && conn->control.bluetoothDisableRequested() && conn->tunnel &&
           conn->tunnel->authSucceeded();
}

} // namespace carplay
