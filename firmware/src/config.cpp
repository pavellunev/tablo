#include "config.h"

#include <ArduinoJson.h>

#include <vector>

// Preferences (NVS) существует только на устройстве. to_json/from_json —
// чистая логика без сети и без флеша, её и гоняют тесты на хосте (env:native,
// NATIVE_BUILD); load/save/reset этой логике не нужны и на хосте не
// собираются — ровно как poll_due в connectors.cpp.
#ifndef NATIVE_BUILD
#include <Preferences.h>
#include <esp_random.h>
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
    s.timezone_minutes = 300;  // Екатеринбург, +05:00

    // Заводские источники. Устройство берёт всё, что доступно публично, само
    // — в этом и смысл проекта: оно не должно зависеть от домашнего сервера,
    // до которого из поездки не дотянуться (docs/decisions.md, п.1).
    //
    // Через Home Assistant идёт только то, что физически живёт дома —
    // датчики воздуха. Вне дома этот коннектор отвалится, его блоки исчезнут,
    // остальной кадр останется живым.

    auto add = [&s](const char* id, const char* url, uint32_t interval,
                    std::initializer_list<SlotMapping> map) {
        Connector c;
        c.id = id;
        c.kind = "http";
        c.url = url;
        c.interval = interval;
        c.insecure = false;
        c.map.assign(map.begin(), map.end());
        s.connectors.push_back(c);
    };

    // Курс биткоина: публичный, без ключа, изменение за сутки тем же запросом.
    //
    // Не CoinGecko, хотя он был первым кандидатом: его цепочку подписывает
    // Google Trust Services, и проверка на устройстве не проходит — в логе
    // «Failed to verify certificate», источник молча пустой. Корень GTS в
    // наборе есть, но цепочка не сходится; разбираться в этом ради одного
    // курса дороже, чем взять источник, чей удостоверяющий центр работает
    // (DigiCert, проверено на живом устройстве).
    add("btc", "https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT", 300,
        {{"btc", "lastPrice", 900}});

    // Официальный курс ЦБ: обновляется раз в сутки, чаще пяти минут спрашивать
    // незачем, но и реже нельзя — иначе утренний курс приедет к обеду.
    add("fiat", "https://www.cbr-xml-daily.ru/daily_json.js", 900,
        {{"usd_rub", "Valute.USD.Value", 86400},
         {"eur_rub", "Valute.EUR.Value", 86400}});

    // Погода по координатам, без ключа и без привязки к дому: в поездке
    // координаты меняются на странице настройки, и блок продолжает работать.
    add("weather", "https://api.open-meteo.com/v1/forecast"
                   "?latitude=56.84&longitude=60.65&current=temperature_2m"
                   "&timezone=Asia%2FYekaterinburg",
        1800,
        {{"weather.temp", "current.temperature_2m", 3600}});

    // Датчики воздуха — единственное, что без Home Assistant взять неоткуда:
    // они физически стоят в кабинете. Токен вводится на странице настройки.
    Connector home;
    home.id = "home";
    home.kind = "homeassistant";
    home.url = "http://192.168.1.2:8123";
    home.interval = 300;
    home.insecure = false;
    home.map.push_back({"co2", "sensor.abinetco2", 600});
    home.map.push_back({"tvoc", "sensor.abinetairquality", 600});
    s.connectors.push_back(home);

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

// ── пароль точки доступа (docs/decisions.md, п.8) ──

// Без символов, которые легко перепутать, читая мелкий растровый шрифт с
// панели и потом набирая на экранной клавиатуре телефона: 0/O, 1/l/I
// выброшены. Только заглавные — набирать их с телефона не сложнее (первый
// тап уже даёт заглавный регистр), а вариантов начертания меньше.
constexpr char kApPasswordAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr size_t kApPasswordAlphabetLen = sizeof(kApPasswordAlphabet) - 1;

// WPA2-Personal требует минимум 8 символов пароля — 10 даёт запас и хорошую
// энтропию (32^10 ≈ 2^50) при том, что от стола его всё ещё можно
// перепечатать без ошибок за один заход.
constexpr size_t kApPasswordLength = 10;

