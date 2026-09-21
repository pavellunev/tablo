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

// ── slots::Store — история для спарклайна: только явная, от put_history() ──
//
// Раньше put() копил историю сам из последовательных значений — три теста
// ниже проверяли именно это накопление. Задача убрала его как источник
// графика (пять минут между опросами — не «сутки», см. Status Log): put()
// теперь ничего не накапливает, историю кладёт только put_history(), и put()
// её лишь сохраняет, если сам не несёт своей (history_len==0 у входного Slot).

static void test_history_empty_on_first_put() {
    slots::Store store;
    slots::Slot s;
    s.ok = true;
    s.at = 10;
    s.ttl = 60;
    s.number = 100;
    store.put("btc", s);

    TEST_ASSERT_EQUAL_UINT8(0, store.find("btc")->history_len);
}

static void test_put_does_not_accumulate_history_across_calls() {
    slots::Store store;
    slots::Slot s;
    s.ok = true;
    s.ttl = 60;

    s.at = 10;
    s.number = 100;
    store.put("btc", s);
    s.at = 20;
    s.number = 200;
    store.put("btc", s);
    s.at = 30;
    s.number = 300;
    store.put("btc", s);

    // Ни один put() не нёс своей истории — сколько бы раз его ни звали,
    // history_len остаётся 0. Это и есть починка: раньше здесь было бы 2.
    const slots::Slot* found = store.find("btc");
    TEST_ASSERT_EQUAL_UINT8(0, found->history_len);
    TEST_ASSERT_EQUAL_FLOAT(300, found->number);
}

static void test_put_history_sets_history_and_creates_slot() {
    slots::Store store;
    float values[3] = {100.0f, 200.0f, 300.0f};
    store.put_history("btc", values, 3, "btc_history");

    const slots::Slot* found = store.find("btc");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT8(3, found->history_len);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, found->history[0]);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, found->history[2]);
}

static void test_put_history_truncates_to_capacity() {
    slots::Store store;
    float values[slots::Slot::kHistoryCapacity + 5];
    for (uint8_t i = 0; i < slots::Slot::kHistoryCapacity + 5; ++i) values[i] = i;

    store.put_history("btc", values, slots::Slot::kHistoryCapacity + 5, "btc_history");

    TEST_ASSERT_EQUAL_UINT8(slots::Slot::kHistoryCapacity, store.find("btc")->history_len);
}

