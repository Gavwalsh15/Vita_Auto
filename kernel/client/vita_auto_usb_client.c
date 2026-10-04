/*
    User mode SUPRX bridge. Keep these wrappers explicit so the API and
    kernel imports are easy to compare
*/
#include "vita_auto_usb.h"
#include "kernel_imports.h"

#include <psp2/kernel/modulemgr.h>

int module_start(SceSize argc, const void *args);
int _start(SceSize argc, const void *args)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}

int vitaAutoUsbClientLoad(const char *suprxPath)
{
    /* The executable links loader.c to load this module. Once inside the
        loaded bridge, the same exported entry point has nothing left to load. */
    (void)suprxPath;
    return 0;
}

int vitaAutoUsbGetState(void)
{
    return ksceVitaAutoUsbGetState();
}

int vitaAutoUsbReadWaitTimed(
    void *buffer,
    uint32_t capacity,
    uint32_t timeoutUs,
    VitaAutoUsbReadMetadata *metadata
)
{
    return ksceVitaAutoUsbReadWaitTimed(
        buffer,
        capacity,
        timeoutUs,
        metadata
    );
}

int vitaAutoUsbWake(void)
{
    return ksceVitaAutoUsbWake();
}

int vitaAutoUsbWrite(const void *buffer, uint32_t length)
{
    return ksceVitaAutoUsbWrite(buffer, length);
}

int vitaAutoUsbGetAccessoryActivateResult(void)
{
    return ksceVitaAutoUsbGetAccessoryActivateResult();
}

int vitaAutoUsbGetHostModeResult(void)
{
    return ksceVitaAutoUsbGetHostModeResult();
}

int vitaAutoUsbGetDriverRegisterResult(void)
{
    return ksceVitaAutoUsbGetDriverRegisterResult();
}

unsigned int vitaAutoUsbGetLastVidPid(void)
{
    return ksceVitaAutoUsbGetLastVidPid();
}

int vitaAutoUsbGetLastFailureStage(void)
{
    return ksceVitaAutoUsbGetLastFailureStage();
}

int vitaAutoUsbGetLastFailureResult(void)
{
    return ksceVitaAutoUsbGetLastFailureResult();
}

int vitaAutoUsbGetLastFailureCount(void)
{
    return ksceVitaAutoUsbGetLastFailureCount();
}

int vitaAutoUsbStart(void)
{
    return ksceVitaAutoUsbStart();
}

int vitaAutoUsbStop(void)
{
    return ksceVitaAutoUsbStop();
}

int vitaAutoUsbDrainReads(void)
{
    return ksceVitaAutoUsbDrainReads();
}

int vitaAutoUsbGetCurrentHostMode(void)
{
    return ksceVitaAutoUsbGetCurrentHostMode();
}

int vitaAutoUsbGetRxDiagnostics(VitaAutoUsbRxDiagnostics *diagnostics)
{
    return ksceVitaAutoUsbGetRxDiagnostics(diagnostics);
}
