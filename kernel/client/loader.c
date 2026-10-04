/* Linked into the app executable, not the SUPRX. This bootstraps the user-mode
 bridge after the kernel plugin is present; the bridge exports a no-op with
 the same name for callers that already have the module loaded. */

#include "vita_auto_usb.h"

#include <psp2/kernel/modulemgr.h>

int vitaAutoUsbClientLoad(const char *suprxPath)
{
    static SceUID loadedModule = -1;
    int startStatus = 0;

    if (suprxPath == NULL || suprxPath[0] == '\0')
        return -1;


    // The session owner calls this serially. The bridge stays loaded for the
    // app's lifetime. reopening USB must not load another
    if (loadedModule >= 0)
        return 0;

    const SceUID module = sceKernelLoadStartModule(
        suprxPath,
        0,
        NULL,
        0,
        NULL,
        &startStatus
    );

    if (module < 0)
        return (int)module;
    if (startStatus < 0) {
        sceKernelStopUnloadModule(module, 0, NULL, 0, NULL, NULL);
        return startStatus;
    }
    loadedModule = module;
    return startStatus;
}
