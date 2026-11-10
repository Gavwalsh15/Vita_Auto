#pragma once

#include <stdint.h>

// States checked by the app, intermediate handshake states retain values 1-6.
enum VitaAutoUsbState {
    VITA_AUTO_USB_IDLE = 0,
    VITA_AUTO_USB_AA_VERSION_COMPLETE = 7,
    VITA_AUTO_USB_FAILED = 8
};

// Diagnostic codes returned by GetLastFailureStage; keep the values stable.
enum VitaAutoUsbFailureStage {
    VITA_AUTO_USB_FAILURE_NONE = 0,
    VITA_AUTO_USB_FAILURE_CONTROL_TRANSFER = 1,
    VITA_AUTO_USB_FAILURE_PROTOCOL_LENGTH = 2,
    VITA_AUTO_USB_FAILURE_PROTOCOL_UNSUPPORTED = 3,
    VITA_AUTO_USB_FAILURE_FIRST_ACCESSORY_STRING = 4,
    VITA_AUTO_USB_FAILURE_NEXT_ACCESSORY_REQUEST = 5,
    VITA_AUTO_USB_FAILURE_BULK_WRITE = 6,
    VITA_AUTO_USB_FAILURE_BULK_READ = 7,
    VITA_AUTO_USB_FAILURE_VERSION_RESPONSE = 8,
    VITA_AUTO_USB_FAILURE_BULK_PIPE_OPEN = 9,
    VITA_AUTO_USB_FAILURE_BULK_READ_SUBMIT = 10,
    VITA_AUTO_USB_FAILURE_VERSION_WRITE_SUBMIT = 11,
    VITA_AUTO_USB_FAILURE_CONTROL_PIPE_OPEN = 12,
    VITA_AUTO_USB_FAILURE_PROTOCOL_SUBMIT = 13,
    VITA_AUTO_USB_FAILURE_ACCESSORY_CONFIGURATION_SETUP = 14,
    VITA_AUTO_USB_FAILURE_CONFIGURATION_SUBMIT = 15,
    VITA_AUTO_USB_FAILURE_CONFIGURATION_TRANSFER = 16,
    VITA_AUTO_USB_FAILURE_READ_TOO_LARGE = 17,
    VITA_AUTO_USB_FAILURE_RING_CHECKSUM = 18,
    VITA_AUTO_USB_FAILURE_RING_SEQUENCE = 19,
    VITA_AUTO_USB_FAILURE_RING_LENGTH = 21,
    VITA_AUTO_USB_FAILURE_STOP_OPERATIONS = 22,
    VITA_AUTO_USB_FAILURE_STOP_CLOSE_PIPE = 23,
    VITA_AUTO_USB_FAILURE_STOP_UNREGISTER = 24,
    VITA_AUTO_USB_FAILURE_STOP_TRANSFERS = 25,
    VITA_AUTO_USB_FAILURE_STOP_HOST_MODE = 26,
    VITA_AUTO_USB_FAILURE_STOP_ACCESSORY = 27,
    VITA_AUTO_USB_FAILURE_RESUME_HOST_MODE = 28,
    VITA_AUTO_USB_FAILURE_STALE_ACCESSORY = 29,
    VITA_AUTO_USB_FAILURE_DETACHED = 30
};

// Best-effort snapshot; counters can change while it is copied. Keep field order and widths stable (kernel/client ABI).
typedef struct VitaAutoUsbRxDiagnostics {
    uint32_t submittedTransfers;
    uint32_t completedTransfers;
    uint32_t publishedPackets;
    uint32_t consumedPackets;
    uint32_t overflowCount;
    uint32_t checksumFailures;
    uint32_t sequenceFailures;
    uint32_t maxRingDepth;
    uint32_t ringDepth;
    uint32_t readArmed; // Number of submitted reads awaiting completion
    uint32_t submittedWrites;
    uint32_t completedWrites;
    uint32_t writeFailures;
    uint32_t writeBusy;
    int32_t lastWriteResult;
    int32_t lastWriteBytes;
} VitaAutoUsbRxDiagnostics;

