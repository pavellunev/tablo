// Тесты config::from_json/to_json — на хосте, без NVS (load/save/reset не
// компилируются под NATIVE_BUILD, см. #ifndef в config.cpp).
//
// Стык, который эти тесты закрывают: сохранение формы настройки не должно
// молча стирать то, что форма не прислала (map, а до этой правки — и вовсе
// весь коннектор при пустом поле). Обе поломки ловились бы тестом в десять
// строк — теперь он есть.
#include <unity.h>

#include <cstring>

#include "../../src/config.cpp"

void setUp() {}
void tearDown() {}

// ── from_json: map коннектора переживает сохранение формы без этого поля ──

static void test_from_json_missing_map_keeps_previous() {
    config::Settings settings;
    config::SlotMapping m;
    m.slot = "btc";
    m.source = "bitcoin.usd";
    m.ttl = 300;
    config::Connector conn;
    conn.id = "rates";
    conn.kind = "http";
    conn.url = "https://example.com";
    conn.map.push_back(m);
    settings.connectors.push_back(conn);

    // index.html пока не редактирует map и не присылает это поле вовсе —
    // ровно то, что уронило все привязки слотов до фикса.
    String payload =
        "{\"connectors\":[{\"id\":\"rates\",\"kind\":\"http\",\"url\":\"https://example.com\"}]}";
    bool ok = config::from_json(payload, settings);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, settings.connectors.size());
    TEST_ASSERT_EQUAL(1, settings.connectors[0].map.size());
    TEST_ASSERT_EQUAL_STRING("btc", settings.connectors[0].map[0].slot.c_str());
}

static void test_from_json_explicit_empty_map_clears_it() {
    config::Settings settings;
    config::SlotMapping m;
    m.slot = "btc";
    config::Connector conn;
    conn.id = "rates";
    conn.map.push_back(m);
    settings.connectors.push_back(conn);

    // А вот явно присланный пустой массив — осознанная очистка, а не «забыли
    // прислать»: это отличается от отсутствия поля.
    String payload = "{\"connectors\":[{\"id\":\"rates\",\"map\":[]}]}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL(0, settings.connectors[0].map.size());
}

// ── from_json: HTTP-токен переживает сохранение формы с пустым полем ──

static void test_from_json_missing_token_keeps_previous() {
    config::Settings settings;
    config::Connector conn;
    conn.id = "rates";
    conn.token = "secret-token";
    settings.connectors.push_back(conn);

    String payload = "{\"connectors\":[{\"id\":\"rates\",\"token\":\"\"}]}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL_STRING("secret-token", settings.connectors[0].token.c_str());
}

// ── from_json: дубликат id коннектора не создаёт вторую запись ──

static void test_from_json_duplicate_connector_id_is_dropped() {
    config::Settings settings;
    String payload =
        "{\"connectors\":["
        "{\"id\":\"rates\",\"url\":\"https://a.example\"},"
        "{\"id\":\"rates\",\"url\":\"https://b.example\"}"
        "]}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL(1, settings.connectors.size());
    TEST_ASSERT_EQUAL_STRING("https://a.example", settings.connectors[0].url.c_str());
}

// ── from_json: interval не даёт устроить опрос каждые несколько мс ──

static void test_from_json_zero_interval_is_clamped() {
    config::Settings settings;
    String payload = "{\"connectors\":[{\"id\":\"rates\",\"interval\":0}]}";
    config::from_json(payload, settings);

    TEST_ASSERT_TRUE(settings.connectors[0].interval >= 5);
}

// ── from_json: device_name отсутствует в присланном JSON — не стирается ──

static void test_from_json_missing_device_name_keeps_previous() {
    config::Settings settings;
    settings.device_name = "my-inkroam";

    String payload = "{}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL_STRING("my-inkroam", settings.device_name.c_str());
}

// ── to_json/from_json: секреты и map переживают полный цикл ──

