// Тесты логики слотов и разбора ответов коннекторов — на хосте, без сети.
//
// slots.cpp и connectors.cpp подключаются исходниками, а не линкуются
// отдельно: по умолчанию PlatformIO не собирает файлы из src_dir в тестовый
// бинарник (test_build_src не включён в platformio.ini, а трогать его не
// входит в эту задачу), так что единственный способ протестировать код из
// firmware/src на хосте — включить его прямо сюда.
#include <unity.h>

#include "../../src/slots.cpp"
#include "../../src/connectors.cpp"

void setUp() {}
void tearDown() {}

// ── slots::Slot::fresh / stale ──

static void test_fresh_within_ttl() {
    slots::Slot s;
    s.ok = true;
    s.at = 100;
    s.ttl = 60;
    TEST_ASSERT_TRUE(s.fresh(120));
    TEST_ASSERT_FALSE(s.stale(120));
}

static void test_stale_after_ttl() {
    slots::Slot s;
    s.ok = true;
    s.at = 100;
    s.ttl = 60;
    TEST_ASSERT_FALSE(s.fresh(200));
    TEST_ASSERT_TRUE(s.stale(200));
}

// Граница at+ttl: ровно на ней слот уже не свежий, но уже устаревший — без
// этих тестов мутации `<`→`<=` в fresh() и `>=`→`>` в stale() выживают.
static void test_fresh_boundary_exactly_at_ttl_is_not_fresh() {
    slots::Slot s;
    s.ok = true;
    s.at = 100;
    s.ttl = 60;
    TEST_ASSERT_FALSE(s.fresh(160));
    TEST_ASSERT_TRUE(s.stale(160));
}

static void test_fresh_boundary_one_second_before_ttl_is_fresh() {
    slots::Slot s;
    s.ok = true;
    s.at = 100;
    s.ttl = 60;
    TEST_ASSERT_TRUE(s.fresh(159));
    TEST_ASSERT_FALSE(s.stale(159));
}

static void test_not_ok_is_neither_fresh_nor_stale() {
    slots::Slot s;
    s.ok = false;
    s.at = 100;
    s.ttl = 60;
    TEST_ASSERT_FALSE(s.fresh(120));
    TEST_ASSERT_FALSE(s.stale(120));
    TEST_ASSERT_TRUE(s.empty());
}

// ── slots::Store ──

static void test_store_put_and_find() {
    slots::Store store;
    slots::Slot s;
    s.ok = true;
    s.text = "45000";
    s.at = 10;
    s.ttl = 60;
    store.put("btc", s);

    const slots::Slot* found = store.find("btc");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("45000", found->text.c_str());
    TEST_ASSERT_TRUE(store.has_fresh("btc", 20));
    TEST_ASSERT_NULL(store.find("missing"));
}

static void test_mark_failed_keeps_value_clears_ok() {
    slots::Store store;
    slots::Slot s;
    s.ok = true;
    s.text = "45000";
    s.number = 45000;
    s.at = 10;
    s.ttl = 60;
    store.put("btc", s, "rates");

    store.mark_failed("rates");

    const slots::Slot* found = store.find("btc");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_FALSE(found->ok);
    // Значение осталось — экран рисует его со знаком устаревания, не дыру.
    TEST_ASSERT_EQUAL_STRING("45000", found->text.c_str());
    TEST_ASSERT_EQUAL_FLOAT(45000, found->number);
}

static void test_mark_failed_does_not_touch_other_connectors() {
    slots::Store store;
    slots::Slot a;
    a.ok = true;
    a.at = 10;
    a.ttl = 60;
    slots::Slot b = a;
    store.put("btc", a, "rates");
    store.put("co2", b, "home");

    store.mark_failed("rates");

    TEST_ASSERT_FALSE(store.find("btc")->ok);
    TEST_ASSERT_TRUE(store.find("co2")->ok);
}

static void test_mark_failed_ignores_slot_without_owner() {
    slots::Store store;
    slots::Slot s;
    s.ok = true;
    s.at = 10;
    s.ttl = 60;
    store.put("btc", s);  // put без connector_id — владелец не зарегистрирован

    store.mark_failed("rates");

    TEST_ASSERT_TRUE(store.find("btc")->ok);
}

// ── connectors::extract_http_path ──