// Earliest/latest nonzero USB completion times among the packets returned by
//one read, in microseconds from ksceKernelGetSystemTimeWide. Zero if empty
typedef struct VitaAutoUsbReadMetadata {
    uint64_t firstCompletedAtUs;
    uint64_t lastCompletedAtUs;
    uint32_t packetCount;
    uint32_t reserved; // Always zero. reserved for ABI compatibility
} VitaAutoUsbReadMetadata;

// Wireless transport (Bluetooth bootstrap), in the same plugin as USB; the app closes whichever loses.
#define VITA_AUTO_ERROR_TRANSPORT_BUSY (-2)

enum VitaAutoBtState {
    VITA_AUTO_BT_OFF = 0,     // Not enabled; the plugin never touches SceBt at boot
    VITA_AUTO_BT_WAITING = 1, // Enable called; patching SceBt and registering
    VITA_AUTO_BT_READY = 2,   // Advertising; no phone linked
    VITA_AUTO_BT_LINKED = 3,  // A phone is connected
    VITA_AUTO_BT_FAILED = 4
};

enum VitaAutoBtFailureStage {
    VITA_AUTO_BT_FAILURE_NONE = 0,
    VITA_AUTO_BT_FAILURE_BT_DISABLED = 1,
    VITA_AUTO_BT_FAILURE_NO_MODULE = 2,
    VITA_AUTO_BT_FAILURE_RECORD_NOT_FOUND = 3, // result = number of matches
    VITA_AUTO_BT_FAILURE_PATCH = 4,
    VITA_AUTO_BT_FAILURE_CALLBACK = 5
};

enum VitaAutoTransport {
    VITA_AUTO_TRANSPORT_NONE = 0,
    VITA_AUTO_TRANSPORT_USB = 1,
    VITA_AUTO_TRANSPORT_BT = 2
};

// Keep field order and widths stable across the kernel/client ABI.
typedef struct VitaAutoBtInfo {
    uint32_t layoutSize; // sizeof(VitaAutoBtInfo) in the plugin: the app checks it matches its own
    int32_t state;
    int32_t claimed;
    int32_t peerValid;
    int32_t failureStage;
    int32_t failureResult;
    uint32_t peerMac0; // Raw words from the SceBt connection event
    uint32_t peerMac1;
    uint32_t eventCount;
    int32_t discoverableResult; // Latest ksceBtSetInquiryScan(1) result
    int32_t discoverable;       // Nonzero once the phone can find the Vita
    uint32_t eventsDropped;     // Raw events lost because the app did not drain them
    int32_t nameResult;         // Result of renaming the Vita to "Vita Auto"
    int32_t nameChanged;        // Nonzero while the Vita carries that name
    uint32_t txCalls;           // ksceBtSendSpp calls made and bytes the stack accepted
    uint32_t txBytes;
    int32_t lastSendResult;     // The stack's last ksceBtSendSpp return value
    uint32_t rxCalls;           // ksceBtRecvSpp polls and bytes they returned
    uint32_t rxBytes;
    int32_t lastRecvResult;     // The stack's last ksceBtRecvSpp return value
    uint8_t searchSeen[64];     // SceBt bytes at the search patch site (16 before, 48 from it)
    int32_t searchPatched;      // Nonzero while the 128-bit service search patch is applied
    int32_t searchResult;       // 0 ok, -1 no SceBt, -2 other firmware, -3 record elsewhere
    int32_t patchVerified;      // Nonzero once the Android Auto record reads back from SceBt
    uint32_t patchOffset;       // Where in SceBt segment 0 the record was found
    // RFCOMM fixes (VITA_AUTO_BT_OPTION_RFCOMM) and what their hooks see.
    int32_t rfcommPatched;      // Nonzero while the PN credit patch is applied
    int32_t rfcommResult;       // 0 ok, -1 no SceBt, -2 other firmware, else inject error
    int32_t rfcommHooked;       // Nonzero while the sender and dispatcher hooks are in
    int32_t rfcommHookResult;   // 0, or the first hook install error
    uint32_t rfcommSenderCalls; // Data-DLCI frame-sender calls
    uint32_t dlcAddrByte;       // Last data send: RFCOMM address octet (DLCI in bits 2-7)
    uint32_t dlcPending;        // Last data send: bytes queued ([DLC+0x48])
    uint32_t dlcCredits;        // Last data send: send credits ([DLC+0x254])
    uint32_t dlcFlags;          // Last data send: flow flags before the fix ([DLC+0x260])
    uint32_t rxDispCalls;       // Incoming frames seen by the dispatcher
    uint32_t rxToEnq;           // ...routed to the SPP receive ring by SceBt itself
    uint32_t rxToCredit;        // ...routed to the credit handler
    uint32_t rxLastDesc;        // Last frame no dispatcher branch took: descriptor
    uint32_t rxLastLen;         // ...its length
    uint32_t rxLastB0;          // ...its first 8 bytes
    uint32_t rxLastB1;
    uint32_t rxRerouted;        // Data frames the fix sent to the SPP receive ring
    // iAP2 client (VITA_AUTO_BT_OPTION_IAP2_CLIENT): the Vita dials the iPhone.
    int32_t iap2Installed;      // Nonzero once the client is running
    int32_t iap2InstallResult;  // 0, or why it did not install (-1 other firmware)
    uint32_t iap2Milestones;    // VITA_AUTO_IAP2_* bits: how far the connection got
    int32_t iap2OpenResult;     // SceBt's result for the RFCOMM channel request
    uint32_t iap2Channel;       // RFCOMM channel the SDP search named (1 until known)
    int32_t iap2TxCredits;      // Frames the phone will take
    uint32_t iap2RxFrames;      // RFCOMM frames and payload bytes from the phone
    uint32_t iap2RxBytes;
    uint32_t iap2TxBytes;       // Payload bytes sent to the phone
    uint32_t iap2Truncated;     // Phone PDUs larger than the capture slot (data lost)
    uint32_t iap2RingDropped;   // PDUs lost because the thread fell behind
    uint32_t iap2LastMcc;       // Last multiplexer command type seen
    // The Vita's own Bluetooth address, which SceBt keeps from the controller's
    // Read BD_ADDR reply. Same packing as the SceBt events (mac0 = first four bytes
    // in memory order, mac1 = the last two); both zero when not known yet.
    uint32_t iap2LocalMac0;
    uint32_t iap2LocalMac1;
} VitaAutoBtInfo;

