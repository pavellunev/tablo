// Разбор ответов — чистые функции ниже компилируются и на устройстве, и на
// хосте. Сетевая часть (poll_due вне NATIVE_BUILD) — только на устройстве:
// HTTPClient и WiFiClientSecure на хосте не существуют, а тестировать их всё
// равно нечем без настоящей сети.
//
// ARDUINOJSON_ENABLE_ARDUINO_STRING выключен явно: иначе ArduinoJson сам
// решает, включать ли работу с Arduino String, по наличию макроса ARDUINO —
// а он есть на устройстве и отсутствует на хосте. Оставь это самотёком, и
// разбор JSON шёл бы по чуть разным путям на двух платформах, и тест на
// хосте перестал бы что-либо гарантировать про устройство.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "connectors.h"

namespace connectors {

namespace {

// "45000.00" -> "45000", "23.40" -> "23.4": число как текст должно выглядеть
// так, как его напечатал бы источник, а не как %.2f по умолчанию.
String format_number(double value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return String(s.c_str());
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(path.substr(start));
            break;
        }
        parts.push_back(path.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

// Сегмент пути — индекс массива, если состоит целиком из цифр. "0" и "12" —
// индексы, "usd" — ключ объекта.
bool as_index(const std::string& segment, size_t& out_index) {
    if (segment.empty()) return false;
    for (char c : segment) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    out_index = static_cast<size_t>(std::strtoul(segment.c_str(), nullptr, 10));
    return true;
}

// Скалярное значение (не объект, не массив, не null) кладём в Slot. Составное
// значение в конце пути — не то, что можно нарисовать одним слотом, поэтому
// тоже считается «не найдено».
bool fill_scalar(JsonVariantConst v, slots::Slot& out) {
    if (v.isNull() || v.is<JsonObjectConst>() || v.is<JsonArrayConst>()) return false;

    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        out.text = String(s);
        out.number = static_cast<float>(std::atof(s));
    } else if (v.is<bool>()) {
        bool b = v.as<bool>();
        out.text = String(b ? "true" : "false");
        out.number = b ? 1.0f : 0.0f;
    } else {
        double d = v.as<double>();
        out.number = static_cast<float>(d);
        out.text = format_number(d);
    }
    out.ok = true;
    return true;
}

// Проход по уже распарсенному дереву — вынесен отдельно от extract_http_path,
// чтобы parse_http_response мог разобрать тело JSON один раз и пройти его по
// всей карте, а не заново на каждый маппинг (см. комментарий там).
bool walk_path(JsonVariantConst root, const std::string& path, slots::Slot& out) {
    JsonVariantConst cur = root;
    for (const std::string& segment : split_path(path)) {
        size_t index;
        // Обращение по несуществующему ключу/индексу или к скаляру, как к
        // контейнеру, у ArduinoJson не падает — возвращает null-вариант,
        // ровно то, что нужно для «путь не нашёлся, а не авария».
        JsonVariantConst next = as_index(segment, index) ? cur[index] : cur[segment.c_str()];
        if (next.isNull()) return false;
        cur = next;
    }
    return fill_scalar(cur, out);
}

}  // namespace

bool extract_http_path(const String& json_body, const String& path, slots::Slot& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;
    return walk_path(doc.as<JsonVariantConst>(), std::string(path.c_str()), out);
}

bool extract_homeassistant_state(const String& json_body, slots::Slot& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;

    JsonVariantConst state = doc["state"];
    if (!fill_scalar(state, out)) return false;

    JsonVariantConst unit = doc["attributes"]["unit_of_measurement"];
    if (unit.is<const char*>()) {
        std::string combined = std::string(out.text.c_str()) + " " + unit.as<const char*>();
        out.text = String(combined.c_str());
    }
    return true;
}

std::vector<ParsedSlot> parse_http_response(const String& json_body,
                                             const std::vector<config::SlotMapping>& map) {
    std::vector<ParsedSlot> result;

    // Тело разбирается один раз на весь коннектор, а не на каждый маппинг:
    // шесть слотов из одного JSON — не шесть deserializeJson (см. заявленное
    // в connectors.h поведение, которое здесь раньше не соблюдалось).
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return result;
    JsonVariantConst root = doc.as<JsonVariantConst>();

    // Пустая карта — ответ сам является словарём слотов: имя → объект с
    // полями text/number/delta/age/ttl. Так отдаёт домашнее приложение, уже
    // сходившее во все источники. Выписывать для него карту из десятка
    // одинаковых строк незачем, а число и изменение так доезжают целиком —
    // через путь к тексту приехала бы только подпись, и шкалы с графиками
    // остались бы пустыми.
    if (map.empty()) {
        JsonObjectConst obj = root.as<JsonObjectConst>();
        if (obj.isNull()) return result;
        for (JsonPairConst kv : obj) {
            JsonVariantConst v = kv.value();
            if (!v.is<JsonObjectConst>()) continue;

            slots::Slot value;
            value.text = String(v["text"] | "");
            value.number = v["number"] | 0.0f;
            value.delta = v["delta"] | 0.0f;
            value.ttl = v["ttl"] | 900;
            value.ok = value.text.length() > 0;
            if (value.ok) result.push_back(ParsedSlot{String(kv.key().c_str()), value});
        }
        return result;
    }

    for (const config::SlotMapping& m : map) {
        slots::Slot value;
        value.ttl = m.ttl;
        if (walk_path(root, std::string(m.source.c_str()), value)) {
            result.push_back(ParsedSlot{m.slot, value});
        }
    }
    return result;
}

}  // namespace connectors