static void test_put_after_put_history_keeps_history() {
    slots::Store store;
    float values[2] = {10.0f, 20.0f};
    store.put_history("btc", values, 2, "btc_history");

    slots::Slot s;
    s.ok = true;
    s.at = 10;
    s.ttl = 60;
    s.number = 45000;
    store.put("btc", s, "btc");  // текущее значение — от другого коннектора

    const slots::Slot* found = store.find("btc");
    TEST_ASSERT_EQUAL_FLOAT(45000, found->number);
    TEST_ASSERT_EQUAL_UINT8(2, found->history_len);  // история не стёрлась
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

static void test_mark_failed_sets_error_reason() {
    slots::Store store;
    slots::Slot s;
    s.ok = true;
    s.at = 10;
    s.ttl = 60;
    store.put("limit.claude.5h", s, "claude");

    store.mark_failed("claude", "недоступен из этой страны — нужен VPN");

    TEST_ASSERT_EQUAL_STRING("недоступен из этой страны — нужен VPN",
                             store.find("limit.claude.5h")->error.c_str());
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


// ── ответ как словарь слотов ───────────────────────────────────────────

void test_empty_map_reads_slot_dictionary(void) {
    // Так отдаёт домашнее приложение: имя слота → объект со значением.
    // Число и изменение должны доехать целиком, иначе шкалы и графики
    // останутся пустыми при заполненной подписи.
    const char* body =
        "{\"btc\":{\"text\":\"80 689\",\"number\":80689.0,\"delta\":-1.13,\"age\":12,\"ttl\":300},"
        "\"co2\":{\"text\":\"798\",\"number\":798.0,\"delta\":92.0,\"age\":5,\"ttl\":300}}";

    auto parsed = connectors::parse_http_response(String(body), {});

    TEST_ASSERT_EQUAL_UINT32(2, parsed.size());
    for (const auto& p : parsed) {
        if (p.id == String("btc")) {
            TEST_ASSERT_EQUAL_STRING("80 689", p.value.text.c_str());
            TEST_ASSERT_EQUAL_FLOAT(80689.0f, p.value.number);
            TEST_ASSERT_EQUAL_FLOAT(-1.13f, p.value.delta);
            TEST_ASSERT_EQUAL_UINT32(300, p.value.ttl);
            TEST_ASSERT_TRUE(p.value.ok);
        }
    }
}

void test_empty_map_skips_entries_without_text(void) {
    // Слот без подписи показывать нечем — пропускаем, а не рисуем пустоту.
    const char* body = "{\"btc\":{\"number\":1.0},\"co2\":{\"text\":\"798\",\"number\":798.0}}";
    auto parsed = connectors::parse_http_response(String(body), {});
    TEST_ASSERT_EQUAL_UINT32(1, parsed.size());
    TEST_ASSERT_EQUAL_STRING("co2", parsed[0].id.c_str());
}

// ── connectors::parse_claude_usage ──

static void test_parse_claude_usage_both_windows() {
    slots::Slot five_hour, week;
    bool ok = connectors::parse_claude_usage(
        "{\"five_hour\":{\"utilization\":7.0,\"resets_at\":\"x\"},"
        "\"seven_day\":{\"utilization\":28.0,\"resets_at\":\"y\"}}",
        five_hour, week);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(five_hour.ok);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, five_hour.number);
    TEST_ASSERT_EQUAL_STRING("7", five_hour.text.c_str());
    TEST_ASSERT_TRUE(week.ok);
    TEST_ASSERT_EQUAL_FLOAT(28.0f, week.number);
}

static void test_parse_claude_usage_missing_week_keeps_five_hour() {
    slots::Slot five_hour, week;
    bool ok = connectors::parse_claude_usage("{\"five_hour\":{\"utilization\":50.0}}", five_hour,
                                              week);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(five_hour.ok);
    TEST_ASSERT_FALSE(week.ok);
}

static void test_parse_claude_usage_missing_five_hour_fails() {
    slots::Slot five_hour, week;
    bool ok = connectors::parse_claude_usage("{\"seven_day\":{\"utilization\":28.0}}", five_hour,
                                              week);
    TEST_ASSERT_FALSE(ok);
}

static void test_parse_claude_usage_broken_json_fails() {
    slots::Slot five_hour, week;
    bool ok = connectors::parse_claude_usage("{not json", five_hour, week);
    TEST_ASSERT_FALSE(ok);
}

// ── connectors::parse_codex_usage ──

static void test_parse_codex_usage_reads_used_percent() {
    slots::Slot out;
    bool ok = connectors::parse_codex_usage(
        "{\"rate_limit\":{\"primary_window\":{\"used_percent\":54,"
        "\"limit_window_seconds\":604800}}}",
        out);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_EQUAL_FLOAT(54.0f, out.number);
}

static void test_parse_codex_usage_missing_window_fails() {
    slots::Slot out;
    bool ok = connectors::parse_codex_usage("{\"rate_limit\":{}}", out);
    TEST_ASSERT_FALSE(ok);
}

// ── connectors::parse_oauth_refresh ──

static void test_parse_oauth_refresh_rotates_refresh_token() {
    String access, refresh;
    bool ok = connectors::parse_oauth_refresh(
        "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\"}", "old-refresh",
        access, refresh);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("new-access", access.c_str());
    TEST_ASSERT_EQUAL_STRING("new-refresh", refresh.c_str());
}

// Сервер не обязан присылать новый refresh_token на каждый обмен — тогда
// нельзя терять единственный рабочий, подставляя пустую строку.
static void test_parse_oauth_refresh_keeps_previous_when_absent() {
    String access, refresh;
    bool ok = connectors::parse_oauth_refresh("{\"access_token\":\"new-access\"}", "old-refresh",
                                               access, refresh);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("new-access", access.c_str());
    TEST_ASSERT_EQUAL_STRING("old-refresh", refresh.c_str());
}

static void test_parse_oauth_refresh_missing_access_token_fails() {
    String access, refresh;
    bool ok = connectors::parse_oauth_refresh("{\"error\":\"invalid_grant\"}", "old-refresh",
                                               access, refresh);
    TEST_ASSERT_FALSE(ok);
}

// ── connectors::count_imap_unseen ──

static void test_count_imap_unseen_multiple_ids() {
    TEST_ASSERT_EQUAL(3, connectors::count_imap_unseen("* SEARCH 12 45 90"));
}

static void test_count_imap_unseen_empty_mailbox() {
    TEST_ASSERT_EQUAL(0, connectors::count_imap_unseen("* SEARCH"));
}

static void test_count_imap_unseen_not_a_search_line() {
    TEST_ASSERT_EQUAL(-1, connectors::count_imap_unseen("a3 OK SEARCH completed"));
}

// ── connectors::parse_imap_search_ids ──

static void test_parse_imap_search_ids_order_preserved() {
    auto ids = connectors::parse_imap_search_ids("* SEARCH 12 45 90");
    TEST_ASSERT_EQUAL(3, ids.size());
    TEST_ASSERT_EQUAL(12, ids[0]);
    TEST_ASSERT_EQUAL(45, ids[1]);
    TEST_ASSERT_EQUAL(90, ids[2]);
}

static void test_parse_imap_search_ids_not_a_search_line_is_empty() {
    auto ids = connectors::parse_imap_search_ids("a3 OK SEARCH completed");
    TEST_ASSERT_EQUAL(0, ids.size());
}

// ── connectors::decode_mime_header ──

static void test_decode_mime_header_base64_utf8() {
    // "Привет!" в UTF-8, Base64.
    String out = connectors::decode_mime_header("=?UTF-8?B?0J/RgNC40LLQtdGCIQ==?=");
    TEST_ASSERT_EQUAL_STRING("Привет!", out.c_str());
}

static void test_decode_mime_header_quoted_printable_underscore_is_space() {
    String out = connectors::decode_mime_header("=?UTF-8?Q?Hello_World?=");
    TEST_ASSERT_EQUAL_STRING("Hello World", out.c_str());
}

static void test_decode_mime_header_plain_text_unchanged() {
    String out = connectors::decode_mime_header("Just plain text");
    TEST_ASSERT_EQUAL_STRING("Just plain text", out.c_str());
}

static void test_decode_mime_header_two_words_no_space_between() {
    // RFC 2047: пробел/перенос МЕЖДУ двумя encoded-word пропадает.
    String out = connectors::decode_mime_header(
        "=?UTF-8?Q?Hello?= =?UTF-8?Q?World?=");
    TEST_ASSERT_EQUAL_STRING("HelloWorld", out.c_str());
}

// ── connectors::extract_sender_name ──

static void test_extract_sender_name_with_display_name() {
    String out = connectors::extract_sender_name("Alice Smith <alice@example.com>");
    TEST_ASSERT_EQUAL_STRING("Alice Smith", out.c_str());
}

static void test_extract_sender_name_mime_encoded_display_name() {
    String out =
        connectors::extract_sender_name("=?UTF-8?B?0J/QsNCy0LXQuw==?= <pavel@example.com>");
    TEST_ASSERT_EQUAL_STRING("Павел", out.c_str());
}

static void test_extract_sender_name_address_only_uses_local_part() {
    String out = connectors::extract_sender_name("noreply@example.com");
    TEST_ASSERT_EQUAL_STRING("noreply", out.c_str());
}

// ── connectors::extract_mail_headers ──

static void test_extract_mail_headers_all_three_fields() {
    String from, subject, date;
    bool ok = connectors::extract_mail_headers(
        "From: Alice <alice@example.com>\n"
        "Subject: =?UTF-8?B?0J/RgNC40LLQtdGCIQ==?=\n"
        "Date: Mon, 21 Sep 2026 10:15:32 +0500\n",
        from, subject, date);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("Alice <alice@example.com>", from.c_str());
    TEST_ASSERT_EQUAL_STRING("=?UTF-8?B?0J/RgNC40LLQtdGCIQ==?=", subject.c_str());
    TEST_ASSERT_EQUAL_STRING("Mon, 21 Sep 2026 10:15:32 +0500", date.c_str());
}

static void test_extract_mail_headers_case_insensitive_names() {
    String from, subject, date;
    bool ok = connectors::extract_mail_headers("from: a@b.com\nSUBJECT: Hi\n", from, subject, date);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("a@b.com", from.c_str());
    TEST_ASSERT_EQUAL_STRING("Hi", subject.c_str());
}

static void test_extract_mail_headers_folded_continuation_appends() {
    String from, subject, date;
    bool ok = connectors::extract_mail_headers(
        "Subject: Long subject\n line continues\n", from, subject, date);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("Long subject line continues", subject.c_str());
}

static void test_extract_mail_headers_empty_block_fails() {
    String from, subject, date;
    bool ok = connectors::extract_mail_headers("", from, subject, date);
    TEST_ASSERT_FALSE(ok);
}

// ── connectors::parse_rfc822_date / format_mail_time ──

static void test_parse_rfc822_date_with_weekday_and_offset() {
    uint32_t out = 0;
    bool ok = connectors::parse_rfc822_date("Mon, 21 Sep 2026 10:15:32 +0500", out);
    TEST_ASSERT_TRUE(ok);
    // 21 Sep 2026 10:15:32 +05:00 -> 05:15:32 UTC того же дня.
    uint32_t expected;
    connectors::parse_iso8601_utc("2026-09-21T05:15:32Z", expected);
    TEST_ASSERT_EQUAL_UINT32(expected, out);
}

static void test_parse_rfc822_date_without_weekday() {
    uint32_t out = 0;
    bool ok = connectors::parse_rfc822_date("21 Sep 2026 10:15:32 +0000", out);
    TEST_ASSERT_TRUE(ok);
}

static void test_parse_rfc822_date_garbage_fails() {
    uint32_t out = 0;
    TEST_ASSERT_FALSE(connectors::parse_rfc822_date("not a date", out));
}

static void test_format_mail_time_today_is_hh_mm() {
    uint32_t now;
    connectors::parse_iso8601_utc("2026-09-21T12:00:00Z", now);
    uint32_t mail_at;
    connectors::parse_iso8601_utc("2026-09-21T05:15:00Z", mail_at);  // 10:15 при +05:00
    String out = connectors::format_mail_time(mail_at, now, 300);
    TEST_ASSERT_EQUAL_STRING("10:15", out.c_str());
}

static void test_format_mail_time_yesterday() {
    uint32_t now;
    connectors::parse_iso8601_utc("2026-09-21T12:00:00Z", now);
    uint32_t mail_at;
    connectors::parse_iso8601_utc("2026-09-20T12:00:00Z", mail_at);
    String out = connectors::format_mail_time(mail_at, now, 0);
    TEST_ASSERT_EQUAL_STRING("Вчера", out.c_str());
}

static void test_format_mail_time_older_is_day_dot_month() {
    uint32_t now;
    connectors::parse_iso8601_utc("2026-09-21T12:00:00Z", now);
    uint32_t mail_at;
    connectors::parse_iso8601_utc("2026-09-05T12:00:00Z", mail_at);
    String out = connectors::format_mail_time(mail_at, now, 0);
    TEST_ASSERT_EQUAL_STRING("5.09", out.c_str());
}

// ── connectors::remaining_percent ──

static void test_remaining_percent_typical() {
    TEST_ASSERT_EQUAL_FLOAT(46.0f, connectors::remaining_percent(54.0f));
}

static void test_remaining_percent_clamps_below_zero_input() {
    TEST_ASSERT_EQUAL_FLOAT(100.0f, connectors::remaining_percent(-5.0f));
}

static void test_remaining_percent_clamps_above_hundred_input() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, connectors::remaining_percent(150.0f));
}

