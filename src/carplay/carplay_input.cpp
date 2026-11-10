#include "carplay/carplay_session_impl.h"

#include "inputs/vita_input.h"
#include "video/input_mark.h"

#include <cstring>

namespace carplay {
namespace {

// HID media report values: the index of the usage in the media descriptor.
constexpr uint8_t MEDIA_PLAY_PAUSE = 3;
constexpr uint8_t MEDIA_NEXT = 4;
constexpr uint8_t MEDIA_PREVIOUS = 5;

// inputs::drainTo takes a plain function, so the session being drained is kept here.
Session::Impl* g_draining = nullptr;

bool inputSink(const inputs::InputEvent& event)
{
    return g_draining != nullptr && g_draining->sendInput(event);
}

} // namespace

bool Session::Impl::sendHid(const char* uid, const uint8_t* report, size_t size)
{
    if (conn == nullptr || !conn->events.ready() || fds[Event] < 0)
        return false;
    Bytes out;
    if (!conn->events.hidReport(uid, report, size, out))
        return false;
    queue(Event, out);
    ++hidSent;
    return true;
}

bool Session::Impl::sendTouch(const inputs::TouchEvent& touch)
{
    // The screen declares two contacts. The event lists every finger the phone
    // knows about, and for a lift the finger that is leaving is still in it.
    bool down[TOUCH_CONTACTS] = {};
    for (uint32_t i = 0; i < TOUCH_CONTACTS && i < touch.count; ++i) {
        down[i] = true;
        touchX[i] = touch.pointers[i].x;
        touchY[i] = touch.pointers[i].y;
    }
    if (touch.action == inputs::TouchAction::Up) {
        std::memset(down, 0, sizeof(down));
    } else if (touch.action == inputs::TouchAction::PointerUp && touch.actionIndex < TOUCH_CONTACTS) {
        down[touch.actionIndex] = false;
    }
    uint8_t report[TOUCH_CONTACTS * 6];
    for (uint32_t i = 0; i < TOUCH_CONTACTS; ++i) {
        uint8_t* p = report + i * 6;
        p[0] = static_cast<uint8_t>(i);
        p[1] = down[i] ? 1 : 0;
        p[2] = static_cast<uint8_t>(touchX[i]);
        p[3] = static_cast<uint8_t>(touchX[i] >> 8);
        p[4] = static_cast<uint8_t>(touchY[i]);
        p[5] = static_cast<uint8_t>(touchY[i] >> 8);
    }
    return sendHid(HID_TOUCH, report, sizeof(report));
}

bool Session::Impl::sendKnob(const nbtplay::KnobState& state)
{
    uint8_t report[4];
    nbtplay::knob_report(state, report);
    return sendHid(HID_KNOB, report, sizeof(report));
}

bool Session::Impl::sendButton(const inputs::ButtonEvent& button)
{
    using inputs::ButtonCommand;
    nbtplay::KnobState next = knob;
    const bool pressed = button.pressed;
    switch (button.command) {
    case ButtonCommand::Select: next.select = pressed; break;
    case ButtonCommand::Back: next.back = pressed; break;
    case ButtonCommand::Home: next.home = pressed; break;
    case ButtonCommand::Up: next.y = pressed ? -1 : 0; break;
    case ButtonCommand::Down: next.y = pressed ? 1 : 0; break;
    case ButtonCommand::Left: next.x = pressed ? -1 : 0; break;
    case ButtonCommand::Right: next.x = pressed ? 1 : 0; break;
    case ButtonCommand::PreviousItem:
    case ButtonCommand::NextItem: {
        // Rotary steps arrive as presses only; each is one detent.
        if (!pressed)
            return true;
        nbtplay::KnobState step;
        step.wheel = button.command == ButtonCommand::NextItem ? 1 : -1;
        return sendKnob(step) && sendKnob(nbtplay::KnobState{});
    }
    case ButtonCommand::PlayPause:
    case ButtonCommand::Next:
    case ButtonCommand::Previous: {
        const uint8_t value = button.command == ButtonCommand::PlayPause ? MEDIA_PLAY_PAUSE
                            : button.command == ButtonCommand::Next ? MEDIA_NEXT : MEDIA_PREVIOUS;
        const uint8_t report = pressed ? value : 0;
        return sendHid(HID_MEDIA, &report, 1);
    }
    default:
        return true; // The voice key has no CarPlay mapping yet.
    }
    if (!sendKnob(next))
        return false;
    knob = next;
    return true;
}

bool Session::Impl::sendInput(const inputs::InputEvent& event)
{
    return event.type == inputs::InputEventType::Touch ? sendTouch(event.touch)
                                                       : sendButton(event.button);
}

void Session::Impl::drainInput()
{
    if (conn == nullptr || !conn->events.ready())
        return;
    video::InputMark mark;
    mark.begin();
    g_draining = this;
    inputs::drainTo(&inputSink);
    g_draining = nullptr;
    mark.end();
}

} // namespace carplay