#ifdef NATIVE_BUILD

namespace connectors {

// На хосте сети нет и не нужно: расписание и HTTP здесь не тестируются,
// только разбор ответа (функции выше). Пустая реализация нужна лишь для
// линковки — вызвать её из теста нельзя, она ничего не делает.
void poll_due(const std::vector<config::Connector>&, slots::Store&, uint32_t) {}

}  // namespace connectors

#else  // !NATIVE_BUILD

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// Набор корневых сертификатов, зашитый в прошивку через board_build.embed_files
// (см. platformio.ini и firmware/certs/regenerate.sh). objcopy формирует имя
// символа из относительного пути файла — проверено сборкой и разбором nm по
// .pio/build/xiao-esp32s3/x509_crt_bundle.bin.txt.o, а не угадано из документации.
extern "C" const uint8_t x509_crt_bundle_start[] asm(
    "_binary_firmware_certs_x509_crt_bundle_bin_start");

namespace connectors {

namespace {

// коннектор → unix-время последнего опроса. Живёт в ОЗУ ровно как и слоты —
// после перезагрузки опрос просто начинается заново.
std::map<std::string, uint32_t> last_polled_;

// http.getString() тянет всё тело в кучу разом — на ESP32 это 320 КБ RAM
// суммарно, и один источник с раздутым ответом (или чужой сервер, отданный по
// ошибке вместо API) способен съесть её целиком. Ограничиваем по
// Content-Length там, где сервер его прислал — а он присылает его в
// подавляющем большинстве случаев для JSON-ответов таких размеров.
constexpr int kMaxResponseBytes = 32 * 1024;

bool should_poll(const config::Connector& c, uint32_t now) {
    auto it = last_polled_.find(std::string(c.id.c_str()));
    if (it == last_polled_.end()) return true;
    return now - it->second >= c.interval;
}

void mark_polled(const config::Connector& c, uint32_t now) {
    last_polled_[std::string(c.id.c_str())] = now;
}

// Один GET с таймаутом. https определяется по схеме в адресе: у Home
// Assistant в локальной сети обычно http, у публичных API — https.
//
// insecure — явная галочка коннектора (config::Connector::insecure), а не
// решение этой функции: по умолчанию цепочка сертификата проверяется по
// встроенному бандлу, и токен уходит только серверу, прошедшему проверку
// (docs/decisions.md, п.9). Если проверка не пройдена, WiFiClientSecure не
// установит соединение вовсе — http.GET() ниже вернёт ошибку до отправки
// заголовков, значит Bearer-токен на сторону атакующего не уйдёт.
bool fetch(const String& url, const String& bearer_token, bool insecure, String& body_out) {
    WiFiClientSecure secure_client;
    WiFiClient plain_client;
    HTTPClient http;

    bool is_https = std::string(url.c_str()).rfind("https://", 0) == 0;
    if (is_https) {
        if (insecure) {
            // Осознанное исключение из проверки — обычно самоподписанный
            // сертификат домашнего Home Assistant. Владелец согласился на
            // это явной галочкой в настройках, а не по умолчанию.
            secure_client.setInsecure();
        } else {
            secure_client.setCACertBundle(x509_crt_bundle_start);
        }
        http.begin(secure_client, url);
    } else {
        http.begin(plain_client, url);
    }

    http.setConnectTimeout(REQUEST_TIMEOUT_MS);
    http.setTimeout(REQUEST_TIMEOUT_MS);
    if (bearer_token.length() > 0) {
        std::string auth = std::string("Bearer ") + bearer_token.c_str();
        http.addHeader("Authorization", auth.c_str());
    }

    int code = http.GET();
    bool ok = code == HTTP_CODE_OK;
    if (ok) {
        int size = http.getSize();
        // getSize() == -1 — сервер не прислал Content-Length (chunked-ответ);
        // такое у наших источников не встречается, но на всякий случай не
        // отказываем, просто не можем отсечь заранее.
        if (size > kMaxResponseBytes) {
            ok = false;
        } else {
            body_out = http.getString();
        }
    }
    http.end();
    return ok;
}

}  // namespace

void poll_due(const std::vector<config::Connector>& list, slots::Store& store, uint32_t now) {
    // Удалённый в форме коннектор не должен висеть в last_polled_ вечно —
    // немного, но зачем копить мусор на объекте, который живёт всё время
    // работы устройства.
    for (auto it = last_polled_.begin(); it != last_polled_.end();) {
        bool still_configured = false;
        for (const auto& c : list) {
            if (std::string(c.id.c_str()) == it->first) {
                still_configured = true;
                break;
            }
        }
        if (still_configured) {
            ++it;
        } else {
            it = last_polled_.erase(it);
        }
    }

    for (const config::Connector& c : list) {
        if (!should_poll(c, now)) continue;
        mark_polled(c, now);

        if (c.kind == "http") {
            String body;
            if (!fetch(c.url, c.token, c.insecure, body)) {
                // Отказ источника — рабочая ситуация, а не авария: слоты
                // гаснут, блок уходит с кадра, остальное живёт. Но в лог это
                // писать обязательно: без строки здесь «почему пусто на
                // экране» выясняется только разбором с кабелем.
                Serial.printf("коннектор «%s»: источник не ответил\n", c.id.c_str());
                store.mark_failed(c.id);
                continue;
            }
            size_t taken = 0;
            for (const ParsedSlot& parsed : parse_http_response(body, c.map)) {
                slots::Slot value = parsed.value;
                value.at = now;
                store.put(parsed.id, value, c.id);
                ++taken;
            }
            Serial.printf("коннектор «%s»: получено значений %u\n", c.id.c_str(),
                          static_cast<unsigned>(taken));
        } else if (c.kind == "homeassistant") {
            // Один GET на entity: у Home Assistant REST нет способа запросить
            // несколько состояний одним запросом без шаблонов Jinja, а
            // заводить их ради этого — лишняя сложность на устройстве.
            //
            // Таймаут есть у каждого запроса (REQUEST_TIMEOUT_MS), но не у
            // коннектора целиком: 10 entity на мёртвом сервере держали бы
            // главный цикл почти минуту. Бюджет ограничивает это парой
            // таймаутов — оставшиеся entity просто подождут следующего
            // опроса, это не авария, а частичный неуспех (см. ниже).
            constexpr uint32_t kConnectorBudgetMs = REQUEST_TIMEOUT_MS * 2;
            uint32_t connector_started = millis();
            size_t succeeded = 0;
            for (const config::SlotMapping& m : c.map) {
                if (millis() - connector_started > kConnectorBudgetMs) break;
                std::string url = std::string(c.url.c_str()) + "/api/states/" + m.source.c_str();
                String body;
                if (!fetch(String(url.c_str()), c.token, c.insecure, body)) continue;

                slots::Slot value;
                value.ttl = m.ttl;
                if (!extract_homeassistant_state(body, value)) continue;

                value.at = now;
                store.put(m.slot, value, c.id);
                ++succeeded;
            }
            // Ни один entity не ответил — считаем коннектор недоступным
            // целиком. Частичный неуспех (не все entity) не гасит остальные:
            // неполученные значения просто не обновляются и стареют по ttl.
            if (succeeded == 0 && !c.map.empty()) store.mark_failed(c.id);
        }
        // Прочие kind (например "none") намеренно не опрашиваются.
    }
}

}  // namespace connectors

#endif  // NATIVE_BUILD