// One raw SceBt event, as the stack reported it, for logging.
typedef struct VitaAutoBtEvent {
    uint32_t timeMs;
    uint8_t data[16];
} VitaAutoBtEvent;

enum VitaAutoApState {
    VITA_AUTO_AP_OFF = 0,
    VITA_AUTO_AP_STARTING = 1, // Waiting for SceWlanBt, then configuring the firmware
    VITA_AUTO_AP_UP = 2,       // Beaconing; the app configures wlan0 and runs DHCP
    VITA_AUTO_AP_FAILED = 3
};

enum VitaAutoApFailureStage {
    VITA_AUTO_AP_FAILURE_NONE = 0,
    VITA_AUTO_AP_FAILURE_NO_MODULE = 1,    // SceWlanBt not loaded
    VITA_AUTO_AP_FAILURE_FIRMWARE = 2,     // Offsets do not look like this firmware; result = which
    VITA_AUTO_AP_FAILURE_HOOK = 3,         // result = taiHEN error
    VITA_AUTO_AP_FAILURE_NO_ADAPTER = 4,   // The Wi-Fi driver issued no command (Wi-Fi off?)
    VITA_AUTO_AP_FAILURE_SYS_CONFIG = 5,   // result = firmware status, or -1 for no reply
    VITA_AUTO_AP_FAILURE_BSS_START = 6,    // result = firmware status, or -1 for no reply
    VITA_AUTO_AP_FAILURE_WATCHDOG = 7      // The app stopped calling KeepAlive; the AP was taken down
};

// Addresses on the access point's network. The Vita is .1; the DHCP server
// hands out .10 to .13. Host byte order.
#define VITA_AUTO_AP_IP          0xC0A84D01u // 192.168.77.1
#define VITA_AUTO_AP_NETMASK     0xFFFFFF00u
#define VITA_AUTO_AP_IP_TEXT     "192.168.77.1"

typedef struct VitaAutoApConfig {
    char ssid[33];       // 1-32 characters
    char passphrase[64]; // Empty: open network; else 8-63 characters, WPA2-PSK (AES)
    uint8_t mac[6];      // The AP's BSSID (the app passes the Vita's MAC, locally administered)
    uint8_t channel;     // 1-11
    uint8_t reserved;
} VitaAutoApConfig;

