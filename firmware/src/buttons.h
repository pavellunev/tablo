// Три кнопки платы (board.h: BUTTON_1..3, подтянуты к питанию, нажатие даёт
// LOW) — переключение дашборда коротким нажатием, точка доступа по
// требованию долгим (docs/widgets.md, main.cpp).
//
// Антидребезг и разбор «короткое/долгое» — чистая функция update() от
// (уровень пина, millis), без GPIO: тестируется на хосте (test_buttons).
// begin()/poll() читают реальные пины и живут только на устройстве
// (#ifndef NATIVE_BUILD, тот же приём, что poll_due в connectors.cpp).
#pragma once

#include <cstdint>

namespace buttons {

enum class Kind : uint8_t { kNone, kShort, kLong };

struct Event {
    Kind kind = Kind::kNone;
    uint8_t index = 0;  // 0..2 — какая кнопка платы (BUTTON_1..3)
};

// 30 мс — типичный дребезг механической кнопки с запасом, не режет обычное
// быстрое нажатие. 3 с — достаточно долго, чтобы не поднять точку доступа
// случайным долгим тычком, и достаточно быстро, чтобы не ждать вечность.
constexpr uint32_t kDebounceMs = 30;
constexpr uint32_t kLongPressMs = 3000;

// Живёт между вызовами update() — по одному на кнопку (poll() ниже держит
// три штуки, но чистая логика не знает про это, а просто как в netman.cpp
// с current_settings — состояние отдельно от той функции, что его использует
// и решает по нему).
struct ButtonState {
    bool debounced_pressed = false;  // состояние ПОСЛЕ антидребезга
    bool raw_pressed = false;        // сырой уровень с прошлого вызова
    uint32_t raw_changed_at = 0;     // когда сырой уровень последний раз менялся
    uint32_t pressed_since = 0;      // когда debounced_pressed стало true
    bool long_fired = false;         // долгое уже отдано за это нажатие — не повторять
};

// level=true — кнопка нажата (поднимающий код уже инвертировал LOW->true).
// kShort — по отпусканию после короткого нажатия; kLong — один раз при
// удержании >= kLongPressMs (повторных kLong, пока держат, не будет — и
// отпускание после kLong не даёт kShort вдогонку). kNone — ничего не
// случилось за этот вызов (внутри окна антидребезга или просто держат).
Kind update(ButtonState& state, bool level, uint32_t now_ms);

// INPUT_PULLUP на трёх пинах платы.
void begin();

// Опрашивает три кнопки платы, возвращает первое случившееся за этот тик
// событие (Kind::kNone — ничего). Зовётся из главного цикла (main.cpp).
Event poll(uint32_t now_ms);

}  // namespace buttons
