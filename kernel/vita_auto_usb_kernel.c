/*
   USB host transport for Android Auto.

 SceUsbd callbacks negotiate accessory mode and complete transfers. The app
 advances the version handshake by polling GetState, then exchanges bytes
 through the exported syscalls. Start/Stop own a session module startup/unload
 own its storage. Retired requests drain before that storage is reused.
 */
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/usbd.h>
#include <psp2kern/usbserv.h>

#include "vita_auto_usb.h"
#include "vita_auto_log.h"

// Android Open Accessory vendor requests
#define USB_GET_PROTOCOL 51
#define USB_SEND_STRING  52
#define USB_START        53

#define USB_REQ_TO_DEVICE_VENDOR 0x40
#define USB_REQ_TO_HOST_VENDOR   0xc0

#define GOOGLE_VENDOR_ID          0x18d1
#define USB_ACCESSORY_PID         0x2d00
#define USB_ACCESSORY_AUDIO_PID   0x2d01

#define AA_BULK_BUFFER_SIZE       16384 // Ring x4
#define USB_BULK_MAX_PACKET_SIZE  512
#define USB_ENDPOINT_PACKET_SIZE_MASK 0x07ff
#define USB_HOST_PORT             2
#define USB_HOST_MODE             0
#define USB_DEVICE_MODE           1
#define USB_STOP_TIMEOUT_US       1000000
#define USB_STOP_POLL_US          1000

/* High bits close admission low bits count code currently using the session.
Diagnostics/wake remain available after Stop, until module unload begins. */
#define USB_IO_CLOSED             0x80000000u
#define USB_ALL_CLOSED            0x40000000u
#define USB_OPERATION_COUNT       0x3fffffffu

// Delays count GetState polls, not elapsed milliseconds.
#define AA_VERSION_MAX_RETRIES    8
#define AA_VERSION_RETRY_POLLS    10
#define AA_CONFIG_SETTLE_POLLS    3
/*
 Vita's host controller reliably completes endpoint-sized bulk reads. AA
 and TLS are byte streams, so larger records are reassembled above this
 layer. Keep enough packet slots to absorb bursts during decode/present.
 */
#define AA_BULK_READ_SLOT_SIZE    4096
#define AA_BULK_READ_SLOT_COUNT   32
#define AA_BULK_READ_PIPELINE_DEPTH 4
#define AA_BULK_READ_RING_BYTES   (AA_BULK_READ_SLOT_SIZE * AA_BULK_READ_SLOT_COUNT)
#define AA_BULK_READ_DMA_BYTES    (AA_BULK_READ_SLOT_SIZE * AA_BULK_READ_PIPELINE_DEPTH)
#define USB_EVENT_RX               0x01
#define USB_EVENT_USER_WAKE        0x02

#define AA_VERSION_RESPONSE_SIZE          12
#define AA_VERSION_RESPONSE_CHANNEL       0
#define AA_VERSION_RESPONSE_FLAGS         1
#define AA_VERSION_RESPONSE_LENGTH_HIGH   2
#define AA_VERSION_RESPONSE_LENGTH_LOW    3
#define AA_VERSION_RESPONSE_MESSAGE_HIGH  4
#define AA_VERSION_RESPONSE_MESSAGE_LOW   5
#define AA_VERSION_RESPONSE_MAJOR_HIGH    6
#define AA_VERSION_RESPONSE_MAJOR_LOW     7
#define AA_VERSION_RESPONSE_MINOR_HIGH    8
#define AA_VERSION_RESPONSE_MINOR_LOW     9
#define AA_VERSION_RESPONSE_STATUS_HIGH   10
#define AA_VERSION_RESPONSE_STATUS_LOW    11

enum UsbState {
    USB_IDLE = VITA_AUTO_USB_IDLE,
    USB_GETTING_PROTOCOL,
    USB_SENDING_STRINGS,
    USB_STARTING,
    USB_WAITING_FOR_ACCESSORY,
    USB_OPENING_BULK,
    USB_AA_VERSION_SENT,
    USB_AA_VERSION_COMPLETE = VITA_AUTO_USB_AA_VERSION_COMPLETE,
    USB_FAILED = VITA_AUTO_USB_FAILED
};

typedef struct UsbContext {
    int deviceId;
    SceUID controlPipe;
    SceUID bulkInPipe;
    SceUID bulkOutPipe;
    unsigned int stringIndex;
    enum UsbState state;
    unsigned char protocol[2] __attribute__((aligned(64)));
    SceUsbdDeviceRequest request;
} UsbContext;

/* Non-cacheable metadata shared by USB callbacks and the syscall reader.
   Volatile accesses do not replace the barriers used when publishing data.
*/
typedef struct UsbBulkShared {
    volatile int readArmed;
    volatile int readHead;
    volatile int readTail;
    volatile int writeBusy;
    volatile uint32_t readSubmitCount;
    volatile uint32_t readCompleteCount;
    volatile uint32_t readPublishedCount;
    volatile uint32_t readConsumedCount;
    volatile uint32_t readChecksumFailures;
    volatile uint32_t readSequenceFailures;
    volatile uint32_t readMaxDepth;
    volatile uint32_t readNextSequence;
    volatile uint32_t readExpectedSequence;
    volatile uint32_t writeSubmitCount;
    volatile uint32_t writeCompleteCount;
    volatile uint32_t writeFailureCount;
    volatile uint32_t writeExpectedLength;
    volatile int writeLastResult;
    volatile int writeLastBytes;
    int readLength[AA_BULK_READ_SLOT_COUNT];
    uint32_t readChecksum[AA_BULK_READ_SLOT_COUNT];
    uint32_t readSequence[AA_BULK_READ_SLOT_COUNT];
    uint64_t readCompletedAtUs[AA_BULK_READ_SLOT_COUNT];
} UsbBulkShared;