// ── connectors::parse_iso8601_utc ──

static void test_parse_iso8601_utc_with_fraction_and_zero_offset() {
    uint32_t out = 0;
    bool ok = connectors::parse_iso8601_utc("2026-09-20T23:40:00.036044+00:00", out);
    TEST_ASSERT_TRUE(ok);
    uint32_t expected;
    connectors::parse_iso8601_utc("2026-09-20T23:40:00Z", expected);
    TEST_ASSERT_EQUAL_UINT32(expected, out);
}

static void test_parse_iso8601_utc_negative_offset() {
    uint32_t plus_five;
    connectors::parse_iso8601_utc("2026-09-21T00:00:00+05:00", plus_five);
    uint32_t minus_five;
    connectors::parse_iso8601_utc("2026-09-20T14:00:00-05:00", minus_five);
    // 21 00:00 +05:00 == 20 19:00 UTC; 20 14:00 -05:00 == 20 19:00 UTC.
    TEST_ASSERT_EQUAL_UINT32(plus_five, minus_five);
}

static void test_parse_iso8601_utc_garbage_fails() {
    uint32_t out = 0;
    TEST_ASSERT_FALSE(connectors::parse_iso8601_utc("not a date", out));
}

// ── connectors::parse_claude_reset_times / format_claude_reset ──

