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

// Разрешение писать настройки проверяется по интерфейсу, с которого физически
// пришло TCP-соединение, а не по netman::mode() — глобальному состоянию, которое
// в момент прихода запроса может уже не совпадать с тем, через какую сеть он
// пришёл (см. docs/decisions.md, п.8 и разбор гонки STA/AP, из-за которой
// прежняя схема была уязвима). Запрос через точку доступа физически не может
// прийти иначе, чем предъявив пароль WPA2 на хендшейке, — сравнение адресов
// здесь просто отличает эту сеть от станционной, саму защиту даёт не оно.
bool authorized(AsyncWebServerRequest* request) {
    // Два замка, и оба нужны.
    //
    // Режим — потому что `softAPIP()` возвращает 192.168.4.1 всегда, даже
    // когда точка не поднята: netif создаётся безусловно и сконфигурирован
    // заранее. Если чужая сеть выдаст устройству ровно этот адрес, сравнение
    // ниже станет тождеством и запись откроется всей сети. Это не редкая
    // случайность: устройство ассоциируется с любой точкой, вещающей
    // сохранённое имя сети, а открытые сети гостиниц мы сохранять разрешаем —
    // значит поднять фальшивую точку и раздать нужный адрес может кто угодно.
    //
    // Интерфейс — потому что режим это глобальное состояние, которое
    // расходится с реальностью в переходные моменты. Как условие «разрешить»
    // он однажды уже дал лишний доступ; здесь он может только отказать, а
    // отказ безвреден: владелец нажмёт «Сохранить» ещё раз.
    if (netman::mode() != netman::Mode::kAccessPoint) return false;

    const IPAddress ap = WiFi.softAPIP();
    if (ap == IPAddress()) return false;

    return request->client()->localIP() == ap;
}

void send_forbidden(AsyncWebServerRequest* request) {
    request->send(403, "application/json", "{\"ok\":false,\"error\":\"forbidden\"}");
}

String scan_to_json() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    // scan_cached(), не scan(): страница настройки дёргает /api/scan заметно
    // чаще, чем меняется эфир, а сам скан блокирует Wi-Fi-стек на ~10 секунд
    // — в это время веб-сервер не отвечает вовсе ни на что (см. netman.h).
    for (const auto& r : netman::scan_cached()) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = r.ssid;
        o["rssi"] = r.rssi;
        o["secure"] = r.secure;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void handle_scan(AsyncWebServerRequest* request) {
    request->send(200, "application/json", scan_to_json());
}

void handle_get_config(AsyncWebServerRequest* request) {
    config::Settings settings = config::load();
    String raw = config::to_json(settings, /*include_secrets=*/false);

    // can_write — не часть сохранённых настроек, а факт про конкретный
    // запрос: странице нужно честно сказать, можно ли отсюда вообще слать
    // /api/config, а не подсовывать форму, запись из которой заведомо
    // получит 403 (docs/decisions.md, п.8).
    JsonDocument doc;
    deserializeJson(doc, raw);
    doc["can_write"] = authorized(request);

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handle_post_config(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!authorized(request)) {
        send_forbidden(request);
        return;
    }

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

void handle_reboot(AsyncWebServerRequest* request) {
    if (!authorized(request)) {
        send_forbidden(request);
        return;
    }

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

void begin() {
    if (!LittleFS.begin(/*formatOnFail=*/true)) {
        Serial.println("portal: LittleFS не поднялась");
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/api/scan", HTTP_GET, handle_scan);
    server.on("/api/config", HTTP_GET, handle_get_config);
    server.on("/api/reboot", HTTP_POST, handle_reboot);

    // setMethod(HTTP_POST) явно: AsyncCallbackJsonWebHandler без него по
    // умолчанию принимает GET|POST|PUT|PATCH — GET-запрос доходил бы до
    // обработчика с пустым JsonVariant, и json["..."] | "" молча подставлял
    // бы значения по умолчанию вместо честного отказа.
    auto* config_handler = new AsyncCallbackJsonWebHandler("/api/config", handle_post_config);
    config_handler->setMethod(HTTP_POST);
    server.addHandler(config_handler);

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
