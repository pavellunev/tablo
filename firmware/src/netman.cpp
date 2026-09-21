#include "netman.h"

#include <ESPmDNS.h>  // MDNS.begin — страница доступна как http://<имя>.local/ из домашней сети
#include <WiFi.h>

#include <atomic>

#include <algorithm>

#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace netman {

namespace {

// Сколько ждём подключения к выбранной сети, прежде чем признать попытку
// неудачной. Больше — устройство дольше висит с погашенным экраном при
// недоступной сети; меньше — не успевает пройти DHCP в медленных гостиничных
// роутерах.
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kConnectPollMs = 200;

// Как часто из режима точки доступа пробуем вернуться в сохранённую сеть.
// Не слишком часто — сканирование Wi-Fi недёшево и на секунду замораживает
// стек, не слишком редко — иначе дома владелец ждёт минуты вместо секунд.
constexpr uint32_t kApRetryIntervalMs = 60000;

// Долгое удержание BTN1 (buttons.h) поднимает точку доступа принудительно —
// на этот срок ретрай в станцию не пробуется вовсе, даже без подключённого
// AP-клиента (см. force_access_point()/forced_hold_until ниже).
constexpr uint32_t kForcedApHoldMs = 3 * 60 * 1000;

// Кратковременный провал WiFi.status() (роутер на секунду перезагрузился,
// доля секунды роуминга) не должен сразу ронять устройство в точку доступа —
// это дороже: рвётся станционное соединение, и через минуту снова придётся
// сканировать эфир и подключаться заново. Ждём подтверждения разрыва.
constexpr uint32_t kDisconnectGraceMs = 5000;

config::Settings current_settings;
Mode current_mode = Mode::kConnecting;
uint32_t last_retry_at = 0;
// 0 — обычный режим (ретраи по kApRetryIntervalMs); millis() до этого
// значения — принудительная точка доступа (force_access_point()) держится
// вне зависимости от обычного расписания ретрая.
uint32_t forced_hold_until = 0;
uint32_t disconnected_since_ms = 0;  // 0 — сейчас подключены или ещё не фиксировали разрыв

String current_ap_ssid;
String current_ap_password;

// portal.cpp (обработчик /api/config) выполняется в задаче async_tcp, а не в
// loop(). reload() зовётся оттуда и не может напрямую трогать
// current_settings — try_connect_best() в этот момент способен перебирать
// тот же vector<Connector> в главном цикле, и одновременное
// присваивание/перебор одного std::vector<String> — use-after-free (был
// источником случайных крешей при сохранении настроек). Поэтому reload()
// только выставляет флаг, а current_settings обновляет сам главный цикл в
// начале своего тика, перечитывая уже сохранённые в NVS настройки.
std::atomic<bool> settings_reload_pending{false};

// Настройки только что сохранены со страницы — подключиться нужно сразу, не
// дожидаясь ухода клиента с точки доступа. Обычное правило «не трогать
// станцию, пока кто-то подключён» здесь работает против владельца: он ввёл
// сеть и как раз поэтому подключён, а переход не наступает никогда.
bool connect_now = false;

// WiFi.scanNetworks() внутри освобождает результат предыдущего скана —
// сериализуем доступ к сканеру мьютексом. Раньше этот же мьютекс защищал и
// саму блокирующую операцию (WiFi.scanNetworks(), до ~10с), и кеш результатов
// — а брал его в том числе /api/scan прямо из задачи async_tcp. Если в этот
// момент главный цикл уже держал мьютекс, выполняя скан, async_tcp ждал его
// освобождения ДО 10 СЕКУНД — у этой задачи свой сторожевой таймер и
// ограниченный стек, и оба не переживают такую паузу: устройство
// перезагружалось на кнопке «Сканировать сети» на странице настройки.
//
// Два разных мьютекса решают это разделением ролей:
// - scan_op_mutex сериализует саму блокирующую операцию и берётся только из
//   главного цикла (try_connect_best() и обработка scan_requested в loop());
// - cache_mutex защищает исключительно копирование вектора cached_scan_results
//   и держится микросекунды — его безопасно брать откуда угодно, в том числе
//   из async_tcp, независимо от того, идёт ли сейчас скан.
//
// Ленивая инициализация обоих — не глобальный конструктор: на некоторых
// сборках ESP32 Arduino статические объекты с side-effect могут отработать
// раньше, чем поднят FreeRTOS, а xSemaphoreCreateMutex() до старта
// планировщика — это undefined behavior.
SemaphoreHandle_t scan_op_mutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

SemaphoreHandle_t cache_mutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

std::vector<ScanResult> cached_scan_results;
uint32_t cached_scan_at_ms = 0;
// Эфир в помещении не меняется по несколько раз в секунду — 5 секунд кеша не
// заметны владельцу, а старее — главный цикл сам поставит скан на обновление
// (см. scan_status()/scan_requested ниже), не блокируя того, кто спрашивает.
constexpr uint32_t kScanCacheMs = 5000;

// Запрошен ли скан (главным циклом ещё не начат) и идёт ли он прямо сейчас —
// то же разделение флагов, что у settings_reload_pending/reboot_pending
// (portal.cpp): async_tcp только просит, физическую работу делает loop().
std::atomic<bool> scan_requested{false};
std::atomic<bool> scan_running{false};

std::vector<ScanResult> scan_locked() {
    std::vector<ScanResult> results;

    int n = WiFi.scanNetworks();
    if (n <= 0) return results;

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;  // скрытые сети без имени не сопоставить с сохранёнными

        int32_t rssi = WiFi.RSSI(i);
        bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

        bool merged = false;
        for (auto& r : results) {
            if (r.ssid != ssid) continue;
            // Одна сеть видна с нескольких точек доступа — оставляем ту
            // запись, что показывает более сильный сигнал.
            if (rssi > r.rssi) {
                r.rssi = rssi;
                r.secure = secure;
            }
            merged = true;
            break;
        }
        if (!merged) results.push_back({ssid, rssi, secure});
    }
    WiFi.scanDelete();

