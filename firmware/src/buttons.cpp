#include "buttons.h"

namespace buttons {

Kind update(ButtonState& state, bool level, uint32_t now_ms) {
    if (level != state.raw_pressed) {
        state.raw_pressed = level;
        state.raw_changed_at = now_ms;
    }

    if (now_ms - state.raw_changed_at >= kDebounceMs &&
        state.debounced_pressed != state.raw_pressed) {
        state.debounced_pressed = state.raw_pressed;
        if (state.debounced_pressed) {
            state.pressed_since = now_ms;
            state.long_fired = false;
        } else if (!state.long_fired) {
            // Отпустили после короткого нажатия — долгое по этому нажатию
            // ещё не отдавалось, значит оно и было коротким.
            return Kind::kShort;
        }
    }

    if (state.debounced_pressed && !state.long_fired &&
        now_ms - state.pressed_since >= kLongPressMs) {
        state.long_fired = true;
        return Kind::kLong;
    }

    return Kind::kNone;
}

}  // namespace buttons

// begin()/poll() — реальные пины, только на устройстве (см. board.h). На
// хосте (NATIVE_BUILD) их не собирают вовсе, ровно как poll_due в
// connectors.cpp: тестируется только чистая update() выше.
#ifndef NATIVE_BUILD

#include <Arduino.h>

#include "board.h"

namespace buttons {

namespace {

ButtonState g_state[3];
constexpr int8_t kPins[3] = {board::BUTTON_1, board::BUTTON_2, board::BUTTON_3};

}  // namespace

void begin() {
    for (int8_t pin : kPins) pinMode(pin, INPUT_PULLUP);
}

Event poll(uint32_t now_ms) {
    for (uint8_t i = 0; i < 3; ++i) {
        const bool level = digitalRead(kPins[i]) == LOW;  // LOW при нажатии, см. board.h
        const Kind kind = update(g_state[i], level, now_ms);
        if (kind != Kind::kNone) return Event{kind, i};
    }
    return Event{};
}

}  // namespace buttons

#endif  // NATIVE_BUILD
