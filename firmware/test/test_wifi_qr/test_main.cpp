// Тесты wifi_qr::payload() — построение строки формата WIFI: для QR-кода
// подключения к точке доступа (docs/decisions.md, п.8). Само кодирование QR
// (ricmoo/QRCode) не наш код и не тестируется юнит-тестами — его декодирует
// внешний сканер по PNG из tools/render_frame (см. Status Log в
// .claude/plans/inkroam.md).
//
// wifi_qr.cpp подключается исходником — тот же приём, что и в test_config/
// test_slots (test_build_src не включён, см. их комментарий).
#include <unity.h>

#include <cstring>

#include "../../src/wifi_qr.cpp"

void setUp() {}
void tearDown() {}

static void test_payload_basic_format() {
    String p = wifi_qr::payload("inkroam-setup", "23456789AB");

    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:inkroam-setup;P:23456789AB;;", p.c_str());
}

// Точка с запятой в SSID — минимальный, но настоящий случай: без экранирования
// она читалась бы как разделитель полей, и телефон получил бы SSID "a" вместо
// "a;b", а остаток строки — "b" — уполз бы в поле пароля.
static void test_payload_escapes_semicolon_in_ssid() {
    String p = wifi_qr::payload("a;b", "pass");

    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\;b;P:pass;;", p.c_str());
}

static void test_payload_escapes_backslash_comma_and_quote() {
    String p = wifi_qr::payload("a\\b,c\"d", "pass");

    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\\\b\\,c\\\"d;P:pass;;", p.c_str());
}

// Пароль генерируется из фиксированного алфавита без спецсимволов (config.cpp,
// kApPasswordAlphabet), но payload() экранирует оба поля симметрично — тест
// проверяет ровно то же правило со стороны пароля, а не только SSID.
static void test_payload_escapes_special_chars_in_password() {
    String p = wifi_qr::payload("home", "a;b");

    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:home;P:a\\;b;;", p.c_str());
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_payload_basic_format);
    RUN_TEST(test_payload_escapes_semicolon_in_ssid);
    RUN_TEST(test_payload_escapes_backslash_comma_and_quote);
    RUN_TEST(test_payload_escapes_special_chars_in_password);

    return UNITY_END();
}
