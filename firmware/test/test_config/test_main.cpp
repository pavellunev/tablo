// Тесты config::from_json/to_json — на хосте, без NVS (load/save/reset не
// компилируются под NATIVE_BUILD, см. #ifndef в config.cpp).
//
// Стык, который эти тесты закрывают: сохранение формы настройки не должно
// молча стирать то, что форма не прислала (map, а до этой правки — и вовсе
// весь коннектор при пустом поле). Обе поломки ловились бы тестом в десять
// строк — теперь он есть.
#include <unity.h>

#include <cstring>
#include <string>

#include "../../src/widgets/types.cpp"  // config.cpp зовёт widgets::find_type/size_*
#include "../../src/i18n.cpp"
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
    settings.device_name = "my-tablo";

    String payload = "{}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL_STRING("my-tablo", settings.device_name.c_str());
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

static void test_defaults_are_neutral() {
    config::Settings s = config::defaults();
    TEST_ASSERT_EQUAL_STRING("", s.city.c_str());
    TEST_ASSERT_EQUAL_STRING("", s.city_resolved.c_str());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.city_lat);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.city_lon);
    TEST_ASSERT_EQUAL_INT(0, s.timezone_minutes);
    TEST_ASSERT_EQUAL_STRING("en", s.lang.c_str());
    TEST_ASSERT_EQUAL_STRING("Desk", s.dashboards[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Road", s.dashboards[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Custom", s.dashboards[2].name.c_str());
}

// ── Settings.lang ──

static void test_lang_round_trip() {
    config::Settings settings;
    TEST_ASSERT_EQUAL_STRING("en", settings.lang.c_str());
    settings.lang = "ru";
    String raw = config::to_json(settings, /*include_secrets=*/true);
    config::Settings loaded;
    config::from_json(raw, loaded);
    TEST_ASSERT_EQUAL_STRING("ru", loaded.lang.c_str());
}

static void test_from_json_garbage_lang_keeps_previous() {
    config::Settings settings;
    settings.lang = "ru";
    String payload = "{\"lang\":\"de\"}";
    config::from_json(payload, settings);
    TEST_ASSERT_EQUAL_STRING("ru", settings.lang.c_str());

    payload = "{}";
    config::from_json(payload, settings);
    TEST_ASSERT_EQUAL_STRING("ru", settings.lang.c_str());
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

// ── секрет не переезжает на новый адрес (docs/decisions.md, п.8) ──
//
// Запись настроек разрешена из любой сети, где устройство оказалось. Пустое
// поле токена по-прежнему значит «не трогали» — но только пока адрес тот же:
// иначе чужой в гостиничной сети присылал бы коннектор со своим адресом и
// получал бы сохранённый токен от устройства собственноручно.

static config::Settings settings_with_home(const char* url, bool insecure) {
    config::Settings settings;
    config::Connector conn;
    conn.id = "home";
    conn.kind = "homeassistant";
    conn.url = url;
    conn.token = "ha-secret";
    conn.refresh_token = "refresh-secret";
    conn.username = "user@example.com";
    conn.insecure = insecure;
    settings.connectors.push_back(conn);
    return settings;
}

static void test_from_json_same_target_without_token_keeps_all_secrets() {
    config::Settings settings = settings_with_home("http://ha.local:8123", false);
    String payload =
        "{\"connectors\":[{\"id\":\"home\",\"kind\":\"homeassistant\","
        "\"url\":\"http://ha.local:8123\",\"insecure\":false}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));
    TEST_ASSERT_EQUAL_STRING("ha-secret", settings.connectors[0].token.c_str());
    TEST_ASSERT_EQUAL_STRING("refresh-secret", settings.connectors[0].refresh_token.c_str());
    TEST_ASSERT_EQUAL_STRING("user@example.com", settings.connectors[0].username.c_str());
}

static void test_from_json_url_change_without_token_drops_secrets() {
    config::Settings settings = settings_with_home("http://ha.local:8123", false);
    // Тот же id, чужой адрес, пустые секреты — ровно запрос атакующего.
    String payload =
        "{\"connectors\":[{\"id\":\"home\",\"kind\":\"homeassistant\","
        "\"url\":\"http://10.0.0.13:8123\"}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.13:8123", settings.connectors[0].url.c_str());
    TEST_ASSERT_EQUAL_STRING("", settings.connectors[0].token.c_str());
    TEST_ASSERT_EQUAL_STRING("", settings.connectors[0].refresh_token.c_str());
    TEST_ASSERT_EQUAL_STRING("", settings.connectors[0].username.c_str());
}