// Строит пароль из потока случайных байт — чистая функция без обращения к
// esp_random(), которого нет на хосте. generate_ap_password() ниже —
// единственный настоящий вызывающий на устройстве; тесты (test_config)
// подают свою детерминированную последовательность байт.
String ap_password_from_bytes(const uint8_t* bytes, size_t count) {
    // std::string, а не Arduino String: у тестового шима нет конкатенации
    // (см. шапку файла про приём из connectors.cpp) — собираем строку и
    // оборачиваем в String один раз на границе.
    std::string result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result += kApPasswordAlphabet[bytes[i] % kApPasswordAlphabetLen];
    }
    return String(result.c_str());
}

#ifndef NATIVE_BUILD
// esp_random() — источник случайности только здесь, тонкой обёрткой поверх
// чистой ap_password_from_bytes(): на хосте esp_random() не существует, а
// саму сборку строки из байт тестировать хочется без ESP-IDF.
String generate_ap_password() {
    uint8_t bytes[kApPasswordLength];
    for (size_t i = 0; i < kApPasswordLength; ++i) {
        bytes[i] = static_cast<uint8_t>(esp_random());
    }
    return ap_password_from_bytes(bytes, kApPasswordLength);
}
#endif

}  // namespace

#ifndef NATIVE_BUILD

Settings load() {
    // Пустая строка, битый JSON или отсутствие самого namespace (первый в
    // жизни устройства запуск: NVS ещё нечего открывать даже на чтение,
    // prefs.begin(readOnly=true) в этом случае возвращает false) — не повод
    // падать: везде одинаково откатываемся к заводским значениям, устройство
    // поднимется в режиме настройки. Раньше здесь был ранний return при
    // неудачном begin(), который пропускал генерацию пароля точки доступа
    // ниже целиком — на первом запуске устройство поднимало точку с пустым
    // паролем (WiFi.softAP() с пустой строкой — открытая сеть), то есть
    // ровно то, что п.8 в docs/decisions.md запрещает.
    Settings settings = defaults();

    Preferences prefs;
    if (prefs.begin(kNamespace, /*readOnly=*/true)) {
        String raw = prefs.getString(kKey, "");
        prefs.end();
        if (!raw.isEmpty() && !from_json(raw, settings)) {
            settings = defaults();
        }

        // Сохранённые настройки без единого источника данных — это устройство,
        // настроенное до того, как источники появились в прошивке: человек
        // ввёл сеть, конфигурация записалась, и заводские значения больше не
        // применяются никогда. Экран в такой ситуации остаётся пустым без
        // единой ошибки. Подставляем заводские источники, не трогая сети и
        // пароль точки доступа — их владелец задавал сам.
        if (settings.connectors.empty()) {
            settings.connectors = defaults().connectors;
        }
    }

    // Пароль точки доступа генерируется один раз и живёт в NVS дальше —
    // «пусто» бывает и на первом старте, и сразу после reset() (docs/
    // decisions.md, п.8: постоянный пароль — единственный способ его сменить
    // тоже reset()). save() здесь — не побочный эффект, а единственный момент
    // записи: без него сгенерированный пароль не пережил бы перезагрузку и
    // генерировался бы заново на каждом старте, то есть остался бы тем же
    // «новым каждый раз», от которого мы уходим.
    if (settings.ap_password.isEmpty()) {
        settings.ap_password = generate_ap_password();
        save(settings);
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

    // Секрет ровно как токен коннектора ниже: наружу (GET /api/config,
    // include_secrets=false) не отдаём вовсе, не только маскируем — странице
    // настройки его знать незачем, она его даже не показывает. Внутреннее
    // хранилище (config::save) зовёт to_json(..., true), иначе пароль
    // терялся бы при каждой перезагрузке.
    if (include_secrets) {
        doc["ap_password"] = settings.ap_password.c_str();
    }

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

    // Страница настройки это поле не присылает вовсе (см. to_json выше) —
    // отсутствие в присланном JSON здесь всегда означает «не трогали», а не
    // «стереть». Единственный писатель самого поля — load() в момент первой
    // генерации; from_json лишь переносит его через сохранение формы.
    if (doc["ap_password"].is<const char*>()) {
        settings.ap_password = doc["ap_password"].as<const char*>();
    } else {
        settings.ap_password = prev.ap_password;
    }

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