/* A completion may run before ksceUsbdBulkTransfer returns. SUBMITTING lets
 the scheduler avoid overwriting that completion with ACTIVE. */
enum BulkReadRequestState {
    BULK_READ_FREE = 0,
    BULK_READ_SUBMITTING,
    BULK_READ_ACTIVE,
    BULK_READ_COMPLETE
};

typedef struct BulkReadRequest {
    volatile int state;
    uint32_t submitSequence;
    int length;
    uint64_t completedAtUs;
    unsigned char *buffer;
} BulkReadRequest;

#define AA_BULK_SHARED_BYTES      ((sizeof(UsbBulkShared) + 511) & ~511)
#define AA_BULK_DMA_BYTES         (AA_BULK_SHARED_BYTES + AA_BULK_READ_RING_BYTES + AA_BULK_READ_DMA_BYTES + AA_BULK_BUFFER_SIZE)
#define AA_BULK_DMA_BLOCK_SIZE    ((AA_BULK_DMA_BYTES + 4095) & ~4095)

static UsbContext g_usb = {
    .deviceId = -1,
    .controlPipe = -1,
    .bulkInPipe = -1,
    .bulkOutPipe = -1,
    .state = USB_IDLE
};

static UsbBulkShared *g_bulkShared;
static unsigned char (*g_bulkReadRing)[AA_BULK_READ_SLOT_SIZE];
/*
 RX DMA, the cross-core RX ring, and TX DMA all live in one physically
 contiguous non-cacheable block.  This prevents the USB completion CPU and
 user-syscall CPU from observing different cached versions of a packet.
 */
static unsigned char (*g_bulkReadDmaBuffers)[AA_BULK_READ_SLOT_SIZE];
static unsigned char *g_bulkWriteBuffer;
static SceUID g_bulkDmaMemBlock = -1;
static SceUID g_readEvent = -1;
static BulkReadRequest g_bulkReadRequests[AA_BULK_READ_PIPELINE_DEPTH];
static volatile uint32_t g_bulkReadSubmitSequence;
static volatile uint32_t g_bulkReadPublishSequence;
static volatile int g_bulkReadSchedulerBusy;
static volatile int g_bulkReadSchedulerPending;
static volatile int g_bulkReadsDraining;
static volatile int g_bulkReadConcurrencyLimit = AA_BULK_READ_PIPELINE_DEPTH;
static unsigned char g_controlBuffer[256]
    __attribute__((aligned(64)));
// Control channel, complete frame, six-byte payload: VERSION_REQUEST, 1.7.
static unsigned char g_versionRequest[] __attribute__((aligned(64))) = {
    0x00, 0x03, 0x00, 0x06, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x07
};
static SceUsbdDeviceRequest g_accessoryConfigRequest;
static volatile uint32_t g_bulkEndpointPacketSize = USB_BULK_MAX_PACKET_SIZE;
static volatile int g_accessoryActivateResult;
static volatile int g_hostModeResult;
static volatile int g_driverRegisterResult;
static volatile unsigned int g_lastVidPid;
static volatile int g_lastFailureStage;
static volatile int g_lastFailureResult;
static volatile int g_lastFailureCount;
static volatile int g_normalDeviceId = -1;
static volatile int g_accessoryDeviceId = -1;
static volatile int g_waitingForAccessory;
static volatile int g_versionSettlePolls;
static volatile int g_versionWriteCompleted;
static volatile int g_versionRetryCount;
static volatile int g_accessoryConfigPending;
static volatile int g_accessoryConfigured;
static SceUID g_accessoryControlPipe = -1;
static volatile uint32_t g_operationGate = USB_IO_CLOSED | USB_ALL_CLOSED;
static volatile int g_pendingTransfers;
static volatile int g_lifecycleBusy;
static volatile enum UsbState g_retiredState = USB_IDLE;
static int g_driverRegistered;
static int g_accessoryActivated;
static int g_hostModeChanged;
static int g_previousHostMode = USB_DEVICE_MODE;

static int sessionIsOpen(void)
{
    return (__sync_fetch_and_add(&g_operationGate, 0) & USB_IO_CLOSED) == 0;
}

// Never hold a lock across an SDK call: USBD can complete a transfer inline.
static int enterOperation(int allowStopped)
{
    uint32_t gate;
    do {
        gate = __sync_fetch_and_add(&g_operationGate, 0);
        if ((gate & USB_ALL_CLOSED) || (!allowStopped && (gate & USB_IO_CLOSED)))
            return 0;
    } while (!__sync_bool_compare_and_swap(&g_operationGate, gate, gate + 1));
    return 1;
}

static void leaveOperation(void)
{
    __sync_sub_and_fetch(&g_operationGate, 1);
}

static void wakeReader(void)
{
    if (g_readEvent >= 0)
        ksceKernelSetEventFlag(g_readEvent, USB_EVENT_RX);
}

/* Retiring only closes admission. Request storage is kept until callbacks drain;
 a detach callback must never wait for other callbacks on the USBD thread. */
static void retireSession(enum UsbState state)
{
    __sync_fetch_and_or(&g_operationGate, USB_IO_CLOSED);
    g_retiredState = state;
    g_usb.state = state;
    wakeReader();
}

#include "vita_auto_usb_bulk.inc"
#include "vita_auto_usb_session.inc"
#include "vita_auto_usb_syscalls.inc"