static void test_from_json_url_change_with_token_takes_new_token() {
    config::Settings settings = settings_with_home("http://ha.local:8123", false);
    String payload =
        "{\"connectors\":[{\"id\":\"home\",\"kind\":\"homeassistant\","
        "\"url\":\"http://10.0.0.13:8123\",\"token\":\"new-secret\"}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));
    TEST_ASSERT_EQUAL_STRING("new-secret", settings.connectors[0].token.c_str());
}

static void test_from_json_enabling_insecure_without_token_drops_secret() {
    config::Settings settings = settings_with_home("https://ha.local:8123", false);
    // Адрес тот же, но проверку сертификата выключили: в чужой сети это
    // адрес подменяется DNS-ом — токен на такую цель не наследуется.
    String payload =
        "{\"connectors\":[{\"id\":\"home\",\"kind\":\"homeassistant\","
        "\"url\":\"https://ha.local:8123\",\"insecure\":true}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));
    TEST_ASSERT_TRUE(settings.connectors[0].insecure);
    TEST_ASSERT_EQUAL_STRING("", settings.connectors[0].token.c_str());
}

static void test_from_json_disabling_insecure_without_token_keeps_secret() {
    config::Settings settings = settings_with_home("https://ha.local:8123", true);
    // Обратное направление безопаснее прежнего — секрет остаётся.
    String payload =
        "{\"connectors\":[{\"id\":\"home\",\"kind\":\"homeassistant\","
        "\"url\":\"https://ha.local:8123\",\"insecure\":false}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));
    TEST_ASSERT_EQUAL_STRING("ha-secret", settings.connectors[0].token.c_str());
}

// ── дашборды (docs/widgets.md): round-trip, валидация при разборе формы ──

static config::Dashboard dashboard_with_one_widget() {
    config::Dashboard db;
    db.name = "Мой";
    widgets::Instance markets;
    markets.type = "markets";
    markets.size = widgets::Size::kM;
    db.rows[0].push_back(markets);
    widgets::Instance today;
    today.type = "today";
    today.size = widgets::Size::kS;
    today.divider = true;
    today.label = "Погода";
    db.rows[1].push_back(today);
    return db;
}

static void test_dashboard_round_trip_keeps_widgets() {
    config::Settings settings;
    settings.dashboards.push_back(dashboard_with_one_widget());
    settings.dashboards.push_back(config::Dashboard{});
    settings.dashboards.push_back(config::Dashboard{});
    settings.active_dashboard = 0;

    String raw = config::to_json(settings, /*include_secrets=*/true);
    config::Settings loaded;
    TEST_ASSERT_TRUE(config::from_json(raw, loaded));

    TEST_ASSERT_EQUAL(3, loaded.dashboards.size());
    TEST_ASSERT_EQUAL_STRING("Мой", loaded.dashboards[0].name.c_str());
    TEST_ASSERT_EQUAL(1, loaded.dashboards[0].rows[0].size());
    TEST_ASSERT_EQUAL_STRING("markets", loaded.dashboards[0].rows[0][0].type.c_str());
    TEST_ASSERT_TRUE(widgets::Size::kM == loaded.dashboards[0].rows[0][0].size);
    TEST_ASSERT_EQUAL(1, loaded.dashboards[0].rows[1].size());
    TEST_ASSERT_TRUE(loaded.dashboards[0].rows[1][0].divider);
    TEST_ASSERT_EQUAL_STRING("Погода", loaded.dashboards[0].rows[1][0].label.c_str());
}

