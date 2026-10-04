#pragma once

#include <cstdint>

namespace inputs {

enum class TouchAction : uint8_t {
    Down = 0,
    Up = 1,
    Move = 2,
};

enum class ButtonCommand : uint8_t {
    Back = 1,
    Home = 2,
    Up = 3,
    Down = 4,
    Left = 5,
    Right = 6,
    Select = 7,
    PlayPause = 8,
    Next = 9,
    Previous = 10,
    VoiceAssistant = 11,
    PreviousItem = 12,
    NextItem = 13,
};

struct TouchEvent {
    TouchAction action = TouchAction::Move;
    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t pointerId = 0;
};

struct ButtonEvent {
    ButtonCommand command = ButtonCommand::Select;
    bool pressed = true;
    bool repeated = false;
};

enum class InputEventType : uint8_t {
    Touch = 0,
    Button = 1,
};

// Platform agnostic event passed from a frontend
struct InputEvent {
    InputEventType type = InputEventType::Touch;
    uint64_t queuedAtUs = 0;
    TouchEvent touch{};
    ButtonEvent button{};
};

} // namespace inputs
