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

// ── ap_password_from_bytes: пароль точки доступа собирается из потока
// случайных байт чистой функцией (docs/decisions.md, п.8) — esp_random()
// недоступен на хосте, поэтому источник случайности инъецируется байтами
// прямо в тесте.

static void test_ap_password_from_bytes_maps_through_alphabet() {
    uint8_t bytes[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    String password = config::ap_password_from_bytes(bytes, 10);

    TEST_ASSERT_EQUAL_STRING("23456789AB", password.c_str());
}

static void test_ap_password_from_bytes_wraps_modulo_alphabet_length() {
    // 32 и 255 — соответственно первый индекс после полного круга по
    // 32-символьному алфавиту и последний валидный байт: оба должны попасть
    // в алфавит через `% kApPasswordAlphabetLen`, а не выйти за его границы.
    uint8_t bytes[2] = {32, 255};
    String password = config::ap_password_from_bytes(bytes, 2);

    TEST_ASSERT_EQUAL_STRING("2Z", password.c_str());
}

// ── пароль точки доступа — секрет наравне с токеном коннектора: не должен
// уходить наружу через to_json(..., false)/GET /api/config. Ревью уже нашло
// один такой пропуск для токенов (см. тесты выше) — тот же класс ошибки
// возможен и здесь, если про новое поле забыть при следующей правке.

static void test_to_json_without_secrets_hides_ap_password() {
    config::Settings settings;
    settings.ap_password = "SECRETPASS";

    String json = config::to_json(settings, /*include_secrets=*/false);

    TEST_ASSERT_NULL(strstr(json.c_str(), "SECRETPASS"));
}

static void test_to_json_with_secrets_includes_ap_password() {
    config::Settings settings;
    settings.ap_password = "SECRETPASS";

    // Внутреннее хранилище (config::save) обязано сохранять пароль — без
    // этого пути он не пережил бы перезагрузку.
    String json = config::to_json(settings, /*include_secrets=*/true);

    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "SECRETPASS"));
}

// ── refresh_token/username (IMAP-логин, OAuth Claude) — секреты того же
// класса, что token: сокрыты в to_json(..., false), переживают сохранение
// формы без явного значения, как и token выше.

static void test_to_json_without_secrets_hides_refresh_token_and_username() {
    config::Settings settings;
    config::Connector conn;
    conn.id = "claude";
    conn.refresh_token = "refresh-secret";
    conn.username = "user@example.com";
    settings.connectors.push_back(conn);

    String json = config::to_json(settings, /*include_secrets=*/false);

    TEST_ASSERT_NULL(strstr(json.c_str(), "refresh-secret"));
    TEST_ASSERT_NULL(strstr(json.c_str(), "user@example.com"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "refresh_token_set"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "username_set"));
}

static void test_to_json_with_secrets_includes_refresh_token_and_username() {
    config::Settings settings;
    config::Connector conn;
    conn.id = "claude";
    conn.refresh_token = "refresh-secret";
    conn.username = "user@example.com";
    settings.connectors.push_back(conn);

    String json = config::to_json(settings, /*include_secrets=*/true);

    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "refresh-secret"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "user@example.com"));
}

static void test_from_json_missing_refresh_token_keeps_previous() {
    config::Settings settings;
    config::Connector conn;
    conn.id = "claude";
    conn.refresh_token = "refresh-secret";
    conn.username = "user@example.com";
    settings.connectors.push_back(conn);

    String payload = "{\"connectors\":[{\"id\":\"claude\",\"refresh_token\":\"\",\"username\":\"\"}]}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL_STRING("refresh-secret", settings.connectors[0].refresh_token.c_str());
    TEST_ASSERT_EQUAL_STRING("user@example.com", settings.connectors[0].username.c_str());
}

static void test_round_trip_keeps_refresh_token_and_username() {
    config::Settings settings;
    config::Connector conn;
    conn.id = "claude";
    conn.refresh_token = "refresh-secret";
    conn.username = "user@example.com";
    settings.connectors.push_back(conn);

    String raw = config::to_json(settings, /*include_secrets=*/true);

    config::Settings loaded;
    config::from_json(raw, loaded);

    TEST_ASSERT_EQUAL_STRING("refresh-secret", loaded.connectors[0].refresh_token.c_str());
    TEST_ASSERT_EQUAL_STRING("user@example.com", loaded.connectors[0].username.c_str());
}

static void test_from_json_missing_ap_password_keeps_previous() {
    config::Settings settings;
    settings.ap_password = "SECRETPASS";

    // Страница настройки это поле никогда не присылает (его там просто нет
    // в форме) — отсутствие в присланном JSON обязано означать «не трогали»,
    // а не стирать единственный постоянный пароль точки доступа.
    String payload = "{}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL_STRING("SECRETPASS", settings.ap_password.c_str());
}

// ── SlotMapping: новые поля (дельта, история) переживают сохранение ──

static void test_round_trip_keeps_slot_mapping_extras() {
    config::Settings settings;
    config::Connector conn;
    conn.id = "btc_history";
    conn.kind = "http";
    config::SlotMapping m;
    m.slot = "btc";
    m.source = "lastPrice";
    m.ttl = 900;
    m.delta_source = "priceChangePercent";
    m.delta_is_previous = true;
    m.has_history = true;
    m.history_source = "prices";
    m.history_item = "4";
    conn.map.push_back(m);
    settings.connectors.push_back(conn);

    String raw = config::to_json(settings, /*include_secrets=*/true);
    config::Settings loaded;
    config::from_json(raw, loaded);

    TEST_ASSERT_EQUAL(1, loaded.connectors[0].map.size());
    const config::SlotMapping& lm = loaded.connectors[0].map[0];
    TEST_ASSERT_EQUAL_STRING("priceChangePercent", lm.delta_source.c_str());
    TEST_ASSERT_TRUE(lm.delta_is_previous);
    TEST_ASSERT_TRUE(lm.has_history);
    TEST_ASSERT_EQUAL_STRING("prices", lm.history_source.c_str());
    TEST_ASSERT_EQUAL_STRING("4", lm.history_item.c_str());
}

