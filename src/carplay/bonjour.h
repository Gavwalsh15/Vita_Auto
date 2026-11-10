#pragma once

#include <cstddef>
#include <cstdint>

#include <psp2/kernel/threadmgr.h>

namespace carplay {

// Bonjour for CarPlay over Wi-Fi: announces the _airplay._tcp receiver and looks up the phone's
// _carplay-ctrl._tcp service, then sends it GET /ctrl-int/1/connect (what DiPlay and LIVI do).
// mDNS is polled from the owner's loop; the HTTP request runs on its own short-lived thread.
class Bonjour {
public:
    // What the _airplay._tcp record says.
    struct Service {
        char name[48] = {};          // instance name, e.g. the accessory name
        char model[24] = {};
        char deviceId[18] = {};      // "AA:BB:CC:DD:EE:FF"
        char sourceVersion[16] = {};
        char pairingId[40] = {};
        char publicKey[66] = {};     // hex
        uint64_t features = 0;
        uint16_t port = 0;
    };

    Bonjour() = default;
    ~Bonjour() { end(); }
    Bonjour(const Bonjour&) = delete;
    Bonjour& operator=(const Bonjour&) = delete;

    // localIp: the Vita's address on the phone's network (dotted).
    bool begin(const char* localIp, const Service& service);
    void end();

    // Starts (or restarts) announcing and looking for the phone's control service.
    void arm(uint64_t nowUs);
    // Announces, answers questions, sends the occasional query, reads answers and
    // starts the request when the phone's service turns up. Never blocks.
    void service(uint64_t nowUs);

    // The phone answered the connect request with a 2xx status.
    bool accepted() const { return __atomic_load_n(&accepted_, __ATOMIC_ACQUIRE) != 0; }

private:
    void openSockets();
    void closeSockets();
    void drain(int sock);
    void handlePacket(const uint8_t* packet, size_t length, uint32_t from, uint16_t fromPort);

    // looking up the phone
    void sendQuery(const uint8_t* name, size_t nameLength, const uint16_t* types, size_t count);
    void sendServiceQuery();
    void parseAnswer(const uint8_t* packet, size_t length, uint32_t from);
    void startProbe(uint32_t address, uint16_t port);
    void joinProbe();
    int probeMain();
    static int threadEntry(SceSize size, void* argp);

    // announcing ourselves
    void answerQuestion(const uint8_t* packet, size_t length, uint32_t from, uint16_t fromPort);
    void sendRecords(bool ptr, bool instance, bool host, uint32_t to, uint16_t toPort,
                     const uint8_t* echo, size_t echoLength, unsigned echoCount, uint16_t id);
    void announce();

    bool begun_ = false;
    bool armed_ = false;
    uint32_t local_ = 0;               // network byte order
    Service service_;
    uint8_t instanceName_[128] = {};   // <name>._airplay._tcp.local, wire form
    size_t instanceNameLength_ = 0;
    uint8_t hostName_[40] = {};        // carplay-<id>.local, wire form
    size_t hostNameLength_ = 0;
    uint8_t txt_[320] = {};            // the TXT record's data
    size_t txtLength_ = 0;
    int querySocket_ = -1;             // ephemeral port: answers come back as unicast
    int listenSocket_ = -1;            // port 5353 in the multicast group, best effort
    uint64_t armedUs_ = 0;
    uint64_t nextQueryUs_ = 0;
    uint64_t nextInstanceQueryUs_ = 0;
    uint64_t nextAnnounceUs_ = 0;
    uint32_t announcesLeft_ = 0;
    uint32_t queries_ = 0;             // for the log only
    uint32_t answers_ = 0;
    uint32_t questions_ = 0;
    uint32_t traced_ = 0;              // packets written to the log so far

    SceUID thread_ = -1;
    uint32_t probeAddress_ = 0;        // network byte order
    uint16_t probePort_ = 0;
    uint32_t lastAddress_ = 0;
    uint16_t lastPort_ = 0;
    volatile uint32_t probeRunning_ = 0;
    volatile uint32_t accepted_ = 0;
    volatile uint32_t stop_ = 0;
};

} // namespace carplay
