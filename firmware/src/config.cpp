#include "config.h"

#include "secrets.h"

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
constexpr const char* kNamespace = "tablo";
// Два ключа — намеренно. Раньше настройки лежали строкой под «settings»;
// blob под тем же ключом NVS не примет: тип записи не совпадает, nvs_set_blob
// отвечает TYPE_MISMATCH, и save() возвращал false — на уже настроенном
// устройстве после обновления прошивки НИЧЕГО нельзя было сохранить, а
// выглядело оно здоровым: чтение-то шло через откат на строку. Поэтому blob
// живёт под своим ключом, старая строка читается как запасной источник и
// удаляется только после успешной записи blob — окна без настроек нет.
constexpr const char* kKeyLegacy = "settings";    // строка, формат до blob
constexpr const char* kKey = "settings_b";         // blob

Settings defaults() {
    Settings s;
    s.device_name = "tablo-setup";
    // Домашняя сеть — заводская, из secrets.h: после перепрошивки или сброса
    // устройство подключается само, а не ждёт настройки с телефона. Пустой
    // SSID в secrets — сети нет, поднимется точка доступа как раньше.
    if (String(WIFI_SSID_DEFAULT).length() > 0) {
        Network home_net;
        home_net.ssid = WIFI_SSID_DEFAULT;
        home_net.password = WIFI_PASSWORD_DEFAULT;
        s.networks.push_back(home_net);
    }
    s.timezone_minutes = 300;  // Екатеринбург, +05:00 — до первого геокодинга

    // Заводской город. Координаты и city_resolved заполнены заранее (а не
    // оставлены пустыми до первого выхода в сеть), чтобы погода работала
    // сразу на новом устройстве — коннектор kind="geocode" молчит, пока
    // city == city_resolved (docs/constructor.md, «Город вместо координат»).
    s.city = "Екатеринбург";
    s.city_resolved = "Екатеринбург";
    s.city_lat = 56.8389f;
    s.city_lon = 60.6057f;

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
    // priceChangePercent — то же тело ответа, что и текущая цена: дельта не
    // требует отдельного похода в сеть (docs/decisions.md, задача про дельты).
    //
    // Не CoinGecko, хотя он был первым кандидатом: его цепочку подписывает
    // Google Trust Services, и проверка на устройстве не проходит — в логе
    // «Failed to verify certificate», источник молча пустой. Корень GTS в
    // наборе есть, но цепочка не сходится; разбираться в этом ради одного
    // курса дороже, чем взять источник, чей удостоверяющий центр работает
    // (DigiCert, проверено на живом устройстве).
    add("btc", "https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT", 300,
        {{"btc", "lastPrice", 900, "priceChangePercent"}});

    // История курса за 24 часа — отдельным запросом и отдельным интервалом
    // опроса (30 минут, не 5): часовые свечи всё равно не меняются чаще, а
    // накопление одной точки на каждый обычный опрос в Store уже показало
    // себя лживым (подпись «ЗА 24 Ч» при часе реальных данных). "4" — индекс
    // цены закрытия в каждой свече Binance ([open_time, open, high, low,
    // close, ...]).
    {
        Connector btc_history;
        btc_history.id = "btc_history";
        btc_history.kind = "http";
        btc_history.url = "https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=24";
        btc_history.interval = 1800;
        SlotMapping history_map;
        history_map.slot = "btc";
        history_map.has_history = true;
        history_map.history_item = "4";
        btc_history.map.push_back(history_map);
        s.connectors.push_back(btc_history);
    }

    // Официальный курс ЦБ: обновляется раз в сутки, чаще пяти минут спрашивать
    // незачем, но и реже нельзя — иначе утренний курс приедет к обеду.
    // Value/Previous — то, что реально отдаёт ЦБ (готового процента
    // изменения в ответе нет), delta_is_previous просит parse_http_response
    // посчитать (value-previous)/previous*100 самому.
    add("fiat", "https://www.cbr-xml-daily.ru/daily_json.js", 900,
        {{"usd_rub", "Valute.USD.Value", 86400, "Valute.USD.Previous", true},
         {"eur_rub", "Valute.EUR.Value", 86400, "Valute.EUR.Previous", true}});

    // Погода — по координатам города (city_lat/city_lon в Settings, не в
    // адресе коннектора: они меняются геокодингом, а не формой настройки).
    // kind="weather" сам строит адрес на каждый опрос из текущих координат;
    // map ниже — как у обычного http, current/daily разбираются тем же
    // walk_path (индекс массива daily.*.0 уже умеет).
    {
        Connector weather;
        weather.id = "weather";
        weather.kind = "weather";
        weather.interval = 1800;
        weather.map.push_back({"weather.temp", "current.temperature_2m", 3600});
        weather.map.push_back({"weather.low", "daily.temperature_2m_min.0", 86400});
        weather.map.push_back({"weather.high", "daily.temperature_2m_max.0", 86400});
        s.connectors.push_back(weather);
    }

    // Геокодинг города в координаты (docs/constructor.md, «Погода и часовой
    // пояс»): молчит, пока Settings.city_resolved == Settings.city — то есть
    // почти всегда, кроме первого запуска после смены города владельцем.
    // В режиме точки доступа сети нет вовсе (poll_due не вызывается —
    // main.cpp), поэтому геокодинг честно откладывается до первого выхода в
    // сеть, как и требует задача.
    {
        Connector geocode;
        geocode.id = "geocode";
        geocode.kind = "geocode";
        geocode.interval = 3600;  // раз в час достаточно проверять «не сменился ли город»
        s.connectors.push_back(geocode);
    }

    // Датчики воздуха — единственное, что без Home Assistant взять неоткуда:
    // они физически стоят в кабинете. Токен вводится на странице настройки.
    // has_history — история за 6 часов тем же периодом, что использовал
    // предшественник (trmnl-ink/renderer/app/providers/air.py, HISTORY_WINDOW).
    Connector home;
    home.id = "home";
    home.kind = "homeassistant";
    home.url = HA_URL_DEFAULT;
    home.token = HA_TOKEN_DEFAULT;  // см. secrets.h, в репозиторий не попадает
    home.interval = 300;
    home.insecure = false;
    {
        SlotMapping co2_map;
        co2_map.slot = "co2";
        co2_map.source = "sensor.abinetco2";
        co2_map.ttl = 600;
        co2_map.has_history = true;
        home.map.push_back(co2_map);

        SlotMapping tvoc_map;
        tvoc_map.slot = "tvoc";
        tvoc_map.source = "sensor.abinetairquality";
        tvoc_map.ttl = 600;
        tvoc_map.has_history = true;
        home.map.push_back(tvoc_map);
    }
    s.connectors.push_back(home);

    // Лимиты AI-подписок и почта — устройство ходит за ними само (OAuth и
    // IMAP разбираются прямо на нём, см. connectors.cpp и docs/decisions.md,
    // п.2 — решение и его причина записаны там честно, включая то, что
    // раньше здесь стоял посредник-приложение). Токены и пароль ящика — в
    // secrets.h, не в этом файле, ровно как HA_TOKEN_DEFAULT выше.

    // Лимиты Claude: access_token живёт около восьми часов, дальше API
    // отвечает 401 — connectors.cpp обновляет его сам через refresh_token
    // (POST на console.anthropic.com/v1/oauth/token) и сохраняет новую пару
    // в NVS через config::save(), иначе обновление не пережило бы перезагрузку.
    Connector claude;
    claude.id = "claude";
    claude.kind = "anthropic";
    claude.token = CLAUDE_ACCESS_TOKEN_DEFAULT;
    claude.refresh_token = CLAUDE_REFRESH_TOKEN_DEFAULT;
    claude.interval = 300;  // 5 минут — TLS-соединение недёшево, лимит не скачет секундами
    s.connectors.push_back(claude);

    // Лимиты Codex (ChatGPT): тот же принцип Bearer-токена, но без
    // обновления — задача явно описывает refresh только для Claude; протухший
    // токен здесь просто гасит слот до следующей ручной подстановки в secrets.h.
    Connector codex;
    codex.id = "codex";
    codex.kind = "codex";
    codex.token = CODEX_ACCESS_TOKEN_DEFAULT;
    codex.refresh_token = CODEX_REFRESH_TOKEN_DEFAULT;  // обновление пары — как у Claude (connectors.cpp, try_refresh)
    codex.interval = 300;
    s.connectors.push_back(codex);

    // Непрочитанные письма: IMAP поверх TLS, три команды протокола
    // (LOGIN/SELECT/SEARCH UNSEEN) — не полноценный клиент, он тут не нужен.
    // url хранит хост IMAP-сервера (не URL в привычном смысле — то же поле,
    // что у остальных коннекторов, лишнего не заводим).
    Connector mail;
    mail.id = "mail";
    mail.kind = "imap";
    mail.url = "imap.gmail.com";
    mail.username = IMAP_USER_DEFAULT;
    mail.token = IMAP_PASSWORD_DEFAULT;  // пароль приложения Gmail, по аналогии с token
    mail.interval = 900;  // 15 минут — почта не требует опроса чаще
    s.connectors.push_back(mail);

    // Заводские дашборды (docs/widgets.md) — «Стол» дословно воспроизводит
    // эталон (reference/cockpit-reference.png): Рынки + Лимиты-и-Воздух
    // сверху, Почта + Сегодня снизу, разделитель слева от Сегодня — как
    // сейчас у draw_mail/draw_today в layout.cpp. «Дорога» — тот же каркас
    // без датчиков дома (widgets::limits вместо widgets::limits_air): что
    // показывать вне дома, где Home Assistant недоступен. «Свой» — пустой
    // холст, чтобы было куда собирать своё.
    auto widget = [](const char* type, widgets::Size size, bool divider = false) {
        widgets::Instance i;
        i.type = type;
        i.size = size;
        i.divider = divider;
        return i;
    };

    {
        Dashboard desk;
        desk.name = "Стол";
        desk.rows[0].push_back(widget("markets", widgets::Size::kM));
        desk.rows[0].push_back(widget("limits_air", widgets::Size::kFlex));
        desk.rows[1].push_back(widget("mail", widgets::Size::kFlex));
        desk.rows[1].push_back(widget("today", widgets::Size::kS, /*divider=*/true));
        s.dashboards.push_back(desk);
    }
    {
        Dashboard road;
        road.name = "Дорога";
        road.rows[0].push_back(widget("markets", widgets::Size::kM));
        road.rows[0].push_back(widget("limits", widgets::Size::kFlex));
        road.rows[1].push_back(widget("mail", widgets::Size::kFlex));
        road.rows[1].push_back(widget("today", widgets::Size::kS, /*divider=*/true));
        s.dashboards.push_back(road);
    }
    {
        Dashboard own;
        own.name = "Свой";
        s.dashboards.push_back(own);
    }
    s.active_dashboard = 0;

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

const Connector* find_connector(const Settings& prev, const String& id) {
    for (const auto& conn : prev.connectors) {
        if (conn.id == id) return &conn;
    }
    return nullptr;
}

// Секрет не переезжает на новый адрес. Форма присылает пустое поле токена в
// смысле «не трогали», и устройство подставляет сохранённый. Пока запись
// настроек была разрешена только из точки доступа, этого хватало; теперь
// форму можно сохранить из любой сети, в которой устройство оказалось
// (docs/decisions.md, п.8), — и сосед по гостиничному Wi-Fi мог бы прислать
// тот же коннектор со СВОИМ адресом и пустым токеном: устройство само
// отнесло бы ему токен Home Assistant или пароль почты. Поэтому прежний
// секрет наследуется только если цель осталась той же: адрес не менялся и
// проверку сертификата не выключали (с выключенной проверкой тот же адрес
// подменяется DNS-ом чужой сети). Сменил адрес — введи секрет заново; это
// единственное, что владелец теперь делает руками, и делает редко.
bool target_unchanged(const Connector* previous, const Connector& next) {
    if (previous == nullptr) return true;  // наследовать всё равно нечего
    if (previous->url != next.url) return false;
    // kind задаёт протокол, которым секрет уходит на адрес (Bearer к HA,
    // LOGIN к IMAP, OAuth к Anthropic): подменить kind при том же адресе —
    // тот же увод секрета другим путём.
    if (previous->kind != next.kind) return false;
    if (!previous->insecure && next.insecure) return false;
    return true;
}

// По той же схеме, что и token: секрет ищется у прежнего коннектора по id,
// а не по индексу — форма могла переставить коннекторы местами.
String find_refresh_token(const Settings& prev, const String& id) {
    for (const auto& conn : prev.connectors) {
        if (conn.id == id) return conn.refresh_token;
    }
    return "";
}

String find_username(const Settings& prev, const String& id) {
    for (const auto& conn : prev.connectors) {
        if (conn.id == id) return conn.username;
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

// Читает сохранённый JSON как blob (см. save() — putBytes, не putString, из-за
// предела строк NVS в 4000 байт). getBytesLength()==0 означает и «ключа нет
// вовсе» (первый запуск), и «ключ есть, но другого типа» — оба случая здесь
// неотличимы, но для обоих правильно попробовать старый putString()-формат:
// устройства, настроенные ДО этой правки, хранят JSON именно так, и без этого
// отката load() увидел бы их как пустые настройки — терялись бы сети,
// коннекторы, пароль точки доступа. Следующий save() перезапишет запись уже
// блобом, второй раз откат не понадобится.
String read_raw_settings(Preferences& prefs) {
    size_t blob_len = prefs.getBytesLength(kKey);
    if (blob_len > 0) {
        std::vector<char> buf(blob_len + 1);
        prefs.getBytes(kKey, buf.data(), blob_len);
        buf[blob_len] = '\0';
        return String(buf.data());
    }
    // Под старым ключом может лежать И строка (совсем старая прошивка), И blob
    // (промежуточная версия писала blob под этим же ключом). getString на
    // blob-ключе молча возвращает пусто — ровно так одна из заливок приняла
    // настроенное устройство за чистое, записала заводские значения и стёрла
    // ключ с настоящими настройками. Поэтому пробуем оба типа.
    size_t legacy_blob = prefs.getBytesLength(kKeyLegacy);
    if (legacy_blob > 0) {
        std::vector<char> buf(legacy_blob + 1);
        prefs.getBytes(kKeyLegacy, buf.data(), legacy_blob);
        buf[legacy_blob] = '\0';
        Serial.println("config: настройки прочитаны из старого ключа (blob)");
        return String(buf.data());
    }
    String legacy = prefs.getString(kKeyLegacy, "");
    if (legacy.length() > 0) {
        Serial.println("config: настройки прочитаны из старого ключа (строка)");
    }
    return legacy;
}
#endif

// Устройство, уже настроенное до появления новых заводских источников
// (btc_history, geocode, «weather» сменил kind с http на специализированный)
// не должно ждать полного сброса настроек, чтобы их получить — иначе
// обновление прошивки на живом устройстве тихо теряет часть кадра. Довешивает
// отсутствующие по id заводские коннекторы, не трогая то, что владелец уже
// настроил сам (токены, url, map пользовательских записей не задеты).
// Коннектор, у которого сменился kind (единственный прецедент — «weather»),
// заменяется заводским целиком: он не несёт секретов, которые было бы жаль
// потерять, а старый kind без этой замены продолжал бы работать по старой
// схеме (статические координаты) вечно.
void merge_missing_factory_connectors(Settings& settings) {
    Settings factory = defaults();
    for (const auto& fc : factory.connectors) {
        bool exists = false;
        for (auto& ec : settings.connectors) {
            if (ec.id == fc.id) {
                exists = true;
                if (ec.kind != fc.kind) ec = fc;
                break;
            }
        }
        if (!exists) settings.connectors.push_back(fc);
    }
}

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
    bool from_legacy = false;
    bool read_failed = false;
    bool legacy_leftover = false;
    bool networks_restored = false;
    if (prefs.begin(kNamespace, /*readOnly=*/true)) {
        const bool any_key = prefs.isKey(kKey) || prefs.isKey(kKeyLegacy);
        // Оба ключа проверяем, пока хранилище открыто: после prefs.end()
        // Preferences молча отвечает «нет» на любой вопрос, и флаг ниже был бы
        // всегда ложным — зачистка мёртвого ключа не сработала бы никогда.
        const bool both_keys = prefs.getBytesLength(kKey) > 0 && prefs.isKey(kKeyLegacy);
        String raw = read_raw_settings(prefs);
        // Переносить в blob можно только то, что реально прочитано И разобрано:
        // пустая строка или битый JSON — не «нет настроек», а «не смогли
        // прочитать», и запись заводских значений поверх стирает настоящие.
        from_legacy = prefs.getBytesLength(kKey) == 0 && prefs.isKey(kKeyLegacy) &&
                      !raw.isEmpty();
        prefs.end();
        bool parsed = false;
        if (!raw.isEmpty()) {
            parsed = from_json(raw, settings);
            if (!parsed) {
                settings = defaults();
                from_legacy = false;
            }
        }
        // Ключ есть, а содержимого не получили — запись в этой загрузке
        // запрещена целиком: заводские значения живут в ОЗУ, NVS не трогаем,
        // настоящие настройки остаются восстановимыми.
        read_failed = any_key && !parsed;
        // Оба ключа сразу — обрыв питания между записью blob и удалением
        // старого ключа в прошлой миграции. Данные уже взяты из blob, старый
        // ключ только занимает ~4 КБ из 20 и роняет потолок обновлений.
        legacy_leftover = parsed && both_keys;

        // Сохранённые настройки без единого источника данных — это устройство,
        // настроенное до того, как источники появились в прошивке: человек
        // ввёл сеть, конфигурация записалась, и заводские значения больше не
        // применяются никогда. Экран в такой ситуации остаётся пустым без
        // единой ошибки. Подставляем заводские источники, не трогая сети и
        // пароль точки доступа — их владелец задавал сам.
        // Сохранённые настройки без единой сети — устройство, чьи настройки
        // стёрла неудачная миграция, либо записанные до появления заводской
        // сети в secrets.h. Без подстановки оно навсегда в точке доступа,
        // хотя домашняя сеть известна.
        if (settings.networks.empty() && !defaults().networks.empty()) {
            settings.networks = defaults().networks;
            networks_restored = true;
        }
        if (settings.connectors.empty()) {
            settings.connectors = defaults().connectors;
        } else {
            merge_missing_factory_connectors(settings);
        }
    }

    // Пароль точки доступа генерируется один раз и живёт в NVS дальше —
    // «пусто» бывает и на первом старте, и сразу после reset() (docs/
    // decisions.md, п.8: постоянный пароль — единственный способ его сменить
    // тоже reset()). save() здесь — не побочный эффект, а единственный момент
    // записи: без него сгенерированный пароль не пережил бы перезагрузку и
    // генерировался бы заново на каждом старте, то есть остался бы тем же
    // «новым каждый раз», от которого мы уходим.
    if (read_failed) {
        Serial.println("config: НАСТРОЙКИ НЕ ПРОЧИТАНЫ — запись заблокирована, NVS не тронут");
        if (settings.ap_password.isEmpty()) settings.ap_password = generate_ap_password();
        return settings;  // пароль точки — на эту сессию, без сохранения
    }
    if (settings.ap_password.isEmpty()) {
        settings.ap_password = generate_ap_password();
        save(settings);
    }
    if (networks_restored && !from_legacy) {
        // Домашняя сеть подставлена в настройки без сетей — сохраняем один раз,
        // чтобы дальше список сетей жил как есть и правился владельцем.
        // Удаление всех сетей — не способ попасть в точку доступа (для этого
        // предназначена кнопка, см. docs/decisions.md п.8), так что конфликта
        // с намерением владельца здесь нет.
        Serial.println(save(settings) ? "config: домашняя сеть восстановлена и сохранена"
                                      : "config: домашняя сеть восстановлена, но НЕ сохранена");
    }
    if (legacy_leftover) {
        Preferences cleanup;
        if (cleanup.begin(kNamespace, /*readOnly=*/false)) {
            cleanup.remove(kKeyLegacy);
            cleanup.end();
            Serial.println("config: удалён мёртвый старый ключ настроек");
        }
    }
    if (from_legacy) {
        // Переносим в blob здесь и сейчас: следующее сохранение может
        // случиться через сутки (refresh токена), и всё это время устройство
        // выглядело бы здоровым при неработающей записи. Старый ключ удаляем
        // только здесь — после того как сами его прочитали, разобрали и
        // успешно записали новый. Результат — в лог.
        if (save(settings)) {
            Preferences cleanup;
            if (cleanup.begin(kNamespace, /*readOnly=*/false)) {
                cleanup.remove(kKeyLegacy);
                cleanup.end();
            }
            Serial.println("config: настройки перенесены в blob, старый ключ удалён");
        } else {
            Serial.println("config: ПЕРЕНОС В BLOB НЕ УДАЛСЯ — запись настроек не работает");
        }
    }
    return settings;
}

bool save(const Settings& settings) {
    // Сохранённый JSON всегда включает секреты — это внутреннее хранилище
    // (NVS), наружу токены и пароли отдаёт только to_json(..., false).
    String raw = to_json(settings, /*include_secrets=*/true);

    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
    // putBytes (тип "blob" в NVS), не putString: у строк в NVS жёсткий предел
    // 4000 байт на значение (nvs_set_str, включая завершающий '\0'), у блобов
    // — формально до 508000, но раздел nvs конечен (partitions.csv: 128 КБ,
    // 32 страницы по 4 КБ, одна всегда свободна под сборку мусора), а
    // обновление blob пишет новую копию до стирания старой. На прежних 20 КБ
    // blob в 5,9 КБ (четыре токена + три дашборда) уже не перезаписывался —
    // nvs_set_blob отказывал, и ни одно сохранение не проходило (2026-09-22).
    // Сейчас потолок ≈ 55 КБ; дальше save() честно вернёт false. Полный
    // JSON с реальными токенами (OAuth Claude/Codex — сотни-полторы тысяч
    // символов каждый) и картой коннекторов уже сам по себе подходил к этой
    // границе вплотную; добавление delta/history-полей и города в эту же
    // задачу вытолкнуло его за 4000 — putString() не просто отказывала с
    // ESP_ERR_NVS_VALUE_TOO_LONG, а роняла устройство в LoadProhibited
    // (поймано на живом устройстве, см. Status Log). Данные и так не текст в
    // пользовательском смысле, а непрозрачный JSON — блоб им подходит не
    // хуже строки.
    size_t written = prefs.putBytes(kKey, raw.c_str(), raw.length());
    // Старый ключ здесь НЕ удаляем: save() не знает, откуда пришли данные.
    // Удаление делает только load() в ветке миграции — единственное место,
    // которое само прочитало и разобрало содержимое старого ключа. Иначе
    // save(заводские) из любой другой ветки стирал бы настоящие настройки.
    prefs.end();
    if (written != raw.length()) {
        // Отказ здесь молчать не должен: раньше он был невидим, и владелец
        // узнал бы о нём по мёртвым лимитам после ротации токена.
        Serial.printf("config: НЕ СОХРАНЕНО (%u из %u байт) — см. потолок размера выше\n",
                      static_cast<unsigned>(written), static_cast<unsigned>(raw.length()));
    }
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

    // Город и координаты — не секрет, отдаём всегда (не только внутреннему
    // хранилищу): страница настройки должна честно показать, определились
    // ли уже координаты, или город ещё ждёт первого выхода в сеть
    // (docs/constructor.md, «Тонкость»). Хранить их нужно и во внутреннем
    // JSON (include_secrets=true), иначе геокодинг терялся бы на каждой
    // перезагрузке, как терялся бы map без своей строки в этом же файле.
    doc["city"] = settings.city.c_str();
    doc["city_resolved"] = settings.city_resolved.c_str();
    doc["city_lat"] = settings.city_lat;
    doc["city_lon"] = settings.city_lon;

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
            o["refresh_token"] = conn.refresh_token.c_str();
            o["username"] = conn.username.c_str();
        } else {
            // refresh_token/username — секреты того же класса, что token
            // (OAuth-токен Claude и логин почты), сокрыты тем же приёмом.
            o["token_set"] = !conn.token.isEmpty();
            o["refresh_token_set"] = !conn.refresh_token.isEmpty();
            o["username_set"] = !conn.username.isEmpty();
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
            // Не секреты — заводские мэппинги (btc, fiat, home, btc_history)
            // должны пережить перезагрузку так же, как slot/source/ttl выше;
            // без этих трёх строк дельта и история заводских источников
            // стирались бы на первом же сохранении настроек (тот же класс
            // бага, что уже чинили с map целиком — см. Status Log).
            mo["delta_source"] = m.delta_source.c_str();
            mo["delta_is_previous"] = m.delta_is_previous;
            mo["has_history"] = m.has_history;
            mo["history_source"] = m.history_source.c_str();
            mo["history_item"] = m.history_item.c_str();
        }
    }

    // Дашборды — не секрет, отдаём всегда (и в NVS, и странице настройки),
    // компактно: slot/label только если непустые, divider только если true
    // (потолок NVS ≈ 8 КБ, см. save() — три дашборда не должны раздувать blob
    // заметно). type/size пишем всегда — без них виджет не восстановить.
    doc["active_dashboard"] = settings.active_dashboard;
    JsonArray dashboards = doc["dashboards"].to<JsonArray>();
    for (const auto& db : settings.dashboards) {
        JsonObject dobj = dashboards.add<JsonObject>();
        dobj["name"] = db.name.c_str();
        JsonArray rows = dobj["rows"].to<JsonArray>();
        for (const auto& row : db.rows) {
            JsonArray row_arr = rows.add<JsonArray>();
            for (const auto& item : row) {
                JsonObject iobj = row_arr.add<JsonObject>();
                iobj["type"] = item.type.c_str();
                iobj["size"] = widgets::size_to_string(item.size);
                if (item.divider) iobj["divider"] = true;
                if (!item.slot.isEmpty()) iobj["slot"] = item.slot.c_str();
                if (!item.label.isEmpty()) iobj["label"] = item.label.c_str();
            }
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

    // city — редактируемое поле формы (страница настройки шлёт его всегда,
    // как device_name); city_resolved/city_lat/city_lon — внутренняя
    // бухгалтерия геокодинга, форма их не присылает вовсе, и отсутствие в
    // присланном JSON здесь всегда означает «не трогали» — тем же приёмом,
    // что ap_password выше.
    if (doc["city"].is<const char*>()) {
        settings.city = doc["city"].as<const char*>();
    } else {
        settings.city = prev.city;
    }
    if (doc["city_resolved"].is<const char*>()) {
        settings.city_resolved = doc["city_resolved"].as<const char*>();
    } else {
        settings.city_resolved = prev.city_resolved;
    }
    settings.city_lat = doc["city_lat"] | prev.city_lat;
    settings.city_lon = doc["city_lon"] | prev.city_lon;

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

            // Явная галочка, не «оставить прежнее»: отсутствие поля — это
            // false, а не значение из prev. Так безопаснее в обе стороны —
            // и коннектор, добавленный без этого поля, не окажется случайно
            // расшарен как insecure, и обновление формы без явного намерения
            // не унаследует чужую галочку через find по id.
            conn.insecure = c["insecure"] | false;

            // Пустое поле секрета — «не трогали», но только пока цель та же
            // (см. target_unchanged выше). Читается до токенов: url и
            // insecure уже разобраны, сравнивать есть с чем.
            const bool inherit = target_unchanged(find_connector(prev, conn.id), conn);

            String token = c["token"] | "";
            if (token.isEmpty() && inherit) token = find_token(prev, conn.id);
            conn.token = token;

            // Тот же приём «пустое поле — не трогали»: страница настройки
            // не отдаёт refresh_token/username обратно (to_json их прячет),
            // а любая нормальная форма без явного значения не должна их стереть.
            String refresh_token = c["refresh_token"] | "";
            if (refresh_token.isEmpty() && inherit) refresh_token = find_refresh_token(prev, conn.id);
            conn.refresh_token = refresh_token;

            String username = c["username"] | "";
            if (username.isEmpty() && inherit) username = find_username(prev, conn.id);
            conn.username = username;

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
                    sm.delta_source = m["delta_source"] | "";
                    sm.delta_is_previous = m["delta_is_previous"] | false;
                    sm.has_history = m["has_history"] | false;
                    sm.history_source = m["history_source"] | "";
                    sm.history_item = m["history_item"] | "";
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

    // Дашборды — тем же приёмом «нет ключа — не трогали», что networks/
    // connectors выше. Присутствие ключа заменяет ВСЕ три целиком —
    // merge-логики нет намеренно (docs/widgets.md): владелец вправе
    // опустошить любой дашборд, наследовать тут нечего, это не секрет.
    if (doc["dashboards"].is<JsonArray>()) {
        settings.dashboards.clear();
        for (JsonObject dobj : doc["dashboards"].as<JsonArray>()) {
            if (settings.dashboards.size() >= 3) break;  // лишние дашборды отбрасываются

            Dashboard db;
            db.name = dobj["name"] | "";

            if (dobj["rows"].is<JsonArray>()) {
                int r = 0;
                for (JsonArray row : dobj["rows"].as<JsonArray>()) {
                    if (r >= 2) break;  // не больше 2 рядов
                    for (JsonObject iobj : row) {
                        if (db.rows[r].size() >= 6) break;  // не больше 6 виджетов в ряду

                        const char* type = iobj["type"] | "";
                        const widgets::TypeInfo* info = widgets::find_type(type);
                        if (info == nullptr) continue;  // неизвестный type — виджет отбрасывается

                        widgets::Instance item;
                        item.type = type;
                        const char* size_str = iobj["size"] | "";
                        if (!widgets::size_from_string(size_str, item.size)) {
                            item.size = info->default_size;  // неизвестный size -> default_size спеки
                        }
                        item.divider = iobj["divider"] | false;
                        item.slot = iobj["slot"] | "";
                        item.label = iobj["label"] | "";
                        db.rows[r].push_back(item);
                    }
                    ++r;
                }
            }

            settings.dashboards.push_back(db);
        }
        // Недостающие дашборды — пустые, а не отсутствующие: активный индекс
        // (ниже) всегда должен указывать на существующую запись.
        while (settings.dashboards.size() < 3) settings.dashboards.push_back(Dashboard{});
    } else {
        settings.dashboards = prev.dashboards;
    }

    settings.active_dashboard = doc["active_dashboard"] | prev.active_dashboard;
    if (settings.active_dashboard > 2) settings.active_dashboard = 0;

    return true;
}

}  // namespace config
