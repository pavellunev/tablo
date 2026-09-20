#include "config.h"

#include <ArduinoJson.h>

#include <vector>

// Preferences (NVS) существует только на устройстве. to_json/from_json —
// чистая логика без сети и без флеша, её и гоняют тесты на хосте (env:native,
// NATIVE_BUILD); load/save/reset этой логике не нужны и на хосте не
// собираются — ровно как poll_due в connectors.cpp.
#ifndef NATIVE_BUILD
#include <Preferences.h>
#endif

namespace config {

namespace {

// Один namespace, один ключ: вся конфигурация — одна JSON-строка. Отдельные
// ключи на каждое поле не дают выигрыша (пишем и читаем всё равно целиком —
// см. «один владелец» в шапке файла), а лишний код по их синхронизации не нужен.
constexpr const char* kNamespace = "inkroam";
constexpr const char* kKey = "settings";

Settings defaults() {
    Settings s;
    s.device_name = "inkroam-setup";
    s.timezone_minutes = 0;
    return s;
}

// Пароль/токен ищутся в прежних настройках по ssid/id, а не по индексу
// массива: пользователь мог поменять порядок сетей в форме, индекс поплывёт.
String find_password(const Settings& prev, const String& ssid) {
    for (const auto& net : prev.networks) {
        if (net.ssid == ssid) return net.password;
    }
    return "";
}

String find_token(const Settings& prev, const String& id) {
    for (const auto& conn : prev.connectors) {
        if (conn.id == id) return conn.token;
    }
    return "";
}

// Привязки слотов ищутся так же, по id коннектора, а не по индексу — по той
// же причине, что пароль и токен выше.
std::vector<SlotMapping> find_map(const Settings& prev, const String& id) {
    for (const auto& conn : prev.connectors) {
        if (conn.id == id) return conn.map;
    }
    return {};
}

// Опрос раз в 50 мс (interval: 0 из битой формы) уложил бы канал и источник
// раньше, чем кто-то заметит проблему в интерфейсе.
constexpr uint32_t kMinPollIntervalSeconds = 5;

}  // namespace

#ifndef NATIVE_BUILD

Settings load() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/true)) return defaults();
    String raw = prefs.getString(kKey, "");
    prefs.end();

    // Пустая строка или битый JSON — не повод падать: возвращаем заводские
    // значения, устройство поднимется в режиме настройки.
    Settings settings = defaults();
    if (raw.isEmpty() || !from_json(raw, settings)) {
        return defaults();
    }
    return settings;
}

bool save(const Settings& settings) {
    // Сохранённый JSON всегда включает секреты — это внутреннее хранилище
    // (NVS), наружу токены и пароли отдаёт только to_json(..., false).
    String raw = to_json(settings, /*include_secrets=*/true);

    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
    size_t written = prefs.putString(kKey, raw);
    prefs.end();
    return written == raw.length();
}

bool reset() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
    bool ok = prefs.clear();
    prefs.end();
    return ok;
}

#endif  // NATIVE_BUILD

String to_json(const Settings& settings, bool include_secrets) {
    JsonDocument doc;

    // Присваиваем через c_str(), а не саму String: у ArduinoJson поддержка
    // Arduino String включается по наличию макроса ARDUINO, а на хосте
    // (тесты, NATIVE_BUILD) его нет и наш шим String под неё не подходит.
    // const char* понимают обе стороны одинаково.
    doc["device_name"] = settings.device_name.c_str();
    doc["timezone_minutes"] = settings.timezone_minutes;

    JsonArray networks = doc["networks"].to<JsonArray>();
    for (const auto& net : settings.networks) {
        JsonObject o = networks.add<JsonObject>();
        o["ssid"] = net.ssid.c_str();
        if (include_secrets) {
            o["password"] = net.password.c_str();
        } else {
            // Страница настройки видит только факт «пароль задан», не сам
            // пароль — иначе он утекал бы каждому, кто откроет /api/config.
            o["password_set"] = !net.password.isEmpty();
        }
    }

    JsonArray connectors = doc["connectors"].to<JsonArray>();
    for (const auto& conn : settings.connectors) {
        JsonObject o = connectors.add<JsonObject>();
        o["id"] = conn.id.c_str();
        o["kind"] = conn.kind.c_str();
        o["url"] = conn.url.c_str();
        o["interval"] = conn.interval;
        if (include_secrets) {
            o["token"] = conn.token.c_str();
        } else {
            o["token_set"] = !conn.token.isEmpty();
        }
        // Не секрет — отдаём всегда: страница настройки должна показывать
        // предупреждение о риске MITM независимо от того, кто читает
        // /api/config (см. docs/decisions.md, п.9).
        o["insecure"] = conn.insecure;

        JsonArray map = o["map"].to<JsonArray>();
        for (const auto& m : conn.map) {
            JsonObject mo = map.add<JsonObject>();
            mo["slot"] = m.slot.c_str();
            mo["source"] = m.source.c_str();
            mo["ttl"] = m.ttl;
        }
    }

    // serializeJson(doc, String&) идёт тем же путём через Arduino Print,
    // которого на хосте нет — сериализуем в буфер и оборачиваем в String
    // через c_str()-конструктор, он есть у обеих реализаций String.
    size_t needed = measureJson(doc) + 1;
    std::vector<char> buf(needed);
    serializeJson(doc, buf.data(), needed);
    return String(buf.data());
}