typedef struct VitaAutoApInfo {
    int32_t state;
    int32_t failureStage;
    int32_t failureResult;
    int32_t configVariant;   // 0 SSID+MAC+channel accepted, 1 only the minimal set
    uint32_t adapterWaitMs;  // How long the driver took to issue a command we could see
    uint32_t moduleNid;      // SceWlanBt module NID (tells firmware builds apart)
    uint32_t commandsSeen;   // Driver commands that went through the enqueue hook
    uint32_t responses;      // Firmware responses and events seen
    uint32_t associations;   // UAP_STA_ASSOC events
    uint32_t deauths;        // UAP_STA_DEAUTH events
    uint8_t lastStation[6];  // MAC of the last station that associated
    uint16_t lastEvent;      // Last firmware event code
    uint32_t wlanFlags;      // SceWlanBt driver state word (0x1000 = station link up)
    uint32_t txKicks;        // Output calls from SceNetPs the plugin passed to the TX worker
    uint32_t txFrames;       // Data frames seen on their way to the chip
    uint32_t txStamped;      // Of those, the ones re-marked for the AP (TxPD bss_type 1)
    int32_t commandFailure;  // Last failed command: -1 no reply, -2 command pool full, -3 no buffers
    uint32_t poolMask;       // Driver command-pool slots in use at that moment (bit per slot)
    uint32_t pendingAtFail;  // Driver command node in flight at that moment (0: none)
    int32_t stopResult;      // Previous run's BSS_STOP result (-100: none yet)
    uint32_t stopPending;    // Driver command node still in flight after that stop (0: none)
} VitaAutoApInfo;

#ifdef __cplusplus
extern "C" {
#endif

// App-side loader: loads and starts the client SUPRX. Returns a negative load error or the start status.
int vitaAutoUsbClientLoad(const char *suprxPath);

// Polling also advances the version handshake. call until start or fail
int vitaAutoUsbGetState(void);

// Read: bytes copied, 0 if no whole packet fits, -1 on error. Packets are never split, so give at least 4096 bytes.
// Wait variants retry once after timeoutUs.
// metadata is required and receives completion timing for the returned bytes.
int vitaAutoUsbReadWaitTimed(
    void *buffer,
    uint32_t capacity,
    uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *metadata
);
// Interrupt an event wait, the reader may still return zero bytes
int vitaAutoUsbWake(void);

// Write: queues 1..16384 bytes and returns the length, 0 while busy, -1 on error.
int vitaAutoUsbWrite(const void *buffer, uint32_t length);

// Startup results are the latest SDK return codes; VID/PID packs VID in the upper 16 bits.
int vitaAutoUsbGetAccessoryActivateResult(void);
int vitaAutoUsbGetHostModeResult(void);
int vitaAutoUsbGetDriverRegisterResult(void);
unsigned int vitaAutoUsbGetLastVidPid(void);
int vitaAutoUsbGetLastFailureStage(void);
int vitaAutoUsbGetLastFailureResult(void);
int vitaAutoUsbGetLastFailureCount(void);
// Start acquires the accessory port and registers the USB driver; repeated Start is a no-op.
int vitaAutoUsbStart(void);
// Stop drains transfers and restores the host role; safe to repeat. A negative result keeps resources for retry.
int vitaAutoUsbStop(void);
// Stop refilling RX. Existing reads and writes remain active until Stop.
// The session owner drains reads while the phone can still answer AA pings.
int vitaAutoUsbDrainReads(void);
int vitaAutoUsbGetCurrentHostMode(void);
// Copy the snapshot to diagnostics. returns the kernel copy result.
int vitaAutoUsbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *diagnostics);