    std::sort(results.begin(), results.end(),
              [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    return results;
}

void start_access_point(const config::Settings& settings) {
    String name = settings.device_name.isEmpty() ? "tablo-setup" : settings.device_name;

    // Пароль постоянный — сгенерирован один раз в config::load() и живёт в
    // NVS (docs/decisions.md, п.8): вводить новый при каждом подъёме точки
    // неудобно и ничего не даёт, пароль всё равно виден на экране любому
    // рядом. Точка всё так же не может быть открытой — запись настроек
    // разрешена только с её интерфейса (portal.cpp, authorized()), пароль на
    // входе — единственная преграда для постороннего, до устройства не
    // добиравшегося.
    //
    // Пустым сюда попасть не должно: config::load() генерирует и сохраняет
    // пароль ещё до того, как settings попадёт в этот код (был баг именно на
    // этом пути — ранний return из load() пропускал генерацию целиком).
    // Проверка здесь — защита на случай будущей регрессии или сбоя NVS:
    // WiFi.softAP() с пустой строкой поднимает ОТКРЫТУЮ сеть, а открытая
    // точка — дыра размером с дом (см. шапку docs/decisions.md, п.8, про
    // POST /api/config с чужого адреса). Отказаться и сказать в лог лучше,
    // чем молча раздавать сеть без пароля.
    if (settings.ap_password.isEmpty()) {
        Serial.println(
            "netman: пароль точки доступа пуст — точка НЕ поднята (docs/decisions.md, п.8)");
        current_ap_ssid = String();
        current_ap_password = String();
        return;
    }

    current_ap_password = settings.ap_password;
    current_ap_ssid = name;
    WiFi.softAP(name.c_str(), current_ap_password.c_str());

    Serial.printf("netman: точка доступа «%s», IP %s\n", name.c_str(),
                  WiFi.softAPIP().toString().c_str());
}

// Объявляет устройство в mDNS под его именем — страница настройки после
// этого открывается как http://<device_name>.local/ из той же сети, не
// только по IP (который меняется от сети к сети). Зовётся при каждом входе
// в станционный режим, не один раз при старте: DHCP каждой новой сети даёт
// новый IP, а mDNS-запись нужно переобъявлять тоже — сам MDNS.begin() не
// умеет "обновить IP" по требованию, только полную переинициализацию.
void announce_mdns(const config::Settings& settings) {
    String name = settings.device_name.isEmpty() ? "tablo-setup" : settings.device_name;
    // MDNS.begin() на уже поднятом стеке падает в mdns_init() с
    // ESP_ERR_INVALID_STATE до установки имени — без end() второй вход в
    // станцию (и смена имени на странице) оставлял бы старую запись.
    MDNS.end();
    if (MDNS.begin(name.c_str())) {
        Serial.printf("netman: mDNS — http://%s.local/\n", name.c_str());
    } else {
        Serial.println("netman: mDNS не поднялся — страница доступна только по IP");
    }
}

}  // namespace

std::vector<ScanResult> scan() {
    // Блокирует на время реального Wi-Fi скана (до ~10с) — вызывать только из
    // главного цикла (try_connect_best() и обработчик scan_requested в
    // loop()), никогда из задачи async_tcp. Для неё — scan_status() ниже.
    if (xSemaphoreTake(scan_op_mutex(), portMAX_DELAY) != pdTRUE) return {};
    std::vector<ScanResult> results = scan_locked();
    xSemaphoreGive(scan_op_mutex());
    return results;
}

void request_scan() {
    // Не блокирует и не сканирует сама — только просит главный цикл сделать
    // это на следующем тике loop(). Идемпотентно: пока скан уже идёт,
    // повторный вызов (например, очередной /api/scan со страницы настройки,
    // пока предыдущий ещё выполняется) ничего не переставляет в очередь.
    if (!scan_running.load(std::memory_order_relaxed)) {
        scan_requested.store(true, std::memory_order_relaxed);
    }
}

bool scan_pending() {
    return scan_requested.load(std::memory_order_relaxed) ||
           scan_running.load(std::memory_order_relaxed);
}

ScanSnapshot scan_status() {
    // cache_mutex() держится микросекунды (копирование вектора) — безопасно
    // звать откуда угодно, включая задачу async_tcp (portal.cpp, /api/scan),
    // в отличие от scan_op_mutex выше, который может быть занят до 10с.
    std::vector<ScanResult> results;
    bool stale;
    if (xSemaphoreTake(cache_mutex(), portMAX_DELAY) == pdTRUE) {
        results = cached_scan_results;
        stale = cached_scan_at_ms == 0 || millis() - cached_scan_at_ms >= kScanCacheMs;
        xSemaphoreGive(cache_mutex());
    } else {
        stale = true;
    }

    // Кеш устарел (или его ещё не было) — просим главный цикл обновить его;
    // сам этот вызов не блокируется и не ждёт результата.
    if (stale) request_scan();

    return ScanSnapshot{std::move(results), stale || scan_pending()};
}

namespace {

// Сканирует эфир и подключается к лучшей по сигналу из сохранённых сетей.
// Каждый раз заново — намеренно, см. комментарий в netman.h про баг ESPHome.
bool try_connect_best(const config::Settings& settings) {
    if (settings.networks.empty()) return false;

    std::vector<ScanResult> found = scan();

    const config::Network* best_net = nullptr;
    int32_t best_rssi = INT32_MIN;
    for (const auto& r : found) {
        for (const auto& net : settings.networks) {
            if (net.ssid != r.ssid) continue;
            if (best_net != nullptr && r.rssi <= best_rssi) continue;
            best_net = &net;
            best_rssi = r.rssi;
        }
    }
    if (best_net == nullptr) return false;

    Serial.printf("netman: пробуем «%s» (%d дБм)\n", best_net->ssid.c_str(), best_rssi);
    WiFi.begin(best_net->ssid.c_str(), best_net->password.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) {
        delay(kConnectPollMs);
    }
    return WiFi.status() == WL_CONNECTED;
}

}  // namespace

void begin(const config::Settings& settings) {
    current_settings = settings;
    current_mode = Mode::kConnecting;

    // AP_STA сразу: точку доступа поднимаем только при необходимости, но
    // режим станции должен быть доступен всё время — иначе периодические
    // попытки вернуться домой из режима настройки требовали бы переключения
    // радио и рвали бы сессию телефона со страницей настройки.
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);  // энергосбережение радио режет отзывчивость веб-страницы