static void test_parse_claude_reset_times_both_windows() {
    String five, week;
    bool ok = connectors::parse_claude_reset_times(
        "{\"five_hour\":{\"resets_at\":\"2026-09-21T03:00:00Z\"},"
        "\"seven_day\":{\"resets_at\":\"2026-09-23T20:00:00Z\"}}",
        five, week);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("2026-09-21T03:00:00Z", five.c_str());
    TEST_ASSERT_EQUAL_STRING("2026-09-23T20:00:00Z", week.c_str());
}

static void test_parse_claude_reset_times_missing_five_hour_fails() {
    String five, week;
    bool ok = connectors::parse_claude_reset_times("{\"seven_day\":{\"resets_at\":\"x\"}}", five,
                                                    week);
    TEST_ASSERT_FALSE(ok);
}

static void test_format_claude_reset_both_windows() {
    uint32_t now, five_reset, week_reset;
    connectors::parse_iso8601_utc("2026-09-20T00:00:00Z", now);
    // +3ч6мин -> "3:06"; +2дн22ч -> "2 дн 22 ч".
    five_reset = now + 3 * 3600 + 6 * 60;
    week_reset = now + 2 * 86400 + 22 * 3600;

    String out = connectors::format_claude_reset(five_reset, true, week_reset, true, now);
    TEST_ASSERT_EQUAL_STRING("5 ч через 3:06 · неделя через 2 дн 22 ч", out.c_str());
}

static void test_format_claude_reset_only_five_hour() {
    uint32_t now = 1000;
    uint32_t five_reset = now + 3661;  // 1:01
    String out = connectors::format_claude_reset(five_reset, true, 0, false, now);
    TEST_ASSERT_EQUAL_STRING("5 ч через 1:01", out.c_str());
}

static void test_format_claude_reset_neither_window_is_empty() {
    String out = connectors::format_claude_reset(0, false, 0, false, 1000);
    TEST_ASSERT_EQUAL_STRING("", out.c_str());
}

// ── connectors::parse_codex_reset / format_codex_reset ──