// Bluetooth bootstrap: Enable patches SceBt, Disable undoes it. Start claims the RFCOMM stream
// (or VITA_AUTO_ERROR_TRANSPORT_BUSY), Stop releases it. Read/Write return bytes or a negative error.
int vitaAutoBtEnable(void);
int vitaAutoBtDisable(void);
int vitaAutoBtStart(void);
int vitaAutoBtStop(void);
int vitaAutoBtReadWaitTimed(void *buffer, uint32_t capacity, uint32_t timeoutUs);
int vitaAutoBtWrite(const void *buffer, uint32_t length);
int vitaAutoBtGetInfo(VitaAutoBtInfo *info);
// Copies up to maxEvents queued raw events, oldest first; returns the count.
int vitaAutoBtReadEvents(VitaAutoBtEvent *events, uint32_t maxEvents);
// Name the Vita shows over Bluetooth while enabled (empty: keep the system
// name). recoverOriginal is the device name saved by a previous run that did not
// restore it (empty if none). Call before vitaAutoBtEnable.
int vitaAutoBtConfigureName(const char *wanted, const char *recoverOriginal);
// While the Vita is renamed: copies its original device name and returns the
// length, or 0 when nothing was changed.
int vitaAutoBtGetOriginalName(char *buffer, uint32_t size);
// The stack's connection state for a device (1 disconnected, 2 connecting,
// 5 connected, as far as is known).
int vitaAutoBtConnectState(uint32_t mac0, uint32_t mac1);
// Drops the Bluetooth link to a device (SceBt's own disconnect).
int vitaAutoBtDisconnect(uint32_t mac0, uint32_t mac1);
// Options for the next vitaAutoBtEnable. VITA_AUTO_BT_OPTION_RFCOMM fixes SceBt's RFCOMM credit flow control so the
// Android Auto channel carries data both ways (see vita_auto_bt.inc). Only takes
// effect on the firmware the fixes were written for.
#define VITA_AUTO_BT_OPTION_RFCOMM 0x2
// VITA_AUTO_BT_OPTION_IAP2_CLIENT makes the Vita dial a connected iPhone and open
// its iAP2 service, as a wireless CarPlay head unit does. The iAP2 bytes then
// come through Start / ReadWaitTimed / Write; GetInfo's iap2Milestones shows how
// far the connection got and VITA_AUTO_IAP2_PIPE says bytes can flow. Needs the
// search patch to have applied, so firmware 3.65 only.
#define VITA_AUTO_BT_OPTION_IAP2_CLIENT 0x4
#define VITA_AUTO_IAP2_CONN        0x0001 // found the phone's connection
#define VITA_AUTO_IAP2_L2CAP_RSP   0x0002 // phone accepted the RFCOMM channel
#define VITA_AUTO_IAP2_CFG_REQ     0x0004 // phone sent its configuration
#define VITA_AUTO_IAP2_CFG_RSP     0x0008 // phone accepted ours
#define VITA_AUTO_IAP2_SABM0       0x0010 // multiplexer requested
#define VITA_AUTO_IAP2_UA0         0x0020 // multiplexer up
#define VITA_AUTO_IAP2_SDP_SENT    0x0040
#define VITA_AUTO_IAP2_SDP_DONE    0x0080
#define VITA_AUTO_IAP2_SABM_DLC    0x0100
#define VITA_AUTO_IAP2_UA_DLC      0x0200 // iAP2 channel open
#define VITA_AUTO_IAP2_MSC_SENT    0x0400
#define VITA_AUTO_IAP2_MSC_RSP     0x0800
#define VITA_AUTO_IAP2_PIPE        0x1000 // bytes can flow
#define VITA_AUTO_IAP2_DM          0x2000 // phone refused the channel
#define VITA_AUTO_IAP2_LOST        0x4000 // phone closed the channel
#define VITA_AUTO_IAP2_SDP_CFG_REQ 0x8000
#define VITA_AUTO_IAP2_SDP_CFG_RSP 0x10000
int vitaAutoBtSetOptions(uint32_t flags);

// Vita-hosted Wi-Fi AP (fw 3.65). Stop before the app exits: launching or exiting an app while it is up panics.
// Call KeepAlive every VITA_AUTO_AP_KEEPALIVE_MS or the plugin drops the AP.
#define VITA_AUTO_AP_KEEPALIVE_MS 1000
int vitaAutoApStart(const VitaAutoApConfig *config);
int vitaAutoApStop(void);
int vitaAutoApKeepAlive(void);
int vitaAutoApGetInfo(VitaAutoApInfo *info);
// Gives wlan0 the AP address once the AP is up (user side: the stack's
// interface ioctls are syscalls). Returns wlan0's flags afterwards (0x8843 is
// up and running) or the first failing ioctl's error.
int vitaAutoApConfigureInterface(void);

#ifdef __cplusplus
}
#endif
