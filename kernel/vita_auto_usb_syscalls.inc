int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
{
    (void)eventid;
    (void)args;
    (void)opt;
    if (resume)
        __sync_lock_test_and_set(&g_hostModeResumePending, 1);
    return 0;
}

static void restoreHostModeAfterResume(void)
{
    int result;
    if (!__sync_lock_test_and_set(&g_hostModeResumePending, 0))
        return;
    if (!sessionIsOpen() || ksceUsbServMacGet(USB_HOST_PORT) == USB_HOST_MODE)
        return;

    result = ksceUsbServMacSelect(USB_HOST_PORT, USB_HOST_MODE);
    if (result < 0) {
        setUsbFailure(VITA_AUTO_USB_FAILURE_RESUME_HOST_MODE, result, 0);
        return;
    }
    g_hostModeChanged = 1;
    vitaUsbLogInfo("[VitaAutoUSB] Host mode restored after resume\n");
}

static int usbSysEventHandler(int resume, int eventid, void *args, void *opt)
{
    (void)eventid;
    (void)args;
    (void)opt;
    if (resume)
        __sync_lock_test_and_set(&g_hostModeResumePending, 1);
    return 0;
}

static void restoreHostModeAfterResume(void)
{
    int result;
    if (!__sync_lock_test_and_set(&g_hostModeResumePending, 0))
        return;
    if (!sessionIsOpen() || ksceUsbServMacGet(USB_HOST_PORT) == USB_HOST_MODE)
        return;

    result = ksceUsbServMacSelect(USB_HOST_PORT, USB_HOST_MODE);
    if (result < 0) {
        setUsbFailure(VITA_AUTO_USB_FAILURE_RESUME_HOST_MODE, result, 0);
        return;
    }
    g_hostModeChanged = 1;
    vitaUsbLogInfo("[VitaAutoUSB] Host mode restored after resume\n");
}