static void test_parse_codex_reset_uses_reset_at() {
    uint32_t out = 0;
    bool ok = connectors::parse_codex_reset(
        "{\"rate_limit\":{\"primary_window\":{\"reset_at\":2000000000}}}", 1000, out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(2000000000, out);
}

static void test_parse_codex_reset_falls_back_to_reset_after_seconds() {
    uint32_t out = 0;
    bool ok = connectors::parse_codex_reset(
        "{\"rate_limit\":{\"primary_window\":{\"reset_after_seconds\":500}}}", 1000, out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(1500, out);
}

static void test_parse_codex_reset_missing_window_fails() {
    uint32_t out = 0;
    TEST_ASSERT_FALSE(connectors::parse_codex_reset("{\"rate_limit\":{}}", 1000, out));
}

static void test_format_codex_reset_matches_reference_shape() {
    // Подобрано так, чтобы одновременно совпало и «через 2 дн 21 ч» (69 полных
    // часов до сброса), и локальное время сброса 13:01 при смещении +05:00
    // (300 минут): reset-now=250000с -> 69ч (2дн21ч, минуты отбрасываются
    // форматированием), reset+18000с (локальное смещение) -> 306060с от
    // эпохи -> 85 часов -> 85%24=13, 5101 минута -> 5101%60=1.
    uint32_t now = 38060;
    uint32_t reset = 288060;
    String out = connectors::format_codex_reset(reset, now, 300);
    TEST_ASSERT_EQUAL_STRING("неделя · сброс 13:01 · через 2 дн 21 ч", out.c_str());
}

// ── connectors::wmo_to_text ──

static void test_wmo_to_text_clear() { TEST_ASSERT_EQUAL_STRING("ЯСНО", connectors::wmo_to_text(0)); }

static void test_wmo_to_text_rain() {
    TEST_ASSERT_EQUAL_STRING("ДОЖДЬ", connectors::wmo_to_text(61));
}

static void test_wmo_to_text_thunderstorm() {
    TEST_ASSERT_EQUAL_STRING("ГРОЗА", connectors::wmo_to_text(95));
}

static void test_wmo_to_text_unknown_code() {
    TEST_ASSERT_EQUAL_STRING("?", connectors::wmo_to_text(-1));
}

// ── connectors::parse_geocode_response ──

static void test_parse_geocode_response_reads_first_result() {
    float lat = 0, lon = 0;
    String name;
    bool ok = connectors::parse_geocode_response(
        "{\"results\":[{\"latitude\":56.8389,\"longitude\":60.6057,\"name\":\"Yekaterinburg\"}]}",
        lat, lon, name);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_FLOAT(56.8389f, lat);
    TEST_ASSERT_EQUAL_FLOAT(60.6057f, lon);
    TEST_ASSERT_EQUAL_STRING("Yekaterinburg", name.c_str());
}

static void test_parse_geocode_response_empty_results_fails() {
    float lat = 0, lon = 0;
    String name;
    bool ok = connectors::parse_geocode_response("{\"results\":[]}", lat, lon, name);
    TEST_ASSERT_FALSE(ok);
}

static void test_parse_geocode_response_missing_results_fails() {
    float lat = 0, lon = 0;
    String name;
    bool ok = connectors::parse_geocode_response("{}", lat, lon, name);
    TEST_ASSERT_FALSE(ok);
}

// ── connectors::extract_history_array / parse_http_history (BTC klines) ──

static void test_extract_history_array_klines_close_price() {
    // Каждая свеча — массив [open_time, open, high, low, close, ...],
    // "4" — индекс цены закрытия.
    const char* body =
        "[[1,\"1\",\"2\",\"0.5\",\"100.5\",\"9\"],"
        "[2,\"1\",\"2\",\"0.5\",\"101.0\",\"9\"],"
        "[3,\"1\",\"2\",\"0.5\",\"102.25\",\"9\"]]";
    float out[8];
    uint8_t n = connectors::extract_history_array(String(body), "", "4", out, 8);
    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_FLOAT(100.5f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(102.25f, out[2]);
}

static void test_extract_history_array_root_of_scalars() {
    float out[8];
    uint8_t n = connectors::extract_history_array(String("[1,2,3]"), "", "", out, 8);
    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, out[1]);
}

static void test_extract_history_array_nested_under_source_path() {
    float out[8];
    uint8_t n = connectors::extract_history_array(String("{\"prices\":[10,20,30]}"), "prices", "",
                                                   out, 8);
    TEST_ASSERT_EQUAL_UINT8(3, n);
}

static void test_extract_history_array_truncates_to_max_len() {
    float out[2];
    uint8_t n = connectors::extract_history_array(String("[1,2,3,4,5]"), "", "", out, 2);
    TEST_ASSERT_EQUAL_UINT8(2, n);
}

static void test_extract_history_array_not_an_array_returns_zero() {
    float out[8];
    uint8_t n = connectors::extract_history_array(String("{\"a\":1}"), "", "", out, 8);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void test_parse_http_history_skips_non_history_mappings() {
    std::vector<config::SlotMapping> map = {{"btc", "lastPrice", 900}};
    auto result = connectors::parse_http_history(String("{\"lastPrice\":\"1\"}"), map);
    TEST_ASSERT_EQUAL(0, result.size());
}

static void test_parse_http_history_reads_klines_into_btc_slot() {
    config::SlotMapping m;
    m.slot = "btc";
    m.has_history = true;
    m.history_item = "4";
    std::vector<config::SlotMapping> map = {m};

    auto result = connectors::parse_http_history(
        String("[[1,\"1\",\"2\",\"0.5\",\"100\",\"9\"],[2,\"1\",\"2\",\"0.5\",\"110\",\"9\"]]"),
        map);
    TEST_ASSERT_EQUAL(1, result.size());
    TEST_ASSERT_EQUAL_STRING("btc", result[0].id.c_str());
    TEST_ASSERT_EQUAL_UINT8(2, result[0].count);
    TEST_ASSERT_EQUAL_FLOAT(110.0f, result[0].values[1]);
}

// ── connectors::decimate ──

static void test_decimate_fewer_points_than_target_copies_all() {
    float in[3] = {1, 2, 3};
    float out[10];
    uint8_t n = connectors::decimate(in, 3, out, 10);
    TEST_ASSERT_EQUAL_UINT8(3, n);
}

static void test_decimate_more_points_reduces_evenly() {
    float in[10];
    for (uint8_t i = 0; i < 10; ++i) in[i] = i;
    float out[5];
    uint8_t n = connectors::decimate(in, 10, out, 5);
    TEST_ASSERT_EQUAL_UINT8(5, n);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[0]);
    // Последняя точка выхода — последняя точка входа (9), не предпоследняя (8):
    // прежняя формула i·in/out теряла конец ряда, на живых данных это были
    // свежие ~15 минут истории.
    TEST_ASSERT_EQUAL_FLOAT(9.0f, out[4]);
}

// ── connectors::extract_ha_history_values (Home Assistant /api/history/period) ──

static void test_extract_ha_history_values_reads_state_field() {
    const char* body =
        "[[{\"state\":\"612\",\"last_changed\":\"t1\"},"
        "{\"state\":\"650\",\"last_changed\":\"t2\"}]]";
    float out[8];
    uint16_t n = connectors::extract_ha_history_values(String(body), out, 8);
    TEST_ASSERT_EQUAL_UINT16(2, n);
    TEST_ASSERT_EQUAL_FLOAT(612.0f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(650.0f, out[1]);
}

static void test_extract_ha_history_values_skips_unavailable() {
    const char* body =
        "[[{\"state\":\"unavailable\"},{\"state\":\"700\"}]]";
    float out[8];
    uint16_t n = connectors::extract_ha_history_values(String(body), out, 8);
    TEST_ASSERT_EQUAL_UINT16(1, n);
    TEST_ASSERT_EQUAL_FLOAT(700.0f, out[0]);
}

static void test_extract_ha_history_values_empty_outer_array() {
    float out[8];
    uint16_t n = connectors::extract_ha_history_values(String("[]"), out, 8);
    TEST_ASSERT_EQUAL_UINT16(0, n);
}

// Регрессия: частый датчик Home Assistant за 6 часов легко даёт больше 255
// записей — max_len был uint8_t и молча резал вход до первых 255 (по факту
// раньше — до 128, лимита вызывающего кода), из-за чего спарклайн строился
// по самому НАЧАЛУ окна, а не по всему периоду (поймано на живом устройстве).
static void test_extract_ha_history_values_supports_more_than_255_points() {
    std::string body = "[[";
    constexpr int kCount = 300;
    for (int i = 0; i < kCount; ++i) {
        if (i > 0) body += ",";
        body += "{\"state\":\"" + std::to_string(i) + "\"}";
    }
    body += "]]";

    std::vector<float> out(kCount);
    uint16_t n = connectors::extract_ha_history_values(String(body.c_str()), out.data(), kCount);
    TEST_ASSERT_EQUAL_UINT16(kCount, n);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(299.0f, out[kCount - 1]);
}

// ── connectors::parse_http_response — дельта (задача про дельты курсов) ──

static void test_parse_http_response_delta_source_direct_percent() {
    std::vector<config::SlotMapping> map = {{"btc", "lastPrice", 900, "priceChangePercent"}};
    auto result = connectors::parse_http_response(
        "{\"lastPrice\":\"45000\",\"priceChangePercent\":\"-1.25\"}", map);
    TEST_ASSERT_EQUAL(1, result.size());
    TEST_ASSERT_EQUAL_FLOAT(-1.25f, result[0].value.delta);
}

static void test_parse_http_response_delta_is_previous_computes_percent_change() {
    std::vector<config::SlotMapping> map = {
        {"usd_rub", "Valute.USD.Value", 86400, "Valute.USD.Previous", true}};
    auto result = connectors::parse_http_response(
        "{\"Valute\":{\"USD\":{\"Value\":95.5,\"Previous\":94.0}}}", map);
    TEST_ASSERT_EQUAL(1, result.size());
    // (95.5-94.0)/94.0*100 ≈ 1.5957f
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5957f, result[0].value.delta);
}

static void test_parse_http_response_no_delta_source_leaves_zero() {
    std::vector<config::SlotMapping> map = {{"btc", "lastPrice", 900}};
    auto result = connectors::parse_http_response("{\"lastPrice\":\"45000\"}", map);
    TEST_ASSERT_EQUAL(1, result.size());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, result[0].value.delta);
}

static void test_parse_http_response_has_history_mapping_is_skipped() {
    // source нарочно указывает на РЕАЛЬНО существующий скаляр в теле — без
    // явной проверки has_history мэппинг ошибочно попал бы в результат как
    // обычное значение (слабое место более раннего варианта теста: пустой
    // source и так не находился бы walk_path, совпадение с "0 результатов"
    // ничего не проверяло — поймано мутацией при прогоне).
    config::SlotMapping m;
    m.slot = "btc";
    m.source = "lastPrice";
    m.has_history = true;
    m.history_item = "4";
    std::vector<config::SlotMapping> map = {m};

    auto result = connectors::parse_http_response("{\"lastPrice\":\"45000\"}", map);
    TEST_ASSERT_EQUAL(0, result.size());
}

// ── decimate: края ─────────────────────────────────────────────────────

static void test_decimate_keeps_last_input_point(void) {
    // Прореживание 1245 → 24: раньше последний выход брал 1193-ю запись,
    // свежие ~15 минут истории отрезались. Первая и последняя точки входа
    // обязаны стать первой и последней точками выхода.
    float in[100];
    for (int i = 0; i < 100; ++i) in[i] = static_cast<float>(i);
    float out[24];
    uint8_t n = connectors::decimate(in, 100, out, 24);
    TEST_ASSERT_EQUAL_UINT8(24, n);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(99.0f, out[23]);
}

// ── mark_failed: заглушка причины для коннектора без слотов ────────────

static void test_mark_failed_without_slots_creates_status_placeholder(void) {
    // Холодный старт с 401/403: коннектор ничего не положил, но причина отказа
    // должна быть видна в /api/status — иначе «нужен VPN» никто не увидит.
    slots::Store store;
    store.mark_failed(String("claude"), String("сервер просит подождать (429)"));
    const slots::Slot* st = store.find(String("claude.status"));
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_FALSE(st->ok);
    TEST_ASSERT_EQUAL_STRING("сервер просит подождать (429)", st->error.c_str());
}

static void test_mark_failed_without_reason_creates_nothing(void) {
    slots::Store store;
    store.mark_failed(String("claude"), String(""));
    TEST_ASSERT_NULL(store.find(String("claude.status")));
    TEST_ASSERT_EQUAL_UINT32(0, store.size());
}

static void test_status_placeholder_removed_when_connector_recovers(void) {
    // Данные пришли — устаревшая причина не должна стоять рядом с живым слотом.
    slots::Store store;
    store.mark_failed(String("claude"), String("429"));
    slots::Slot v; v.text = String("57"); v.ok = true; v.at = 100; v.ttl = 300;
    store.put(String("limit.claude.5h"), v, String("claude"));
    TEST_ASSERT_NULL(store.find(String("claude.status")));
    TEST_ASSERT_NOT_NULL(store.find(String("limit.claude.5h")));
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

    RUN_TEST(test_history_empty_on_first_put);
    RUN_TEST(test_put_does_not_accumulate_history_across_calls);
    RUN_TEST(test_put_history_sets_history_and_creates_slot);
    RUN_TEST(test_put_history_truncates_to_capacity);
    RUN_TEST(test_put_after_put_history_keeps_history);
    RUN_TEST(test_mark_failed_sets_error_reason);

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

    RUN_TEST(test_empty_map_reads_slot_dictionary);
    RUN_TEST(test_empty_map_skips_entries_without_text);

    RUN_TEST(test_parse_claude_usage_both_windows);
    RUN_TEST(test_parse_claude_usage_missing_week_keeps_five_hour);
    RUN_TEST(test_parse_claude_usage_missing_five_hour_fails);
    RUN_TEST(test_parse_claude_usage_broken_json_fails);

    RUN_TEST(test_parse_codex_usage_reads_used_percent);
    RUN_TEST(test_parse_codex_usage_missing_window_fails);

    RUN_TEST(test_parse_oauth_refresh_rotates_refresh_token);
    RUN_TEST(test_parse_oauth_refresh_keeps_previous_when_absent);
    RUN_TEST(test_parse_oauth_refresh_missing_access_token_fails);

    RUN_TEST(test_count_imap_unseen_multiple_ids);
    RUN_TEST(test_count_imap_unseen_empty_mailbox);
    RUN_TEST(test_count_imap_unseen_not_a_search_line);

    RUN_TEST(test_parse_imap_search_ids_order_preserved);
    RUN_TEST(test_parse_imap_search_ids_not_a_search_line_is_empty);

    RUN_TEST(test_decode_mime_header_base64_utf8);
    RUN_TEST(test_decode_mime_header_quoted_printable_underscore_is_space);
    RUN_TEST(test_decode_mime_header_plain_text_unchanged);
    RUN_TEST(test_decode_mime_header_two_words_no_space_between);

    RUN_TEST(test_extract_sender_name_with_display_name);
    RUN_TEST(test_extract_sender_name_mime_encoded_display_name);
    RUN_TEST(test_extract_sender_name_address_only_uses_local_part);

    RUN_TEST(test_extract_mail_headers_all_three_fields);
    RUN_TEST(test_extract_mail_headers_case_insensitive_names);
    RUN_TEST(test_extract_mail_headers_folded_continuation_appends);
    RUN_TEST(test_extract_mail_headers_empty_block_fails);

    RUN_TEST(test_parse_rfc822_date_with_weekday_and_offset);
    RUN_TEST(test_parse_rfc822_date_without_weekday);
    RUN_TEST(test_parse_rfc822_date_garbage_fails);

    RUN_TEST(test_format_mail_time_today_is_hh_mm);
    RUN_TEST(test_format_mail_time_yesterday);
    RUN_TEST(test_format_mail_time_older_is_day_dot_month);

    RUN_TEST(test_remaining_percent_typical);
    RUN_TEST(test_remaining_percent_clamps_below_zero_input);
    RUN_TEST(test_remaining_percent_clamps_above_hundred_input);

    RUN_TEST(test_parse_iso8601_utc_with_fraction_and_zero_offset);
    RUN_TEST(test_parse_iso8601_utc_negative_offset);
    RUN_TEST(test_parse_iso8601_utc_garbage_fails);

    RUN_TEST(test_parse_claude_reset_times_both_windows);
    RUN_TEST(test_parse_claude_reset_times_missing_five_hour_fails);
    RUN_TEST(test_format_claude_reset_both_windows);
    RUN_TEST(test_format_claude_reset_only_five_hour);
    RUN_TEST(test_format_claude_reset_neither_window_is_empty);

    RUN_TEST(test_parse_codex_reset_uses_reset_at);
    RUN_TEST(test_parse_codex_reset_falls_back_to_reset_after_seconds);
    RUN_TEST(test_parse_codex_reset_missing_window_fails);
    RUN_TEST(test_format_codex_reset_matches_reference_shape);

    RUN_TEST(test_wmo_to_text_clear);
    RUN_TEST(test_wmo_to_text_rain);
    RUN_TEST(test_wmo_to_text_thunderstorm);
    RUN_TEST(test_wmo_to_text_unknown_code);

    RUN_TEST(test_parse_geocode_response_reads_first_result);
    RUN_TEST(test_parse_geocode_response_empty_results_fails);
    RUN_TEST(test_parse_geocode_response_missing_results_fails);

    RUN_TEST(test_extract_history_array_klines_close_price);
    RUN_TEST(test_extract_history_array_root_of_scalars);
    RUN_TEST(test_extract_history_array_nested_under_source_path);
    RUN_TEST(test_extract_history_array_truncates_to_max_len);
    RUN_TEST(test_extract_history_array_not_an_array_returns_zero);
    RUN_TEST(test_parse_http_history_skips_non_history_mappings);
    RUN_TEST(test_parse_http_history_reads_klines_into_btc_slot);

    RUN_TEST(test_decimate_fewer_points_than_target_copies_all);
    RUN_TEST(test_decimate_more_points_reduces_evenly);

    RUN_TEST(test_extract_ha_history_values_reads_state_field);
    RUN_TEST(test_extract_ha_history_values_skips_unavailable);
    RUN_TEST(test_extract_ha_history_values_empty_outer_array);
    RUN_TEST(test_extract_ha_history_values_supports_more_than_255_points);

    RUN_TEST(test_parse_http_response_delta_source_direct_percent);
    RUN_TEST(test_parse_http_response_delta_is_previous_computes_percent_change);
    RUN_TEST(test_parse_http_response_no_delta_source_leaves_zero);
    RUN_TEST(test_parse_http_response_has_history_mapping_is_skipped);

    RUN_TEST(test_decimate_keeps_last_input_point);

    RUN_TEST(test_mark_failed_without_slots_creates_status_placeholder);
    RUN_TEST(test_mark_failed_without_reason_creates_nothing);
    RUN_TEST(test_status_placeholder_removed_when_connector_recovers);

    return UNITY_END();
}
