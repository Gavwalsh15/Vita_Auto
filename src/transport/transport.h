#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/app_errors.h"

struct UsbRxTiming {
    uint64_t firstCompletedAtUs = 0;
    uint64_t lastCompletedAtUs = 0;
};

// Result of one non-blocking step while waiting for a phone to appear.
enum class OpenPoll { Waiting, Connected, Failed };

struct TransportDrainState {
    uint32_t readsArmed = 0;
    bool writeBusy = false;
};

class Transport {
public:
    static constexpr size_t MAX_TRANSFER_SIZE = 16 * 1024;

    virtual ~Transport() = default;

    virtual bool open(bool (*cancelled)()) = 0;
    // Call on the session owner thread, after producers have stopped.
    virtual int close() = 0;
    virtual bool send(const uint8_t* data, size_t size) = 0;

    // Returns 1 for a packet, 0 while no data is available, and -1 after a
    // terminal transport failure.
    virtual int receive(
        std::vector<uint8_t>& data,
        UsbRxTiming* timing = nullptr
    ) = 0;

    virtual bool connected() const = 0;

    virtual bool versionNegotiated() const = 0;

    virtual int beginShutdownDrain() { return 0; }
    virtual int shutdownDrainState(TransportDrainState& state)
    {
        state = TransportDrainState{};
        return 0;
    }

    virtual void logDiagnostics(const char*) const {}
    virtual void logHeartbeat(uint32_t) {}
    virtual bool wireless() const { return false; }

    virtual uint32_t mediaMaxUnacked() const { return 1; }

    virtual app::AppError lastAppError() const { return lastAppError_; }
    virtual int lastError() const { return lastError_; }

    using WakeHandler = void (*)();
    static void setWakeHandler(WakeHandler handler);
    static void wake();

protected:
    bool fail(app::AppError error, int detail = 0);
    void clearError()
    {
        lastAppError_ = app::AppError::None;
        lastError_ = 0;
    }

private:
    app::AppError lastAppError_ = app::AppError::None;
    int lastError_ = 0;
};