static void test_http_path_nested_object() {
    slots::Slot out;
    bool ok = connectors::extract_http_path(
        "{\"bitcoin\":{\"usd\":45000.5}}", "bitcoin.usd", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_EQUAL_FLOAT(45000.5f, out.number);
    TEST_ASSERT_EQUAL_STRING("45000.5", out.text.c_str());
}

static void test_http_path_array_index() {
    slots::Slot out;
    bool ok = connectors::extract_http_path(
        "{\"items\":[{\"price\":10},{\"price\":20}]}", "items.1.price", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_FLOAT(20, out.number);
}

static void test_http_path_missing_does_not_set_ok() {
    slots::Slot out;
    bool ok = connectors::extract_http_path("{\"bitcoin\":{\"usd\":45000}}", "bitcoin.eur", out);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(out.ok);
}

static void test_http_path_index_out_of_range_does_not_set_ok() {
    slots::Slot out;
    bool ok = connectors::extract_http_path("{\"items\":[1,2]}", "items.5", out);
    TEST_ASSERT_FALSE(ok);
}

static void test_http_path_broken_json_does_not_crash() {
    slots::Slot out;
    bool ok = connectors::extract_http_path("{not json", "a.b", out);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(out.ok);
}

static void test_http_path_string_value() {
    slots::Slot out;
    bool ok = connectors::extract_http_path("{\"status\":\"ok\"}", "status", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("ok", out.text.c_str());
}

// ── connectors::parse_http_response ──

static void test_parse_http_response_multiple_slots() {
    std::vector<config::SlotMapping> map = {
        {"btc", "bitcoin.usd", 300},
        {"usd_rub", "usd.rub", 900},
    };
    auto result = connectors::parse_http_response(
        "{\"bitcoin\":{\"usd\":45000},\"usd\":{\"rub\":95.5}}", map);

    TEST_ASSERT_EQUAL(2, result.size());
    TEST_ASSERT_EQUAL_STRING("btc", result[0].id.c_str());
    TEST_ASSERT_EQUAL_FLOAT(45000, result[0].value.number);
    TEST_ASSERT_EQUAL_UINT32(300, result[0].value.ttl);
    TEST_ASSERT_EQUAL_STRING("usd_rub", result[1].id.c_str());
    TEST_ASSERT_EQUAL_FLOAT(95.5f, result[1].value.number);
}

static void test_parse_http_response_broken_json_returns_empty() {
    std::vector<config::SlotMapping> map = {{"btc", "bitcoin.usd", 300}};
    auto result = connectors::parse_http_response("{not json", map);
    TEST_ASSERT_EQUAL(0, result.size());
}

static void test_parse_http_response_skips_missing_path() {
    std::vector<config::SlotMapping> map = {
        {"btc", "bitcoin.usd", 300},
        {"missing", "no.such.path", 300},
    };
    auto result = connectors::parse_http_response("{\"bitcoin\":{\"usd\":45000}}", map);

    TEST_ASSERT_EQUAL(1, result.size());
    TEST_ASSERT_EQUAL_STRING("btc", result[0].id.c_str());
}

// ── connectors::extract_homeassistant_state ──

static void test_homeassistant_state_with_unit() {
    slots::Slot out;
    bool ok = connectors::extract_homeassistant_state(
        "{\"state\":\"612\",\"attributes\":{\"unit_of_measurement\":\"ppm\"}}", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("612 ppm", out.text.c_str());
    TEST_ASSERT_EQUAL_FLOAT(612, out.number);
}

static void test_homeassistant_state_without_unit() {
    slots::Slot out;
    bool ok = connectors::extract_homeassistant_state("{\"state\":\"23.5\"}", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("23.5", out.text.c_str());
}

static void test_homeassistant_missing_state_does_not_set_ok() {
    slots::Slot out;
    bool ok = connectors::extract_homeassistant_state("{\"attributes\":{}}", out);
    TEST_ASSERT_FALSE(ok);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_fresh_within_ttl);
    RUN_TEST(test_stale_after_ttl);
    RUN_TEST(test_fresh_boundary_exactly_at_ttl_is_not_fresh);
    RUN_TEST(test_fresh_boundary_one_second_before_ttl_is_fresh);
    RUN_TEST(test_not_ok_is_neither_fresh_nor_stale);

    RUN_TEST(test_store_put_and_find);
    RUN_TEST(test_mark_failed_keeps_value_clears_ok);
    RUN_TEST(test_mark_failed_does_not_touch_other_connectors);
    RUN_TEST(test_mark_failed_ignores_slot_without_owner);

    RUN_TEST(test_http_path_nested_object);
    RUN_TEST(test_http_path_array_index);
    RUN_TEST(test_http_path_missing_does_not_set_ok);
    RUN_TEST(test_http_path_index_out_of_range_does_not_set_ok);
    RUN_TEST(test_http_path_broken_json_does_not_crash);
    RUN_TEST(test_http_path_string_value);

    RUN_TEST(test_parse_http_response_multiple_slots);
    RUN_TEST(test_parse_http_response_broken_json_returns_empty);
    RUN_TEST(test_parse_http_response_skips_missing_path);

    RUN_TEST(test_homeassistant_state_with_unit);
    RUN_TEST(test_homeassistant_state_without_unit);
    RUN_TEST(test_homeassistant_missing_state_does_not_set_ok);

    return UNITY_END();
}
