#pragma once

#include <stdint.h>

// States checked by the app, intermediate handshake states retain values 1-6.
enum VitaAutoUsbState {
    VITA_AUTO_USB_IDLE = 0,
    VITA_AUTO_USB_AA_VERSION_COMPLETE = 7,
    VITA_AUTO_USB_FAILED = 8
};

/* Diagnostic codes returned by GetLastFailureStage. Keep the numeric
   values so existing logs remain
*/
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
    VITA_AUTO_USB_FAILURE_STOP_ACCESSORY = 27
};

/*
   Best-effort: callbacks can update counters while this is copied.
   Transfer counts include the version handshake. Packet counts cover ring data.
   Completion counts exclude callbacks ignored after retirement. Busy/armed
   become zero once Stop has drained the outstanding transfers.
   Keep field order and widths stable across the kernel/client ABI.
*/
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

#ifdef __cplusplus
extern "C" {
#endif

/* App-side loader: load/start the client SUPRX after the kernel plugin is loaded.
 Returns a negative load error or the module's start status. */
int vitaAutoUsbClientLoad(const char *suprxPath);

// Polling also advances the version handshake. call until start or fail
int vitaAutoUsbGetState(void);

/* Returns bytes copied, zero when no wholr packet fits/available,
    or -1 on error. Packets are never split, so provide
    at least 4096 bytes of capacity to fit any valid ring slot.
    Wait variants try immediately, wait once for at most timeoutUs, then retry.
    A zero timeout polls. Stop, detach, and failure return -1.
    Ignore buffer/metadata contents whenever the returned value is negative.
*/
// metadata is required and receives completion timing for the returned bytes.
int vitaAutoUsbReadWaitTimed(
    void *buffer,
    uint32_t capacity,
    uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *metadata
);
// Interrupt an event wait, the reader may still return zero bytes
int vitaAutoUsbWake(void);

/* Copies and queues 1..16384 bytesm, returns length when queued,
    zero while busy/handshaking, or -1 on error, detach, or stop.
    queued bytes have not necessarily reached the phone.
*/
int vitaAutoUsbWrite(const void *buffer, uint32_t length);

/* Startup results are the latest SDK return codes. VID/PID packs VID in the
 upper 16 bits. Failure count is stage specific,
 not a cumulative error count*/
int vitaAutoUsbGetAccessoryActivateResult(void);
int vitaAutoUsbGetHostModeResult(void);
int vitaAutoUsbGetDriverRegisterResult(void);
unsigned int vitaAutoUsbGetLastVidPid(void);
int vitaAutoUsbGetLastFailureStage(void);
int vitaAutoUsbGetLastFailureResult(void);
int vitaAutoUsbGetLastFailureCount(void);
/* Start acquires the accessory port/host role and registers the USB driver.
 Repeated Start is a no op. A retired session is drained before starting again.
 Return zero or a error. */
int vitaAutoUsbStart(void);
/* Stop rejects new I/O, wakes readers, waits for transfers, unregisters the
  driver, and restores the previous host role. It can be called again safely.
  Returns zero when stopped. a negative result retains resources for retry.
The kernel plugin stays loaded. Serialize start/stop with the session owner. */
int vitaAutoUsbStop(void);
// Stop refilling RX. Existing reads and writes remain active until Stop.
// The session owner drains reads while the phone can still answer AA pings.
int vitaAutoUsbDrainReads(void);
int vitaAutoUsbGetCurrentHostMode(void);
// Copy the snapshot to diagnostics. returns the kernel copy result.
int vitaAutoUsbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *diagnostics);

#ifdef __cplusplus
}
#endif
