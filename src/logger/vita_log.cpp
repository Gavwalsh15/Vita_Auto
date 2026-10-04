#include "vita_log.h"
#include "app/config.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
 
namespace {
// Log Errors and stuff to see how far we are getting
SceUID g_logFile = -1;

void writeLogLine(const char* text, bool reset)
{
    if (reset && g_logFile >= 0) {
        sceIoClose(g_logFile);
        g_logFile = -1;
    }

    if (g_logFile < 0) {
        sceIoMkdir("ux0:data", 0777);
        sceIoMkdir(app::config::DATA_DIR, 0777);
        const int flags = SCE_O_WRONLY | SCE_O_CREAT |
            (reset ? SCE_O_TRUNC : SCE_O_APPEND);
        g_logFile = sceIoOpen(app::config::LOG_PATH, flags, 0777);
    }
    if (g_logFile < 0)
        return;

    char output[258];
    const size_t length = std::strlen(text);
    const size_t copyLength = length < sizeof(output) - 2
        ? length
        : sizeof(output) - 2;
    std::memcpy(output, text, copyLength);
    output[copyLength] = '\n';
    sceIoWrite(g_logFile, output, static_cast<SceSize>(copyLength + 1));
}

}

void vitaLogReset(void)
{
    writeLogLine("Vita Auto log start", true);
}

void vitaLog(const char* format, ...)
{
    char line[256];
    va_list args;

    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    line[sizeof(line) - 1] = '\0';
    writeLogLine(line, false);
}

void vitaLogAppError(app::AppError error, int detail)
{
    vitaLog("app.error code=%d name=%s detail=0x%08x",
            static_cast<int>(error), app::appErrorName(error),
            static_cast<unsigned int>(detail));
}