static void test_from_json_missing_dashboards_keeps_previous() {
    config::Settings settings;
    settings.dashboards.push_back(dashboard_with_one_widget());
    settings.dashboards.push_back(config::Dashboard{});
    settings.dashboards.push_back(config::Dashboard{});

    String payload = "{}";
    config::from_json(payload, settings);

    TEST_ASSERT_EQUAL(3, settings.dashboards.size());
    TEST_ASSERT_EQUAL_STRING("Мой", settings.dashboards[0].name.c_str());
}

static void test_from_json_extra_dashboard_dropped() {
    config::Settings settings;
    String payload =
        "{\"dashboards\":["
        "{\"name\":\"A\",\"rows\":[[],[]]},"
        "{\"name\":\"B\",\"rows\":[[],[]]},"
        "{\"name\":\"C\",\"rows\":[[],[]]},"
        "{\"name\":\"D\",\"rows\":[[],[]]}"
        "]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    TEST_ASSERT_EQUAL(3, settings.dashboards.size());
    TEST_ASSERT_EQUAL_STRING("A", settings.dashboards[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("C", settings.dashboards[2].name.c_str());
}

static void test_from_json_fewer_than_three_dashboards_padded_empty() {
    config::Settings settings;
    String payload = "{\"dashboards\":[{\"name\":\"A\",\"rows\":[[],[]]}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    TEST_ASSERT_EQUAL(3, settings.dashboards.size());
    TEST_ASSERT_EQUAL_STRING("", settings.dashboards[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("", settings.dashboards[2].name.c_str());
}

static void test_from_json_unknown_widget_type_is_dropped() {
    config::Settings settings;
    String payload =
        "{\"dashboards\":[{\"name\":\"A\",\"rows\":[[{\"type\":\"bogus\",\"size\":\"S\"},"
        "{\"type\":\"markets\",\"size\":\"M\"}],[]]}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    TEST_ASSERT_EQUAL(1, settings.dashboards[0].rows[0].size());
    TEST_ASSERT_EQUAL_STRING("markets", settings.dashboards[0].rows[0][0].type.c_str());
}

static void test_from_json_unknown_size_falls_back_to_widget_default() {
    config::Settings settings;
    // mail — default_size flex (widgets/types.cpp); "wat" не входит в S/M/flex.
    String payload =
        "{\"dashboards\":[{\"name\":\"A\",\"rows\":[[{\"type\":\"mail\",\"size\":\"wat\"}],[]]}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    TEST_ASSERT_TRUE(widgets::Size::kFlex == settings.dashboards[0].rows[0][0].size);
}

static void test_from_json_too_many_widgets_in_row_are_dropped() {
    config::Settings settings;
    std::string payload_str = "{\"dashboards\":[{\"name\":\"A\",\"rows\":[[";
    for (int i = 0; i < 8; ++i) {
        if (i) payload_str += ",";
        payload_str += "{\"type\":\"text\",\"label\":\"x\"}";
    }
    payload_str += "],[]]}]}";
    String payload(payload_str.c_str());
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    // Не больше 6 виджетов в ряду — остальные молча отброшены.
    TEST_ASSERT_EQUAL(6, settings.dashboards[0].rows[0].size());
}

static void test_from_json_active_dashboard_out_of_range_resets_to_zero() {
    config::Settings settings;
    // 3 — первое недопустимое значение (валидный диапазон 0..2, дашбордов
    // всегда ровно три): граница, а не запасом, иначе мутация «> 2» -> «> 3»
    // прошла бы тестом с большим числом незамеченной.
    String payload = "{\"active_dashboard\":3}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    TEST_ASSERT_EQUAL(0, settings.active_dashboard);
}

static void test_from_json_active_dashboard_in_range_is_kept() {
    config::Settings settings;
    String payload = "{\"active_dashboard\":2}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));

    TEST_ASSERT_EQUAL(2, settings.active_dashboard);
}

static void test_to_json_omits_default_divider_slot_and_label() {
    config::Settings settings;
    config::Dashboard db;
    widgets::Instance plain;
    plain.type = "markets";
    plain.size = widgets::Size::kM;
    db.rows[0].push_back(plain);
    settings.dashboards.push_back(db);
    settings.dashboards.push_back(config::Dashboard{});
    settings.dashboards.push_back(config::Dashboard{});

    String json = config::to_json(settings, /*include_secrets=*/false);

    // Компактность — потолок NVS ≈ 8 КБ (config.cpp, save()): поля со
    // значением по умолчанию не должны раздувать JSON.
    TEST_ASSERT_NULL(strstr(json.c_str(), "\"divider\""));
    TEST_ASSERT_NULL(strstr(json.c_str(), "\"slot\""));
    TEST_ASSERT_NULL(strstr(json.c_str(), "\"label\""));
}

static void test_to_json_defaults_stays_under_nvs_ceiling() {
    // Потолок NVS ≈ 8 КБ (config.cpp, save()) — три заводских дашборда не
    // должны вытолкнуть JSON настроек за границу вместе с токенами.
    String json = config::to_json(config::defaults(), /*include_secrets=*/true);
    TEST_ASSERT_TRUE(json.length() < 7000);
}

static void test_from_json_kind_change_without_token_drops_secret() {
    config::Settings settings = settings_with_home("http://ha.local:8123", false);
    // Адрес тот же, протокол другой — секрет ушёл бы другим путём.
    String payload =
        "{\"connectors\":[{\"id\":\"home\",\"kind\":\"http\","
        "\"url\":\"http://ha.local:8123\"}]}";
    TEST_ASSERT_TRUE(config::from_json(payload, settings));
    TEST_ASSERT_EQUAL_STRING("", settings.connectors[0].token.c_str());
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
    RUN_TEST(test_defaults_are_neutral);
    RUN_TEST(test_lang_round_trip);
    RUN_TEST(test_from_json_garbage_lang_keeps_previous);
    RUN_TEST(test_merge_missing_factory_connectors_adds_new_by_id);
    RUN_TEST(test_merge_missing_factory_connectors_replaces_changed_kind);
    RUN_TEST(test_merge_missing_factory_connectors_keeps_user_edited_connector);
    RUN_TEST(test_from_json_same_target_without_token_keeps_all_secrets);
    RUN_TEST(test_from_json_url_change_without_token_drops_secrets);
    RUN_TEST(test_from_json_url_change_with_token_takes_new_token);
    RUN_TEST(test_from_json_enabling_insecure_without_token_drops_secret);
    RUN_TEST(test_from_json_disabling_insecure_without_token_keeps_secret);
    RUN_TEST(test_from_json_kind_change_without_token_drops_secret);

    RUN_TEST(test_dashboard_round_trip_keeps_widgets);
    RUN_TEST(test_from_json_missing_dashboards_keeps_previous);
    RUN_TEST(test_from_json_extra_dashboard_dropped);
    RUN_TEST(test_from_json_fewer_than_three_dashboards_padded_empty);
    RUN_TEST(test_from_json_unknown_widget_type_is_dropped);
    RUN_TEST(test_from_json_unknown_size_falls_back_to_widget_default);
    RUN_TEST(test_from_json_too_many_widgets_in_row_are_dropped);
    RUN_TEST(test_from_json_active_dashboard_out_of_range_resets_to_zero);
    RUN_TEST(test_from_json_active_dashboard_in_range_is_kept);
    RUN_TEST(test_to_json_omits_default_divider_slot_and_label);
    RUN_TEST(test_to_json_defaults_stays_under_nvs_ceiling);

    return UNITY_END();
}
