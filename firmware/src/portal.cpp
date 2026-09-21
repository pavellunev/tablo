#include "portal.h"

#include <AsyncJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <atomic>

#include "config.h"
#include "display.h"
#include "netman.h"
#include "widgets/widget.h"

namespace portal {

namespace {

constexpr uint16_t kDnsPort = 53;

AsyncWebServer server(80);
DNSServer dns_server;
bool dns_active = false;

// handle_reboot() выполняется в задаче async_tcp, а не в главном цикле —
// ESP.restart() прямо оттуда тем же классом проблем, что и netman::reload()
// (см. комментарий у settings_reload_pending в netman.cpp): что бы ни делали
// в этот момент главный цикл или другая задача с NVS/сетью, их обрывает без
// предупреждения. Поэтому только флаг, а сам restart — из portal::loop().
std::atomic<bool> reboot_pending{false};

// Последний снимок состояния слотов. Пишет главный цикл, читает задача
// веб-сервера — отсюда мьютекс: копирование String под чужой записью даёт
// рваную строку или чтение освобождённого буфера.
String g_status_json = "{}";
SemaphoreHandle_t g_status_lock = nullptr;

void handle_status(AsyncWebServerRequest* request) {
    String copy;
    if (g_status_lock && xSemaphoreTake(g_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = g_status_json;
        xSemaphoreGive(g_status_lock);
    } else {
        copy = "{\"error\":\"busy\"}";
    }
    request->send(200, "application/json", copy);
}

// Запись настроек разрешена из любой сети, в которой устройство оказалось,
// — не только из его точки доступа (docs/decisions.md, п.8: решение
// пересмотрено владельцем, там же — что взамен). Секреты наружу всё так же не
// отдаются (to_json без include_secrets), а сохранённый секрет не наследуется
// коннектором, у которого сменился адрес (config.cpp, target_unchanged) — это
// закрывает единственную атаку, ради которой гейт вводился.

void handle_scan(AsyncWebServerRequest* request) {
    // scan_status(), не scan(): та блокирует на время реального Wi-Fi скана
    // (до ~10с) — вызванная прямо из этого обработчика (задача async_tcp, у
    // неё свой сторожевой таймер и ограниченный стек), она валила устройство
    // в перезагрузку на кнопке «Сканировать сети» на странице настройки.
    // scan_status() не блокируется сама никогда: если кеш устарел, только
    // просит главный цикл обновить его и сразу отдаёт то, что есть.
    netman::ScanSnapshot snap = netman::scan_status();

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (const auto& r : snap.networks) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = r.ssid;
        o["rssi"] = r.rssi;
        o["secure"] = r.secure;
    }
    // scanning=true — список выше может быть пустым или устаревшим, страница
    // должна спросить ещё раз через секунду-другую (см. netman::scan_status()).
    doc["scanning"] = snap.scanning;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handle_get_config(AsyncWebServerRequest* request) {
    config::Settings settings = config::load();
    request->send(200, "application/json", config::to_json(settings, /*include_secrets=*/false));
}

// Реестр виджетов для палитры на странице настройки (docs/widgets.md):
// добавить виджет — один файл widgets/w_<type>.cpp, страница подхватывает
// его сама через этот эндпоинт, portal.cpp/index.html руками не трогая.
void handle_get_widgets(AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    size_t count = 0;
    const widgets::Spec* const* specs = widgets::all(&count);
    for (size_t i = 0; i < count; ++i) {
        const widgets::Spec* spec = specs[i];
        JsonObject o = arr.add<JsonObject>();
        o["type"] = spec->type;
        o["title"] = spec->title;
        o["default_size"] = widgets::size_to_string(spec->default_size);
        o["min_width"] = spec->min_width;
        JsonArray slots = o["slots"].to<JsonArray>();
        // metric не несёт статического списка слотов (widgets::Spec::slots
        // == nullptr) — какой слот показывать, владелец выбирает при
        // добавлении виджета на дашборд, здесь отдавать нечего.
        for (const char* const* p = spec->slots; p != nullptr && *p != nullptr; ++p) {
            slots.add(*p);
        }
        o["refresh"] = spec->refresh_seconds;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handle_post_config(AsyncWebServerRequest* request, JsonVariant& json) {
    // Настройки читаем заново перед слиянием: from_json достраивает
    // присланный JSON прежними секретами там, где поле пришло пустым — без
    // свежей копии из NVS «оставить как было» означало бы «стереть».
    config::Settings settings = config::load();

    String raw;
    serializeJson(json, raw);
    if (!config::from_json(raw, settings) || !config::save(settings)) {
        request->send(400, "application/json", "{\"ok\":false}");
        return;
    }

    netman::reload(settings);
    request->send(200, "application/json", "{\"ok\":true}");
}

// JsonVariant не используется — обработчик через AsyncCallbackJsonWebHandler
// ради требования Content-Type: application/json: простой POST формой с чужой
// страницы браузер отправит без предварительного запроса, JSON — нет. Так
// открытая вкладка настройки не даёт другому сайту перезагружать устройство.
void handle_reboot(AsyncWebServerRequest* request, JsonVariant&) {
    request->send(200, "application/json", "{\"ok\":true}");
    // Сам restart — не отсюда, а из portal::loop() в главном цикле: см.
    // reboot_pending выше.
    reboot_pending.store(true, std::memory_order_relaxed);
}

void handle_not_found(AsyncWebServerRequest* request) {
    // Captive-portal детекторы iOS/Android/Windows ходят по своим адресам
    // (/hotspot-detect.html, /generate_204 и т.п.). Пока мы в режиме точки
    // доступа, отдаём страницу настройки на любой путь — вместе с DNS,
    // отвечающим на всё нашим адресом, это заставляет телефон открыть её сам.
    if (netman::mode() == netman::Mode::kAccessPoint) {
        request->send(LittleFS, "/index.html", "text/html");
        return;
    }
    request->send(404, "text/plain", "not found");
}

}  // namespace

void set_status_json(const String& json) {
    if (!g_status_lock) return;
    if (xSemaphoreTake(g_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_status_json = json;
        xSemaphoreGive(g_status_lock);
    }
}

void begin() {
    g_status_lock = xSemaphoreCreateMutex();
    // Метку раздела передаём явно: LittleFS.begin() по умолчанию ищет раздел
    // с именем «spiffs», а у нас в partitions.csv он назван «littlefs» — без
    // этого файловая система не монтируется и страница настройки не
    // отдаётся, хотя залита и лежит на месте.
    if (!LittleFS.begin(/*formatOnFail=*/true, "/littlefs", 10, "littlefs")) {
        Serial.println("portal: LittleFS не поднялась");
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/api/scan", HTTP_GET, handle_scan);
    server.on("/api/config", HTTP_GET, handle_get_config);
    server.on("/api/widgets", HTTP_GET, handle_get_widgets);
    server.on("/api/status", HTTP_GET, handle_status);

    // setMethod(HTTP_POST) явно: AsyncCallbackJsonWebHandler без него по
    // умолчанию принимает GET|POST|PUT|PATCH — GET-запрос доходил бы до
    // обработчика с пустым JsonVariant, и json["..."] | "" молча подставлял
    // бы значения по умолчанию вместо честного отказа.
    auto* config_handler = new AsyncCallbackJsonWebHandler("/api/config", handle_post_config);
    config_handler->setMethod(HTTP_POST);
    server.addHandler(config_handler);
    auto* reboot_handler = new AsyncCallbackJsonWebHandler("/api/reboot", handle_reboot);
    reboot_handler->setMethod(HTTP_POST);
    server.addHandler(reboot_handler);

    server.onNotFound(handle_not_found);

    server.begin();
}

void loop() {
    if (reboot_pending.exchange(false, std::memory_order_relaxed)) {
        ESP.restart();
    }

    bool want_dns = netman::mode() == netman::Mode::kAccessPoint;

    if (want_dns && !dns_active) {
        dns_server.start(kDnsPort, "*", netman::ip());
        dns_active = true;
    } else if (!want_dns && dns_active) {
        dns_server.stop();
        dns_active = false;
    }

    if (dns_active) dns_server.processNextRequest();
}

}  // namespace portal
