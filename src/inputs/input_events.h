#pragma once

#include <cstdint>

namespace inputs {

// The numbers are Android Auto's own pointer actions.
enum class TouchAction : uint8_t {
    Down = 0,        // The first finger lands
    Up = 1,          // The last finger lifts
    Move = 2,
    PointerDown = 5, // Another finger lands while others are down
    PointerUp = 6,   // A finger lifts while others stay down
};

// The Vita's front panel reports at most six fingers.
constexpr uint8_t MAX_TOUCH_POINTERS = 6;

struct TouchPointer {
    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t id = 0; // Stable for as long as the finger stays down
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

// Like an Android motion event: every finger currently down, plus which one the
// action is about (PointerDown and PointerUp only).
struct TouchEvent {
    TouchAction action = TouchAction::Move;
    uint8_t actionIndex = 0;
    uint8_t count = 0;
    TouchPointer pointers[MAX_TOUCH_POINTERS]{};
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