// ── Settings.city / city_resolved / city_lat / city_lon ──

static void test_from_json_missing_city_keeps_previous() {
    config::Settings settings;
    settings.city = "Лиссабон";
    settings.city_resolved = "Лиссабон";
    settings.city_lat = 38.7f;
    settings.city_lon = -9.1f;

    // Форма не редактирует city_resolved/city_lat/city_lon вовсе (см.
    // config.h) — их отсутствие в присланном JSON означает «не трогали».
    String payload = "{}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL_STRING("Лиссабон", settings.city.c_str());
    TEST_ASSERT_EQUAL_STRING("Лиссабон", settings.city_resolved.c_str());
    TEST_ASSERT_EQUAL_FLOAT(38.7f, settings.city_lat);
    TEST_ASSERT_EQUAL_FLOAT(-9.1f, settings.city_lon);
}

static void test_round_trip_keeps_city_fields() {
    config::Settings settings;
    settings.city = "Екатеринбург";
    settings.city_resolved = "Екатеринбург";
    settings.city_lat = 56.8389f;
    settings.city_lon = 60.6057f;

    String raw = config::to_json(settings, /*include_secrets=*/true);
    config::Settings loaded;
    config::from_json(raw, loaded);

    TEST_ASSERT_EQUAL_STRING("Екатеринбург", loaded.city.c_str());
    TEST_ASSERT_EQUAL_STRING("Екатеринбург", loaded.city_resolved.c_str());
    TEST_ASSERT_EQUAL_FLOAT(56.8389f, loaded.city_lat);
    TEST_ASSERT_EQUAL_FLOAT(60.6057f, loaded.city_lon);
}

// ── merge_missing_factory_connectors: миграция уже настроенных устройств ──

static void test_merge_missing_factory_connectors_adds_new_by_id() {
    config::Settings settings;
    config::Connector existing;
    existing.id = "home";
    existing.kind = "homeassistant";
    existing.token = "my-token";
    settings.connectors.push_back(existing);

    config::merge_missing_factory_connectors(settings);

    bool has_geocode = false, has_btc_history = false;
    for (const auto& c : settings.connectors) {
        if (c.id == "geocode") has_geocode = true;
        if (c.id == "btc_history") has_btc_history = true;
    }
    TEST_ASSERT_TRUE(has_geocode);
    TEST_ASSERT_TRUE(has_btc_history);
    // Существующий коннектор не тронут — токен на месте.
    for (const auto& c : settings.connectors) {
        if (c.id == "home") TEST_ASSERT_EQUAL_STRING("my-token", c.token.c_str());
    }
}

static void test_merge_missing_factory_connectors_replaces_changed_kind() {
    config::Settings settings;
    config::Connector old_weather;
    old_weather.id = "weather";
    old_weather.kind = "http";  // старая заводская схема, до geocode/city
    old_weather.url = "https://api.open-meteo.com/v1/forecast?latitude=56.84&longitude=60.65";
    settings.connectors.push_back(old_weather);

    config::merge_missing_factory_connectors(settings);

    for (const auto& c : settings.connectors) {
        if (c.id == "weather") TEST_ASSERT_EQUAL_STRING("weather", c.kind.c_str());
    }
}

static void test_merge_missing_factory_connectors_keeps_user_edited_connector() {
    config::Settings settings;
    config::Connector btc;
    btc.id = "btc";
    btc.kind = "http";
    btc.url = "https://example.com/custom-btc";  // владелец подменил адрес сам
    settings.connectors.push_back(btc);

    config::merge_missing_factory_connectors(settings);

    for (const auto& c : settings.connectors) {
        if (c.id == "btc") TEST_ASSERT_EQUAL_STRING("https://example.com/custom-btc", c.url.c_str());
    }
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

    RUN_TEST(test_to_json_without_secrets_hides_refresh_token_and_username);
    RUN_TEST(test_to_json_with_secrets_includes_refresh_token_and_username);
    RUN_TEST(test_from_json_missing_refresh_token_keeps_previous);
    RUN_TEST(test_round_trip_keeps_refresh_token_and_username);

    RUN_TEST(test_ap_password_from_bytes_maps_through_alphabet);
    RUN_TEST(test_ap_password_from_bytes_wraps_modulo_alphabet_length);
    RUN_TEST(test_to_json_without_secrets_hides_ap_password);
    RUN_TEST(test_to_json_with_secrets_includes_ap_password);
    RUN_TEST(test_from_json_missing_ap_password_keeps_previous);

    RUN_TEST(test_round_trip_keeps_slot_mapping_extras);
    RUN_TEST(test_from_json_missing_city_keeps_previous);
    RUN_TEST(test_round_trip_keeps_city_fields);
    RUN_TEST(test_merge_missing_factory_connectors_adds_new_by_id);
    RUN_TEST(test_merge_missing_factory_connectors_replaces_changed_kind);
    RUN_TEST(test_merge_missing_factory_connectors_keeps_user_edited_connector);

    return UNITY_END();
}
