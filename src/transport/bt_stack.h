#pragma once

#include <cstdint>

struct VitaAutoBtInfo; // vita_auto_usb.h

class BtStack {
public:

    bool enable(const char* tag, const char* name, const char* recoverOriginal, uint32_t options);
    void disable(const char* tag);
    bool enabled() const { return enabled_; }

    enum class Status { Starting, Ready, Failed };
    Status poll(VitaAutoBtInfo& info);

    int claim();
    void release();
    bool claimed() const { return claimed_; }

private:
    void noteName(const VitaAutoBtInfo& info);
    bool readInfo(VitaAutoBtInfo& info);
    void logEvents();

    const char* tag_ = "bt";
    bool summaryLogged_ = false;
    bool enabled_ = false;
    bool claimed_ = false;
    bool nameChanged_ = false;
    bool nameNoted_ = false;
    bool mismatched_ = false;
};