    if (try_connect_best(settings)) {
        current_mode = Mode::kStation;
        Serial.printf("netman: подключено, IP %s\n", WiFi.localIP().toString().c_str());
        announce_mdns(settings);
    } else {
        start_access_point(settings);
        current_mode = Mode::kAccessPoint;
        last_retry_at = millis();
    }
}

void loop() {
    if (settings_reload_pending.exchange(false, std::memory_order_relaxed)) {
        // portal.cpp уже вызвал config::save() до reload() — NVS свежая,
        // здесь просто перечитываем её сами, в главном цикле, где
        // current_settings больше никто не трогает.
        current_settings = config::load();
        if (current_mode == Mode::kAccessPoint) {
            // Следующая попытка ниже должна пройти немедленно — только что
            // введённый на странице настройки пароль незачем откладывать на
            // минуту по расписанию kApRetryIntervalMs.
            last_retry_at = millis() - kApRetryIntervalMs;
            connect_now = true;
        }
    }

    // Скан по запросу со страницы настройки (/api/scan -> scan_status() ->
    // request_scan()) — единственное место, где действительно выполняется
    // блокирующий WiFi.scanNetworks() для этого пути: главный цикл, не
    // async_tcp. До этой правки блокировку на ~10с делала сама задача
    // веб-сервера — и падала по сторожевому таймеру (см. docs/decisions.md,
    // п.8, комментарий у scan_op_mutex/cache_mutex выше).
    if (scan_requested.exchange(false, std::memory_order_relaxed)) {
        scan_running.store(true, std::memory_order_relaxed);
        std::vector<ScanResult> results = scan();
        if (xSemaphoreTake(cache_mutex(), portMAX_DELAY) == pdTRUE) {
            cached_scan_results = std::move(results);
            cached_scan_at_ms = millis();
            xSemaphoreGive(cache_mutex());
        }
        scan_running.store(false, std::memory_order_relaxed);
    }

    if (current_mode == Mode::kAccessPoint) {
        // Пока кто-то подключён к точке — не трогаем станцию. В режиме AP_STA
        // попытка подключиться уводит радио на другой канал, и телефон, с
        // которого сейчас настраивают устройство, отваливается со страницы.
        // Раньше это было терпимо: настроить можно было и из домашней сети.
        // Теперь точка доступа — единственный путь, и сохранение, попавшее в
        // окно попытки, просто не дойдёт.
        // Исключение — сразу после сохранения настроек: владелец ввёл сеть и
        // ждёт перехода, а он подключён к точке именно потому, что настраивал.
        // Без этого исключения переход не наступает вовсе.
        if (!connect_now && WiFi.softAPgetStationNum() > 0) {
            last_retry_at = millis();  // отсчёт с ухода последнего клиента
            return;
        }

        // Принудительная точка (долгое BTN1, force_access_point()) держится
        // без ретрая заданный срок независимо от обычного расписания —
        // владелец только что попросил её явно, откатывать её раньше, чем он
        // успеет открыть страницу, было бы против его прямого намерения.
        if (millis() < forced_hold_until) return;

        if (millis() - last_retry_at < kApRetryIntervalMs) return;
        last_retry_at = millis();

        connect_now = false;

        if (try_connect_best(current_settings)) {
            WiFi.softAPdisconnect(/*wifioff=*/false);  // радио остаётся в AP_STA, гасим только вещание
            current_mode = Mode::kStation;
            Serial.printf("netman: вернулись в сеть, IP %s\n", WiFi.localIP().toString().c_str());
            announce_mdns(current_settings);
        }
        return;
    }

    if (current_mode != Mode::kStation) return;

    if (WiFi.status() == WL_CONNECTED) {
        disconnected_since_ms = 0;
        return;
    }

    // Разрыв фиксируем не по первой же неудачной проверке — короткий сбой
    // роутера (перезагрузка, доля секунды роуминга) сам восстановится, а
    // немедленный уход в точку доступа рвёт станционное соединение и потом
    // требует заново сканировать эфир и подключаться.
    if (disconnected_since_ms == 0) {
        disconnected_since_ms = millis();
        return;
    }
    if (millis() - disconnected_since_ms < kDisconnectGraceMs) return;

    Serial.println("netman: связь потеряна, переподключаемся");
    if (!try_connect_best(current_settings)) {
        start_access_point(current_settings);
        current_mode = Mode::kAccessPoint;
        last_retry_at = millis();
    }
    disconnected_since_ms = 0;
}