int module_start(SceSize argc, const void *args)
{
    int result;
    (void)argc;
    (void)args;

    /* Loading the plugin prepares storage. The app explicitly acquires the USB
     host role with Start, so a failed session start never unloads callback code. */
    result = allocateBulkMemory();
    if (result < 0)
        return result;
    g_readEvent = ksceKernelCreateEventFlag(
        "vita_auto_usb_rx", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    if (g_readEvent < 0) {
        result = g_readEvent;
        freeBulkMemory();
        return result;
    }
    resetSession();
    __sync_lock_test_and_set(&g_operationGate, USB_IO_CLOSED);
    if (ksceKernelRegisterSysEventHandler(
            "vita_auto_usb", usbSysEventHandler, NULL) >= 0)
        g_sysEventRegistered = 1;
    else
        ksceDebugPrintf("[VitaAutoUSB] Resume handler not registered\n");
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    int result;
    (void)argc;
    (void)args;
    if (!__sync_bool_compare_and_swap(&g_lifecycleBusy, 0, 1))
        return SCE_KERNEL_STOP_FAIL;
    if (g_sysEventRegistered) {
        __sync_lock_release(&g_lifecycleBusy);
        return SCE_KERNEL_STOP_FAIL;
    }

    // Also exclude diagnostics and wake calls before deleting their resources.
    __sync_fetch_and_or(&g_operationGate, USB_ALL_CLOSED | USB_IO_CLOSED);
    result = stopSession();
    if (result >= 0 && g_readEvent >= 0) {
        result = ksceKernelDeleteEventFlag(g_readEvent);
        if (result >= 0)
            g_readEvent = -1;
    }
    if (result >= 0)
        result = freeBulkMemory();
    if (result < 0 && g_readEvent >= 0)
        __sync_fetch_and_and(&g_operationGate, ~USB_ALL_CLOSED);
    __sync_lock_release(&g_lifecycleBusy);
    return result < 0 ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}

// User-mode syscall entry points. Start/Stop are serialized with module unload.
static int usbStart(void)
{
    int result;
    if (!__sync_bool_compare_and_swap(&g_lifecycleBusy, 0, 1))
        return -1;
    if (g_bulkShared == NULL || (g_operationGate & USB_ALL_CLOSED)) {
        result = -1;
        goto done;
    }
    if (sessionIsOpen()) {
        result = 0;
        goto done;
    }

    // This also completes cleanup after a detach or a previous failed Start.
    result = stopSession();
    if (result < 0)
        goto done;
    resetSession();
    g_driverRegisterResult = 0;
    g_hostModeResult = 0;
    g_accessoryActivateResult = 0;
    g_previousHostMode = ksceUsbServMacGet(USB_HOST_PORT);
    if (g_previousHostMode != USB_HOST_MODE &&
        g_previousHostMode != USB_DEVICE_MODE) {
        result = -1;
        goto failed;
    }
    // Register before enabling the port; attach callbacks need initialized storage.
    __sync_fetch_and_and(&g_operationGate, ~USB_IO_CLOSED);
    g_driverRegisterResult = ksceUsbdRegisterDriver(&g_usbDriver);
    result = g_driverRegisterResult;
    if (result < 0)
        goto failed;
    g_driverRegistered = 1;

    g_accessoryActivateResult = ksceUsbServAccessoryActivate();
    if (g_accessoryActivateResult >= 0)
        g_accessoryActivated = 1;
    else
        ksceDebugPrintf("[VitaAutoUSB] AccessoryActivate failed: 0x%08x\n",
                        g_accessoryActivateResult);
    g_hostModeResumePending = 0;
    if (g_previousHostMode != USB_HOST_MODE) {
        g_hostModeResult = ksceUsbServMacSelect(USB_HOST_PORT, USB_HOST_MODE);
        result = g_hostModeResult;
        if (result < 0)
            goto failed;
        g_hostModeChanged = 1;
    }

    result = 0;
    goto done;

failed:
    {
        const int cleanupResult = stopSession();
        if (cleanupResult < 0)
            ksceDebugPrintf("[VitaAutoUSB] Start rollback incomplete: 0x%08x\n", cleanupResult);
        g_retiredState = USB_FAILED;
        g_usb.state = USB_FAILED;
    }
done:
    __sync_lock_release(&g_lifecycleBusy);
    return result;
}

static int usbStop(void)
{
    int result;
    if (!__sync_bool_compare_and_swap(&g_lifecycleBusy, 0, 1))
        return -1;
    result = stopSession();
    __sync_lock_release(&g_lifecycleBusy);
    return result;
}

static int usbGetState(void)
{
    int state;
    if (!enterOperation(0))
        return g_retiredState;
    restoreHostModeAfterResume();
    pumpAndroidAutoVersionHandshake();
    state = sessionIsOpen() ? g_usb.state : g_retiredState;
    leaveOperation();
    return state;
}

static int usbGetCurrentHostMode(void)
{
    return ksceUsbServMacGet(USB_HOST_PORT);
}

static int usbGetAccessoryActivateResult(void)
{
    return g_accessoryActivateResult;
}

static int usbGetHostModeResult(void)
{
    return g_hostModeResult;
}

static int usbGetDriverRegisterResult(void)
{
    return g_driverRegisterResult;
}

static unsigned int usbGetLastVidPid(void)
{
    return g_lastVidPid;
}

static int usbGetLastFailureStage(void)
{
    return g_lastFailureStage;
}

static int usbGetLastFailureResult(void)
{
    return g_lastFailureResult;
}

static int usbGetLastFailureCount(void)
{
    return g_lastFailureCount;
}

/* One syscall reader owns the tail. Copy only complete ring slots from a fixed
 head snapshot, then release them after all selected copies succeed. Metadata
 is a kernel pointer here; exported timed reads copy it to user space below. */
static int readBulkInternal(
    void *userBuffer,
    uint32_t capacity,
    VitaAutoUsbReadMetadata *metadata
)
{
    int scan;
    int snapshotHead;
    uint32_t expectedSequence;
    uint32_t packetCount = 0;
    uint32_t totalLength = 0;
    uint64_t firstCompletedAtUs = 0;
    uint64_t lastCompletedAtUs = 0;

    if (userBuffer == NULL || capacity == 0 || !sessionIsOpen())
        return -1;

    if (metadata != NULL) {
        metadata->firstCompletedAtUs = 0;
        metadata->lastCompletedAtUs = 0;
        metadata->packetCount = 0;
        metadata->reserved = 0;
    }

    snapshotHead = g_bulkShared->readHead;
    if (snapshotHead == g_bulkShared->readTail) {
        if (g_usb.state == USB_AA_VERSION_COMPLETE && submitBulkRead() < 0)
            return -1;
        return 0;
    }

    scan = g_bulkShared->readTail;
    expectedSequence = g_bulkShared->readExpectedSequence;
    while (scan != snapshotHead) {
        const int length = g_bulkShared->readLength[scan];
        uint32_t checksum;

        if (length <= 0 || length > AA_BULK_READ_SLOT_SIZE) {
            setUsbFailure(VITA_AUTO_USB_FAILURE_RING_LENGTH, length, scan);
            return -1;
        }
        if (totalLength + (uint32_t)length > capacity)
            break;
        checksum = checksumBytes(g_bulkReadRing[scan], length);
        if (checksum != g_bulkShared->readChecksum[scan]) {
            ++g_bulkShared->readChecksumFailures;
            setUsbFailure(VITA_AUTO_USB_FAILURE_RING_CHECKSUM, (int)checksum, scan);
            return -1;
        }
        if (g_bulkShared->readSequence[scan] != expectedSequence) {
            ++g_bulkShared->readSequenceFailures;
            setUsbFailure(
                VITA_AUTO_USB_FAILURE_RING_SEQUENCE,
                (int)g_bulkShared->readSequence[scan],
                (int)expectedSequence
            );
            return -1;
        }

        {
            const uint64_t completedAtUs = g_bulkShared->readCompletedAtUs[scan];
            if (completedAtUs != 0) {
                if (firstCompletedAtUs == 0 || completedAtUs < firstCompletedAtUs)
                    firstCompletedAtUs = completedAtUs;
                if (completedAtUs > lastCompletedAtUs)
                    lastCompletedAtUs = completedAtUs;
            }
        }

        if (ksceKernelMemcpyKernelToUser(
                (unsigned char *)userBuffer + totalLength,
                g_bulkReadRing[scan],
                length) < 0)
        {
            return -1;
        }

        totalLength += (uint32_t)length;
        ++packetCount;
        ++expectedSequence;
        scan = (scan + 1) % AA_BULK_READ_SLOT_COUNT;
    }

    if (packetCount == 0)
        return 0;

    // A failed copy above leaves the tail unchanged so queued bytes can retry.
    while (g_bulkShared->readTail != scan) {
        const int slot = g_bulkShared->readTail;
        clearBulkReadSlot(slot);
        g_bulkShared->readTail = (slot + 1) % AA_BULK_READ_SLOT_COUNT;
    }
    g_bulkShared->readExpectedSequence = expectedSequence;
    g_bulkShared->readConsumedCount += packetCount;
    __sync_synchronize();

    if (submitBulkRead() < 0)
        return -1;

    if (metadata != NULL) {
        metadata->firstCompletedAtUs = firstCompletedAtUs;
        metadata->lastCompletedAtUs = lastCompletedAtUs;
        metadata->packetCount = packetCount;
    }

    return (int)totalLength;
}

static int readBulkTimed(
    void *userBuffer,
    uint32_t capacity,
    VitaAutoUsbReadMetadata *userMetadata
)
{
    VitaAutoUsbReadMetadata metadata;
    int result;

    if (userMetadata == NULL)
        return -1;

    result = readBulkInternal(userBuffer, capacity, &metadata);
    if (result < 0)
        return result;
    if (ksceKernelMemcpyKernelToUser(
            userMetadata,
            &metadata,
            sizeof(metadata)) < 0)
    {
        return -1;
    }
    return result;
}

/* Events are wake hints, not packet counts. After a wake or timeout the caller
 checks the ring again; Wake is also allowed to interrupt an empty read. */
static void waitForBulkReadEvent(uint32_t timeoutUs)
{
    SceUInt timeout = timeoutUs;
    unsigned int matched = 0;

    ksceKernelWaitEventFlag(
        g_readEvent,
        USB_EVENT_RX | USB_EVENT_USER_WAKE,
        SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
        &matched,
        &timeout
    );
}

/* Hold admission through the event wait: Stop wakes us and waits for us to
 leave before closing pipes. The second read sees retirement and returns -1. */
static int readBulkWait(
    void *userBuffer, uint32_t capacity, uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *userMetadata)
{
    int result;
    if (!enterOperation(0))
        return -1;
    result = readBulkTimed(userBuffer, capacity, userMetadata);
    if (result == 0 && timeoutUs != 0 && g_readEvent >= 0) {
        waitForBulkReadEvent(timeoutUs);
        result = readBulkTimed(userBuffer, capacity, userMetadata);
    }
    if (!sessionIsOpen())
        result = -1;
    leaveOperation();
    return result;
}

static int usbReadWaitTimed(
    void *userBuffer, uint32_t capacity, uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *userMetadata)
{
    if (userMetadata == NULL)
        return -1;
    return readBulkWait(userBuffer, capacity, timeoutUs, userMetadata);
}

static int usbWake(void)
{
    int result;
    if (!enterOperation(1))
        return -1;
    result = g_readEvent >= 0
        ? ksceKernelSetEventFlag(g_readEvent, USB_EVENT_USER_WAKE) : -1;
    leaveOperation();
    return result;
}

static int copyRxDiagnostics(VitaAutoUsbRxDiagnostics *userDiagnostics)
{
    VitaAutoUsbRxDiagnostics diagnostics;

    if (userDiagnostics == NULL)
        return -1;

    diagnostics.submittedTransfers = g_bulkShared->readSubmitCount;
    diagnostics.completedTransfers = g_bulkShared->readCompleteCount;
    diagnostics.publishedPackets = g_bulkShared->readPublishedCount;
    diagnostics.consumedPackets = g_bulkShared->readConsumedCount;
    diagnostics.overflowCount = 0; /* Retained ABI field; capacity is reserved. */
    diagnostics.checksumFailures = g_bulkShared->readChecksumFailures;
    diagnostics.sequenceFailures = g_bulkShared->readSequenceFailures;
    diagnostics.maxRingDepth = g_bulkShared->readMaxDepth;
    diagnostics.ringDepth = bulkReadRingDepth(
        g_bulkShared->readHead,
        g_bulkShared->readTail
    );
    diagnostics.readArmed = (uint32_t)g_bulkShared->readArmed;
    diagnostics.submittedWrites = g_bulkShared->writeSubmitCount;
    diagnostics.completedWrites = g_bulkShared->writeCompleteCount;
    diagnostics.writeFailures = g_bulkShared->writeFailureCount;
    diagnostics.writeBusy = (uint32_t)g_bulkShared->writeBusy;
    diagnostics.lastWriteResult = g_bulkShared->writeLastResult;
    diagnostics.lastWriteBytes = g_bulkShared->writeLastBytes;

    return ksceKernelMemcpyKernelToUser(
        userDiagnostics,
        &diagnostics,
        sizeof(diagnostics)
    );
}

static int writeBulkInternal(const void *userBuffer, uint32_t length)
{
    if (userBuffer == NULL || length == 0 || length > AA_BULK_BUFFER_SIZE ||
        g_bulkWriteBuffer == NULL)
        return -1;

    if (g_usb.state != USB_AA_VERSION_COMPLETE ||
        g_usb.bulkOutPipe < 0 ||
        g_bulkShared->writeBusy) {
        return 0;
    }

    if (ksceKernelMemcpyUserToKernel(g_bulkWriteBuffer, userBuffer, length) < 0)
        return -1;

    g_bulkShared->writeBusy = 1;
    g_bulkShared->writeExpectedLength = length;
    ++g_bulkShared->writeSubmitCount;
    if (queueBulkTransfer(
            g_usb.bulkOutPipe,
            g_bulkWriteBuffer,
            length,
            aaBulkWriteDone,
            NULL
        ) < 0) {
        g_bulkShared->writeBusy = 0;
        return -1;
    }

    return (int)length;
}

static int usbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *userDiagnostics)
{
    int result;
    if (!enterOperation(1))
        return -1;
    result = copyRxDiagnostics(userDiagnostics);
    leaveOperation();
    return result;
}

