#include "transport/wireless_status.h"

#include <cstdio>

namespace wireless_status {

namespace {
volatile int g_progress = 0;
volatile int g_failureStage = 0;
volatile int g_nameState = 0;
int g_nameSeq = 0;
char g_originalName[64] = {};
} // namespace

void publish(Progress progress, int failureStage)
{
    __atomic_store_n(&g_failureStage, failureStage, __ATOMIC_RELAXED);
    __atomic_store_n(&g_progress, static_cast<int>(progress), __ATOMIC_RELEASE);
}

// One writer (the bootstrap), one reader (the UI thread). The sequence number
// is odd while the name is being rewritten, so a reader that overlapped a write
// sees the number change (or stay odd) and tries again instead of keeping a
// half-written name.
void publishName(NameState state, const char* original)
{
    __atomic_add_fetch(&g_nameSeq, 1, __ATOMIC_ACQ_REL);
    std::snprintf(g_originalName, sizeof(g_originalName), "%s", original);
    __atomic_store_n(&g_nameState, static_cast<int>(state), __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_nameSeq, 1, __ATOMIC_RELEASE);
}

NameState nameState(char* original, size_t size)
{
    for (;;) {
        const int before = __atomic_load_n(&g_nameSeq, __ATOMIC_ACQUIRE);
        if (before & 1)
            continue;
        const NameState state = static_cast<NameState>(
            __atomic_load_n(&g_nameState, __ATOMIC_ACQUIRE));
        if (original != nullptr && size > 0)
            std::snprintf(original, size, "%s", state == NameState::Changed ? g_originalName : "");
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_nameSeq, __ATOMIC_RELAXED) == before)
            return state;
    }
}

Progress progress()
{
    return static_cast<Progress>(__atomic_load_n(&g_progress, __ATOMIC_ACQUIRE));
}

int failureStage()
{
    return __atomic_load_n(&g_failureStage, __ATOMIC_RELAXED);
}

} // namespace wireless_status
