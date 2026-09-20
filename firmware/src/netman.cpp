#include "netman.h"

#include <WiFi.h>
#include <esp_random.h>

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

// Кратковременный провал WiFi.status() (роутер на секунду перезагрузился,
// доля секунды роуминга) не должен сразу ронять устройство в точку доступа —
// это дороже: рвётся станционное соединение, и через минуту снова придётся
// сканировать эфир и подключаться заново. Ждём подтверждения разрыва.
constexpr uint32_t kDisconnectGraceMs = 5000;

config::Settings current_settings;
Mode current_mode = Mode::kConnecting;
uint32_t last_retry_at = 0;
uint32_t disconnected_since_ms = 0;  // 0 — сейчас подключены или ещё не фиксировали разрыв

String current_ap_ssid;
String current_ap_password;

// Алфавит пароля точки доступа (docs/decisions.md, п.8) — без символов,
// которые легко перепутать, читая мелкий растровый шрифт с панели и потом
// набирая на экранной клавиатуре телефона: 0/O, 1/l/I выброшены. Только
// заглавные — набирать их с телефона не сложнее (первый тап уже даёт
// заглавный регистр), а вариантов начертания меньше.
constexpr char kApPasswordAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr size_t kApPasswordAlphabetLen = sizeof(kApPasswordAlphabet) - 1;

// WPA2-Personal требует минимум 8 символов пароля — 10 даёт запас и хорошую
// энтропию (32^10 ≈ 2^50) при том, что от стола его всё ещё можно перепечатать
// без ошибок за один заход.
constexpr size_t kApPasswordLength = 10;

// esp_random() — источник случайности прямо здесь, а не параметром, как в
// удалённом auth.cpp: там инъекция была нужна ради чистых функций под тесты
// на хосте, а netman.cpp и так завязан на WiFi.h и на устройстве целиком —
// тестировать эту функцию на хосте всё равно нечем.
String generate_ap_password() {
    char buf[kApPasswordLength + 1];
    for (size_t i = 0; i < kApPasswordLength; ++i) {
        buf[i] = kApPasswordAlphabet[esp_random() % kApPasswordAlphabetLen];
    }
    buf[kApPasswordLength] = '\0';
    return String(buf);
}

// portal.cpp (обработчик /api/config) выполняется в задаче async_tcp, а не в
// loop(). reload() зовётся оттуда и не может напрямую трогать
// current_settings — try_connect_best() в этот момент способен перебирать
// тот же vector<Connector> в главном цикле, и одновременное
// присваивание/перебор одного std::vector<String> — use-after-free (был
// источником случайных крешей при сохранении настроек). Поэтому reload()
// только выставляет флаг, а current_settings обновляет сам главный цикл в
// начале своего тика, перечитывая уже сохранённые в NVS настройки.
std::atomic<bool> settings_reload_pending{false};

// WiFi.scanNetworks() внутри освобождает результат предыдущего скана —
// portal.cpp дёргает scan()/scan_cached() из задачи async_tcp (/api/scan),
// пока try_connect_best() сканирует из главного цикла на периодической
// попытке вернуться из точки доступа. Мьютекс сериализует доступ к сканеру.
// Ленивая инициализация — не глобальный конструктор: на некоторых сборках
// ESP32 Arduino статические объекты с side-effect могут отработать раньше,
// чем поднят FreeRTOS, а xSemaphoreCreateMutex() до старта планировщика — это
// undefined behavior.
SemaphoreHandle_t scan_mutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

std::vector<ScanResult> cached_scan_results;
uint32_t cached_scan_at_ms = 0;
// Эфир в помещении не меняется по несколько раз в секунду — 5 секунд кеша
// не заметны владельцу, а /api/scan перестаёт держать веб-сервер по 10 секунд
// на каждый опрос страницы настройки.
constexpr uint32_t kScanCacheMs = 5000;

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
    String name = settings.device_name.isEmpty() ? "inkroam-setup" : settings.device_name;

    // Пароль новый при каждом поднятии AP (docs/decisions.md, п.8): точка
    // доступа — не «пара минут настройки», а устойчивое состояние на всё
    // время без сохранённой сети в эфире, и открытой она быть не может —
    // запись настроек разрешена только с её интерфейса (portal.cpp,
    // authorized()), так что пароль на входе — единственная преграда.
    current_ap_password = generate_ap_password();
    current_ap_ssid = name;
    WiFi.softAP(name.c_str(), current_ap_password.c_str());

    Serial.printf("netman: точка доступа «%s», IP %s\n", name.c_str(),
                  WiFi.softAPIP().toString().c_str());
}

}  // namespace

std::vector<ScanResult> scan() {
    if (xSemaphoreTake(scan_mutex(), portMAX_DELAY) != pdTRUE) return {};
    std::vector<ScanResult> results = scan_locked();
    xSemaphoreGive(scan_mutex());
    return results;
}

std::vector<ScanResult> scan_cached() {
    if (xSemaphoreTake(scan_mutex(), portMAX_DELAY) != pdTRUE) return {};
    uint32_t now = millis();
    if (cached_scan_at_ms == 0 || now - cached_scan_at_ms >= kScanCacheMs) {
        cached_scan_results = scan_locked();
        cached_scan_at_ms = now;
    }
    std::vector<ScanResult> results = cached_scan_results;
    xSemaphoreGive(scan_mutex());
    return results;
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
        }
    }

    if (current_mode == Mode::kAccessPoint) {
        // Пока кто-то подключён к точке — не трогаем станцию. В режиме AP_STA
        // попытка подключиться уводит радио на другой канал, и телефон, с
        // которого сейчас настраивают устройство, отваливается со страницы.
        // Раньше это было терпимо: настроить можно было и из домашней сети.
        // Теперь точка доступа — единственный путь, и сохранение, попавшее в
        // окно попытки, просто не дойдёт.
        if (WiFi.softAPgetStationNum() > 0) {
            last_retry_at = millis();  // отсчёт с ухода последнего клиента
            return;
        }

        if (millis() - last_retry_at < kApRetryIntervalMs) return;
        last_retry_at = millis();

        if (try_connect_best(current_settings)) {
            WiFi.softAPdisconnect(/*wifioff=*/false);  // радио остаётся в AP_STA, гасим только вещание
            current_mode = Mode::kStation;
            Serial.printf("netman: вернулись в сеть, IP %s\n", WiFi.localIP().toString().c_str());
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

}  // namespace netman