static int usbWrite(const void *userBuffer, uint32_t length)
{
    int result;
    if (!enterOperation(0))
        return -1;
    result = writeBulkInternal(userBuffer, length);
    if (!sessionIsOpen())
        result = -1;
    leaveOperation();
    return result;
}

/* Vita syscall boundary. SDK services must run with the kernel thread-state
 guard, especially Stop's pipe operations and waits. Module entry points and
 USBD callbacks are already kernel calls and use the internal helpers above. */

static int usbDrainReads(void)
{
    uint64_t startedAt;
    int result = 0;
    if (!enterOperation(0))
        return -1;
    __sync_lock_test_and_set(&g_bulkReadsDraining, 1);
    startedAt = ksceKernelGetSystemTimeWide();
    while (__sync_fetch_and_add(&g_bulkReadSchedulerBusy, 0)) {
        if (ksceKernelGetSystemTimeWide() - startedAt >= USB_STOP_TIMEOUT_US) {
            result = -1;
            break;
        }
        ksceKernelDelayThread(USB_STOP_POLL_US);
    }
    if (!sessionIsOpen())
        result = -1;
    leaveOperation();
    return result;
}

int ksceVitaAutoUsbDrainReads(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbDrainReads();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbStart(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbStart();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbStop(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbStop();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetState(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetState();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetCurrentHostMode(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetCurrentHostMode();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetAccessoryActivateResult(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetAccessoryActivateResult();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetHostModeResult(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetHostModeResult();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetDriverRegisterResult(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetDriverRegisterResult();
    EXIT_SYSCALL(syscallState);
    return result;
}

unsigned int ksceVitaAutoUsbGetLastVidPid(void)
{
    int syscallState;
    unsigned int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetLastVidPid();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetLastFailureStage(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetLastFailureStage();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetLastFailureResult(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetLastFailureResult();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetLastFailureCount(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetLastFailureCount();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbReadWaitTimed(
    void *userBuffer, uint32_t capacity, uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *userMetadata)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbReadWaitTimed(userBuffer, capacity, timeoutUs, userMetadata);
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbWake(void)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbWake();
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *userDiagnostics)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbGetRxDiagnostics(userDiagnostics);
    EXIT_SYSCALL(syscallState);
    return result;
}

int ksceVitaAutoUsbWrite(const void *userBuffer, uint32_t length)
{
    int syscallState;
    int result;
    ENTER_SYSCALL(syscallState);
    result = usbWrite(userBuffer, length);
    EXIT_SYSCALL(syscallState);
    return result;
}
