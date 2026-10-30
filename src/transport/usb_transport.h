#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/app_errors.h"

struct UsbRxTiming {
    uint64_t firstCompletedAtUs = 0;
    uint64_t lastCompletedAtUs = 0;
};

// User-mode half of the Android USB transport. The kernel plugin
// owns USB enumeration and bulk endpoints; this class only exchanges bytes
// once that plugin has completed the AA version handshake.
class UsbTransport {
public:
    static constexpr size_t MAX_TRANSFER_SIZE = 16 * 1024;

    // Open once per attempt. The IO owner must close even after failure or cancellation.
    bool open(bool (*cancelled)());
    // Call on the session owner thread, after producers have stopped. Keeps the
    // bridge loaded; failed cleanup can be retried with another close().
    // Returns the stop result (negative on failure), independently of lastError().
    int close();
    bool send(const uint8_t* data, size_t size);

    // Returns 1 for a packet, 0 while no completed USB read is available, and
    // -1 after a terminal kernel/USB failure.
    int receive(
        std::vector<uint8_t>& data,
        UsbRxTiming* timing = nullptr
    );

    // Wake the blocking receive owner when another user thread queues control
    // traffic. Safe before/after a connection; failures are intentionally
    // ignored by callers because the receive timeout remains a safety net.
    static void wake();

    void logRxDiagnostics(const char* reason) const;
    // Rate-limited liveness line: proves the IO loop is running even while no
    // frames arrive, and shows how long the stream has been quiet.
    void logHeartbeat(uint32_t videoFrames);

    bool connected() const;
    app::AppError lastAppError() const { return lastAppError_; }
    int lastError() const { return lastError_; }

private:
    bool fail(app::AppError error, int detail = 0);
    app::AppError lastAppError_ = app::AppError::None;
    int lastError_ = 0;
    bool connected_ = false;
    bool sessionStarted_ = false;
    uint64_t lastRxAtUs_ = 0;
    uint64_t nextHeartbeatUs_ = 0;
};