bool from_json(const String& json, Settings& settings) {
    JsonDocument doc;
    // Тот же довод про c_str(): вход тоже должен идти через const char*,
    // а не полагаться на поддержку Arduino String в ArduinoJson.
    if (deserializeJson(doc, json.c_str()) != DeserializationError::Ok) return false;

    // Прежнее значение — источник «оставить как было» для секретов и для
    // полей, которых в присланном JSON вовсе нет.
    Settings prev = settings;

    // doc["device_name"] | prev.device_name потребовал бы у ArduinoJson
    // конвертер для нашего String — на хосте его нет. Проверяем тип вручную,
    // семантика та же: поле отсутствует или не строка — оставляем прежнее.
    if (doc["device_name"].is<const char*>()) {
        settings.device_name = doc["device_name"].as<const char*>();
    } else {
        settings.device_name = prev.device_name;
    }
    settings.timezone_minutes = doc["timezone_minutes"] | prev.timezone_minutes;

    settings.networks.clear();
    if (doc["networks"].is<JsonArray>()) {
        for (JsonObject n : doc["networks"].as<JsonArray>()) {
            Network net;
            net.ssid = n["ssid"] | "";
            if (net.ssid.isEmpty()) continue;  // сеть без имени бессмысленна

            String password = n["password"] | "";
            // Пустое поле в форме — не «стереть пароль», а «не трогали»:
            // иначе каждое сохранение формы без пароля обнуляло бы его.
            if (password.isEmpty()) password = find_password(prev, net.ssid);
            net.password = password;

            settings.networks.push_back(net);
        }
    } else {
        settings.networks = prev.networks;
    }

    settings.connectors.clear();
    if (doc["connectors"].is<JsonArray>()) {
        for (JsonObject c : doc["connectors"].as<JsonArray>()) {
            Connector conn;
            conn.id = c["id"] | "";
            if (conn.id.isEmpty()) continue;

            // Повторный id в присланной форме — не повод завести второй
            // коннектор с тем же именем: last_polled_ в connectors.cpp и
            // владелец слота в Store ключуются по id, дубликат просто путал
            // бы их между собой. Первая запись побеждает, лишние — молча
            // отбрасываются.
            bool duplicate = false;
            for (const auto& existing : settings.connectors) {
                if (existing.id == conn.id) { duplicate = true; break; }
            }
            if (duplicate) continue;

            conn.kind = c["kind"] | "";
            conn.url = c["url"] | "";
            conn.interval = c["interval"] | 300;
            if (conn.interval < kMinPollIntervalSeconds) conn.interval = kMinPollIntervalSeconds;

            String token = c["token"] | "";
            if (token.isEmpty()) token = find_token(prev, conn.id);
            conn.token = token;

            // Явная галочка, не «оставить прежнее»: отсутствие поля — это
            // false, а не значение из prev. Так безопаснее в обе стороны —
            // и коннектор, добавленный без этого поля, не окажется случайно
            // расшарен как insecure, и обновление формы без явного намерения
            // не унаследует чужую галочку через find по id.
            conn.insecure = c["insecure"] | false;

            // Страница настройки сейчас не умеет редактировать map и не
            // присылает это поле вовсе — отсутствие поля означает «не
            // трогали», а не «очистить», иначе любое сохранение формы стирало
            // бы все привязки слотов у коннектора молча, без признака ошибки.
            if (c["map"].is<JsonArray>()) {
                for (JsonObject m : c["map"].as<JsonArray>()) {
                    SlotMapping sm;
                    sm.slot = m["slot"] | "";
                    sm.source = m["source"] | "";
                    sm.ttl = m["ttl"] | 300;
                    if (sm.slot.isEmpty()) continue;
                    conn.map.push_back(sm);
                }
            } else {
                conn.map = find_map(prev, conn.id);
            }

            settings.connectors.push_back(conn);
        }
    } else {
        settings.connectors = prev.connectors;
    }

    return true;
}

}  // namespace config