static void test_round_trip_keeps_map_and_secrets() {
    config::Settings settings;
    config::SlotMapping m;
    m.slot = "btc";
    m.source = "bitcoin.usd";
    m.ttl = 300;
    config::Connector conn;
    conn.id = "rates";
    conn.kind = "http";
    conn.url = "https://example.com";
    conn.token = "secret-token";
    conn.interval = 60;
    conn.map.push_back(m);
    settings.connectors.push_back(conn);

    String raw = config::to_json(settings, /*include_secrets=*/true);

    config::Settings loaded;
    bool ok = config::from_json(raw, loaded);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, loaded.connectors.size());
    TEST_ASSERT_EQUAL_STRING("secret-token", loaded.connectors[0].token.c_str());
    TEST_ASSERT_EQUAL(1, loaded.connectors[0].map.size());
    TEST_ASSERT_EQUAL_STRING("bitcoin.usd", loaded.connectors[0].map[0].source.c_str());
}

// ── to_json: секреты отдаются наружу только явным include_secrets=true ──
//
// Ревью нашло, что этой проверки не было вовсе: мутация `if (include_secrets)`
// → `if (true)` в to_json проходила все тесты зелёными, то есть ничего не
// гарантировало, что /api/config (который всегда зовёт to_json(..., false))
// не отдаёт пароль сети и токен коннектора первому встречному в этой сети.

static void test_to_json_without_secrets_hides_password_and_token() {
    config::Settings settings;
    config::Network net;
    net.ssid = "home";
    net.password = "wifi-secret";
    settings.networks.push_back(net);

    config::Connector conn;
    conn.id = "rates";
    conn.token = "bearer-secret";
    settings.connectors.push_back(conn);

    String json = config::to_json(settings, /*include_secrets=*/false);

    TEST_ASSERT_NULL(strstr(json.c_str(), "wifi-secret"));
    TEST_ASSERT_NULL(strstr(json.c_str(), "bearer-secret"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "password_set"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "token_set"));
}

static void test_to_json_with_secrets_includes_password_and_token() {
    config::Settings settings;
    config::Network net;
    net.ssid = "home";
    net.password = "wifi-secret";
    settings.networks.push_back(net);

    config::Connector conn;
    conn.id = "rates";
    conn.token = "bearer-secret";
    settings.connectors.push_back(conn);

    // Внутреннее хранилище (config::save) зовёт to_json(..., true) — этот
    // путь обязан сохранять секреты, иначе они терялись бы при каждой
    // перезагрузке. Тест на противоположную сторону условия: без него
    // мутация "всегда прятать" тоже прошла бы незамеченной.
    String json = config::to_json(settings, /*include_secrets=*/true);

    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "wifi-secret"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "bearer-secret"));
}

// ── from_json: отсутствие поля insecure — это false, а не «оставить прежнее» ──
//
// Мутация `c["insecure"] | false` → `c["insecure"] | true` тоже проходила
// незамеченной: коннектор без этого поля в присланном JSON стал бы insecure
// по умолчанию, то есть отключал бы проверку сертификата без явного согласия.

static void test_from_json_missing_insecure_defaults_to_false() {
    config::Settings settings;
    String payload = "{\"connectors\":[{\"id\":\"rates\"}]}";
    config::from_json(payload, settings);

    TEST_ASSERT_FALSE(settings.connectors[0].insecure);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_from_json_missing_map_keeps_previous);
    RUN_TEST(test_from_json_explicit_empty_map_clears_it);
    RUN_TEST(test_from_json_missing_token_keeps_previous);
    RUN_TEST(test_from_json_duplicate_connector_id_is_dropped);
    RUN_TEST(test_from_json_zero_interval_is_clamped);
    RUN_TEST(test_from_json_missing_device_name_keeps_previous);
    RUN_TEST(test_round_trip_keeps_map_and_secrets);

    RUN_TEST(test_to_json_without_secrets_hides_password_and_token);
    RUN_TEST(test_to_json_with_secrets_includes_password_and_token);
    RUN_TEST(test_from_json_missing_insecure_defaults_to_false);

    return UNITY_END();
}
