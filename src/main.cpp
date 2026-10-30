#include <psp2/kernel/processmgr.h>

#include "app/application.h"
#include "logger/vita_log.h"

int main()
{
    vitaLogReset();
    vitaLog("build=detach-recover-v1");
    vitaLogInfo("main start");
    vitaLogInfo("mode=DIRECT_USB");
    const int result = app::run();
    sceKernelExitProcess(result);
    return result;
}