void force_access_point() {
    // Зовётся из главного цикла (main.cpp, buttons::poll()) — тем же путём,
    // что и loop() выше, поэтому current_settings/current_mode можно трогать
    // напрямую, без флага (в отличие от reload(), которую зовёт задача
    // async_tcp — см. её комментарий про settings_reload_pending).
    start_access_point(current_settings);
    current_mode = Mode::kAccessPoint;
    last_retry_at = millis();
    forced_hold_until = millis() + kForcedApHoldMs;
    connect_now = false;
}

void reload_now() { current_settings = config::load(); }

void reload(const config::Settings& settings) {
    // settings здесь не используется намеренно: portal.cpp вызывает
    // config::save(settings) перед reload(), так что к этому моменту NVS уже
    // содержит то же самое. Присваивать settings в current_settings отсюда
    // означало бы трогать этот вектор из задачи async_tcp, пока главный цикл
    // может перебирать его же в try_connect_best() — см. комментарий у
    // settings_reload_pending выше. Поэтому только флаг; данные подхватит
    // loop() сам.
    (void)settings;
    settings_reload_pending.store(true, std::memory_order_relaxed);
}

Mode mode() { return current_mode; }

const config::Settings& settings() { return current_settings; }

String status_text() {
    switch (current_mode) {
        case Mode::kStation:
            return WiFi.SSID();
        case Mode::kAccessPoint:
            return "точка доступа";
        default:
            return "подключение…";
    }
}

IPAddress ip() {
    return current_mode == Mode::kAccessPoint ? WiFi.softAPIP() : WiFi.localIP();
}

String ap_ssid() { return current_ap_ssid; }
String ap_password() { return current_ap_password; }

int32_t rssi() {
    // 0 — сентинел «нет сигнала» для layout::wifi_bars (см. layout.h): в
    // режиме точки доступа или до первого подключения WiFi.RSSI() значения не
    // даёт вовсе, а настоящий 0 дБм для реального сигнала физически не
    // бывает — можно не заводить отдельный bool "есть ли значение".
    return current_mode == Mode::kStation ? WiFi.RSSI() : 0;
}

}  // namespace netman
