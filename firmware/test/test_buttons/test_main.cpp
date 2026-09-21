// Тесты чистой логики кнопок (buttons::update) — антидребезг, короткое/
// долгое нажатие. begin()/poll() читают реальные GPIO и на хосте не
// собираются (#ifndef NATIVE_BUILD в buttons.cpp, тот же приём, что poll_due
// в connectors.cpp) — здесь тестируется только update().
//
// buttons.h/.cpp не используют Arduino String — своего Arduino.h-шима
// (как у test_config/test_slots) этому тесту не нужно.
#include <unity.h>

#include "../../src/buttons.cpp"

void setUp() {}
void tearDown() {}

static int k(buttons::Kind kind) { return static_cast<int>(kind); }

// ── короткое нажатие ──

static void test_short_press_fires_on_release() {
    buttons::ButtonState s;
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(buttons::update(s, true, 0)));
    // Внутри окна антидребезга (30 мс) — состояние ещё не подтверждено.
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(buttons::update(s, true, 10)));
    // Антидребезг прошёл — нажатие подтверждено, но событие ещё не событие
    // «нажали», это просто внутреннее состояние.
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(buttons::update(s, true, 35)));
    TEST_ASSERT_TRUE(s.debounced_pressed);
    // Отпустили быстро — kShort приходит по отпусканию, тоже после своего
    // антидребезга.
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(buttons::update(s, false, 40)));
    TEST_ASSERT_EQUAL(k(buttons::Kind::kShort), k(buttons::update(s, false, 70)));
}

static void test_debounce_boundary_exactly_at_threshold_confirms() {
    buttons::ButtonState s;
    buttons::update(s, true, 0);
    buttons::update(s, true, 30);  // ровно порог kDebounceMs
    TEST_ASSERT_TRUE(s.debounced_pressed);
}

static void test_debounce_one_ms_before_threshold_does_not_confirm() {
    buttons::ButtonState s;
    buttons::update(s, true, 0);
    buttons::update(s, true, 29);
    TEST_ASSERT_FALSE(s.debounced_pressed);
}

static void test_bounce_within_debounce_window_is_ignored() {
    buttons::ButtonState s;
    buttons::update(s, true, 0);
    buttons::update(s, false, 5);   // дребезг
    buttons::update(s, true, 10);   // дребезг
    // Ни один уровень не продержался 30 мс подряд — состояние не менялось.
    buttons::Kind result = buttons::update(s, true, 15);
    TEST_ASSERT_FALSE(s.debounced_pressed);
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(result));
}

// ── долгое нажатие ──

static void test_long_press_fires_once_at_threshold() {
    buttons::ButtonState s;
    buttons::update(s, true, 0);
    buttons::update(s, true, 35);  // нажатие подтверждено на t=35
    buttons::Kind first = buttons::update(s, true, 35 + buttons::kLongPressMs);
    TEST_ASSERT_EQUAL(k(buttons::Kind::kLong), k(first));
}

static void test_long_press_does_not_repeat_while_held() {
    buttons::ButtonState s;
    buttons::update(s, true, 0);
    buttons::update(s, true, 35);
    buttons::update(s, true, 35 + buttons::kLongPressMs);  // kLong уже отдан
    buttons::Kind second = buttons::update(s, true, 35 + buttons::kLongPressMs + 500);
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(second));
}

static void test_release_after_long_press_does_not_also_fire_short() {
    buttons::ButtonState s;
    buttons::update(s, true, 0);
    buttons::update(s, true, 35);
    buttons::update(s, true, 35 + buttons::kLongPressMs);  // kLong

    buttons::update(s, false, 3300);  // сырой уровень меняется
    buttons::Kind on_release = buttons::update(s, false, 3300 + buttons::kDebounceMs);
    TEST_ASSERT_EQUAL(k(buttons::Kind::kNone), k(on_release));
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_short_press_fires_on_release);
    RUN_TEST(test_debounce_boundary_exactly_at_threshold_confirms);
    RUN_TEST(test_debounce_one_ms_before_threshold_does_not_confirm);
    RUN_TEST(test_bounce_within_debounce_window_is_ignored);

    RUN_TEST(test_long_press_fires_once_at_threshold);
    RUN_TEST(test_long_press_does_not_repeat_while_held);
    RUN_TEST(test_release_after_long_press_does_not_also_fire_short);


    return UNITY_END();
}
