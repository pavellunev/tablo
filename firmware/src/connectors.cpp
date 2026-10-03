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
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include "connectors.h"
#include "i18n.h"

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

// ── арифметика календаря (алгоритм Howard Hinnant, public domain) ──
//
// Дублирует ровно то же по сути, что уже есть в layout::to_civil, но не
// подключает layout.h: коннекторы не должны знать про пиксели и canvas (см.
// шапку connectors.h про «ничего не знает про экран», docs/architecture.md).
// Сам алгоритм — общеизвестная арифметика календаря, а не бизнес-логика
// проекта, дублировать её дешевле, чем тянуть чужой слой ради пяти чисел.

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

// Гражданская дата -> число дней от эпохи 1970-01-01 (может быть
// отрицательным для дат до эпохи — здесь не нужно, но не роняем на них).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    int64_t era = floor_div(y >= 0 ? y : y - 399, 400);
    unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;      // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct CivilTime {
    int year, month, day, hour, minute, second;
};

// unix-время (+ смещение в минутах, 0 — UTC) -> календарь. Обратная операция
// к days_from_civil — тот же алгоритм, что civil_from_days в layout.cpp,
// продублированный по той же причине (см. комментарий выше).
CivilTime civil_from_unix(uint32_t unix_time, int32_t offset_minutes) {
    int64_t local = static_cast<int64_t>(unix_time) + offset_minutes * 60;
    int64_t z = floor_div(local, 86400);
    int64_t sec_of_day = local - z * 86400;

    z += 719468;
    int64_t era = floor_div(z, 146097);
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));
    int year = static_cast<int>(y + (month <= 2 ? 1 : 0));

    CivilTime out;
    out.year = year;
    out.month = static_cast<int>(month);
    out.day = static_cast<int>(day);
    out.hour = static_cast<int>(sec_of_day / 3600);
    out.minute = static_cast<int>((sec_of_day / 60) % 60);
    out.second = static_cast<int>(sec_of_day % 60);
    return out;
}

// Три буквы месяца без учёта регистра — RFC822 месяцы всегда "Jan".."Dec",
// но некоторые почтовые серверы шлют иной регистр.
bool month_abbrev_equal(const char* a, const char* b) {
    for (int i = 0; i < 3; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
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
        // История — отдельный путь (parse_http_history): у неё нет текущего
        // скалярного значения, а "" в history_source/history_item — не
        // «путь не найден», а «корень/элемент уже то, что нужно» — walk_path
        // истолковал бы это иначе, поэтому такие мэппинги здесь просто
        // пропускаются.
        if (m.has_history) continue;

        slots::Slot value;
        value.ttl = m.ttl;
        if (walk_path(root, std::string(m.source.c_str()), value)) {
            if (m.delta_source.length() > 0) {
                slots::Slot delta_value;
                if (walk_path(root, std::string(m.delta_source.c_str()), delta_value)) {
                    if (m.delta_is_previous) {
                        // delta_source указывает на ПРЕДЫДУЩЕЕ значение того
                        // же показателя (Valute.USD.Previous у ЦБ) — готового
                        // процента в ответе нет, считаем сами.
                        float previous = delta_value.number;
                        if (previous != 0.0f) {
                            value.delta = (value.number - previous) / previous * 100.0f;
                        }
                    } else {
                        value.delta = delta_value.number;
                    }
                }
            }
            result.push_back(ParsedSlot{m.slot, value});
        }
    }
    return result;
}

bool parse_claude_usage(const String& json_body, slots::Slot& five_hour, slots::Slot& week) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;

    // five_hour — то, что упрётся раньше всего, обязано быть в любом ответе.
    // Его отсутствие (или не-объект на этом месте) — признак того, что тело
    // не тот JSON, который мы ждём (например, HTML страницы ошибки).
    JsonVariantConst five = doc["five_hour"];
    if (!five.is<JsonObjectConst>()) return false;
    float five_pct = five["utilization"] | 0.0f;
    five_hour.number = five_pct;
    five_hour.text = format_number(five_pct);
    five_hour.ok = true;

    // seven_day может не прийти в ответе — не повод проваливать разбор
    // целиком, у five_hour он всё равно есть.
    JsonVariantConst week_v = doc["seven_day"];
    if (week_v.is<JsonObjectConst>()) {
        float week_pct = week_v["utilization"] | 0.0f;
        week.number = week_pct;
        week.text = format_number(week_pct);
        week.ok = true;
    }
    return true;
}

bool parse_codex_usage(const String& json_body, slots::Slot& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;

    JsonVariantConst window = doc["rate_limit"]["primary_window"];
    if (!window.is<JsonObjectConst>()) return false;

    float pct = window["used_percent"] | 0.0f;
    out.number = pct;
    out.text = format_number(pct);
    out.ok = true;
    return true;
}

bool parse_oauth_refresh(const String& json_body, const String& previous_refresh_token,
                          String& access_token_out, String& refresh_token_out) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;
    if (!doc["access_token"].is<const char*>()) return false;

    access_token_out = String(doc["access_token"].as<const char*>());
    // Сервер не обязан ротировать refresh_token на каждый обмен — если поля
    // нет в ответе, старый остаётся действительным. Записать сюда пустую
    // строку означало бы потерять единственный рабочий refresh_token навсегда.
    if (doc["refresh_token"].is<const char*>()) {
        refresh_token_out = String(doc["refresh_token"].as<const char*>());
    } else {
        refresh_token_out = previous_refresh_token;
    }
    return true;
}

std::vector<int> parse_imap_search_ids(const String& line) {
    std::vector<int> ids;
    std::string s(line.c_str());
    const std::string prefix = "* SEARCH";
    if (s.rfind(prefix, 0) != 0) return ids;

    std::istringstream iss(s.substr(prefix.size()));
    std::string token;
    while (iss >> token) {
        bool all_digits = !token.empty();
        for (char c : token) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) ids.push_back(std::atoi(token.c_str()));
    }
    return ids;
}

int count_imap_unseen(const String& line) {
    std::string s(line.c_str());
    if (s.rfind("* SEARCH", 0) != 0) return -1;
    return static_cast<int>(parse_imap_search_ids(line).size());
}

// ── список писем: MIME-декодирование и разбор заголовков ──

namespace {

// Base64 -> сырые байты. Молчаливо игнорирует символы вне алфавита (padding
// "=" и случайные переносы строк внутри длинного encoded-word) — почтовые
// клиенты такое иногда всё равно шлют, обрывать разбор из-за пробела не надо.
std::string base64_decode(const std::string& in) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        size_t pos = alphabet.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Quoted-Printable для encoded-word (RFC 2047): "_" — пробел (отличие от
// обычного QP в теле письма, где подчёркивание — это подчёркивание), "=XX" —
// байт по HEX.
std::string quoted_printable_decode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '_') {
            out.push_back(' ');
        } else if (in[i] == '=' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Декодирует ОДНО encoded-word "=?CHARSET?ENC?text?=" начиная с позиции pos;
// pos на выходе сдвигается за закрывающее "?=". Возвращает false, если по
// этой позиции encoded-word не начинается (вызывающий код копирует символ
// как есть).
bool decode_one_word(const std::string& s, size_t& pos, std::string& out) {
    if (s.compare(pos, 2, "=?") != 0) return false;
    size_t q1 = s.find('?', pos + 2);
    if (q1 == std::string::npos) return false;
    size_t q2 = s.find('?', q1 + 1);
    if (q2 == std::string::npos) return false;
    size_t end = s.find("?=", q2 + 1);
    if (end == std::string::npos) return false;

    std::string encoding = s.substr(q1 + 1, q2 - q1 - 1);
    std::string payload = s.substr(q2 + 1, end - q2 - 1);
    if (encoding == "B" || encoding == "b") {
        out += base64_decode(payload);
    } else if (encoding == "Q" || encoding == "q") {
        out += quoted_printable_decode(payload);
    } else {
        return false;  // незнакомая буква кодировки — не наш формат
    }
    pos = end + 2;  // за "?="
    return true;
}

}  // namespace

String decode_mime_header(const String& raw) {
    std::string s(raw.c_str());
    std::string out;
    size_t i = 0;
    bool last_was_encoded = false;
    while (i < s.size()) {
        size_t before = i;
        std::string piece;
        if (decode_one_word(s, i, piece)) {
            out += piece;
            last_was_encoded = true;
            // RFC 2047: пробел/перенос МЕЖДУ двумя encoded-word — часть
            // разметки, не часть текста, и должен исчезнуть. Пропускаем
            // ведущий пробел следующего слова, только если предыдущее тоже
            // было encoded-word — одиночный обычный пробел в обычном тексте
            // трогать нельзя.
            while (i < s.size() && s[i] == ' ') {
                size_t lookahead = i;
                while (lookahead < s.size() && s[lookahead] == ' ') ++lookahead;
                if (s.compare(lookahead, 2, "=?") == 0) {
                    i = lookahead;
                } else {
                    break;
                }
            }
            continue;
        }
        out.push_back(s[before]);
        ++i;
        last_was_encoded = false;
    }
    (void)last_was_encoded;
    return String(out.c_str());
}

String extract_sender_name(const String& from_header) {
    std::string decoded(decode_mime_header(from_header).c_str());
    size_t lt = decoded.find('<');
    std::string name;
    if (lt != std::string::npos) {
        name = decoded.substr(0, lt);
        // Обрезаем пробелы и кавычки по краям — display-name часто в кавычках.
        size_t start = name.find_first_not_of(" \t\"");
        size_t end = name.find_last_not_of(" \t\"");
        name = (start == std::string::npos) ? "" : name.substr(start, end - start + 1);
    }
    if (!name.empty()) return String(name.c_str());

    // Нет display-name — только адрес: часть до "@" короче и не тащит домен.
    size_t at = decoded.find('@');
    std::string addr = at != std::string::npos ? decoded.substr(0, at) : decoded;
    size_t start = addr.find_first_not_of(" \t<\"");
    size_t end = addr.find_last_not_of(" \t>\"");
    if (start == std::string::npos) return String("");
    return String(addr.substr(start, end - start + 1).c_str());
}

bool extract_mail_headers(const String& header_block, String& from_out, String& subject_out,
                          String& date_out) {
    std::string block(header_block.c_str());
    std::istringstream stream(block);
    std::string line;
    std::string* current = nullptr;
    std::string from, subject, date;
    bool found_any = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if ((line[0] == ' ' || line[0] == '\t') && current != nullptr) {
            // RFC 5322 folding — продолжение предыдущего заголовка.
            size_t start = line.find_first_not_of(" \t");
            if (start != std::string::npos) {
                *current += " ";
                *current += line.substr(start);
            }
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        size_t vstart = value.find_first_not_of(" \t");
        value = (vstart == std::string::npos) ? "" : value.substr(vstart);

        // Сравнение имени заголовка без учёта регистра — "From"/"from"/"FROM".
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower == "from") {
            from = value;
            current = &from;
            found_any = true;
        } else if (lower == "subject") {
            subject = value;
            current = &subject;
            found_any = true;
        } else if (lower == "date") {
            date = value;
            current = &date;
            found_any = true;
        } else {
            current = nullptr;  // чужой заголовок — его продолжение не наше дело
        }
    }

    from_out = String(from.c_str());
    subject_out = String(subject.c_str());
    date_out = String(date.c_str());
    return found_any;
}

bool parse_rfc822_date(const String& date_header, uint32_t& unix_out) {
    std::string s(date_header.c_str());
    // День недели с запятой ("Mon, ") необязателен — пропускаем его, если есть.
    size_t comma = s.find(',');
    std::string rest = comma != std::string::npos ? s.substr(comma + 1) : s;
    size_t start = rest.find_first_not_of(' ');
    if (start == std::string::npos) return false;
    rest = rest.substr(start);

    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char mon[8] = {0};
    char tz[16] = {0};
    int matched = std::sscanf(rest.c_str(), "%d %7s %d %d:%d:%d %15s", &day, mon, &year, &hour,
                              &minute, &second, tz);
    if (matched < 6) return false;

    static const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = -1;
    for (int i = 0; i < 12; ++i) {
        if (month_abbrev_equal(mon, months[i])) {
            month = i + 1;
            break;
        }
    }
    if (month < 0) return false;

    int32_t offset_seconds = 0;
    if (matched >= 7 && (tz[0] == '+' || tz[0] == '-') && std::strlen(tz) == 5) {
        int oh = (tz[1] - '0') * 10 + (tz[2] - '0');
        int om = (tz[3] - '0') * 10 + (tz[4] - '0');
        offset_seconds = (oh * 3600 + om * 60) * (tz[0] == '-' ? -1 : 1);
    }

    int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    int64_t utc = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
    if (utc < 0) return false;
    unix_out = static_cast<uint32_t>(utc);
    return true;
}

String format_mail_time(uint32_t email_unix, uint32_t now, int16_t timezone_minutes) {
    CivilTime mail_civil = civil_from_unix(email_unix, timezone_minutes);
    CivilTime now_civil = civil_from_unix(now, timezone_minutes);

    if (mail_civil.year == now_civil.year && mail_civil.month == now_civil.month &&
        mail_civil.day == now_civil.day) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", mail_civil.hour, mail_civil.minute);
        return String(buf);
    }

    int64_t mail_days = days_from_civil(mail_civil.year, mail_civil.month, mail_civil.day);
    int64_t now_days = days_from_civil(now_civil.year, now_civil.month, now_civil.day);
    if (now_days - mail_days == 1) return String(i18n::tr(i18n::Str::kYesterday));

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.%02d", mail_civil.day, mail_civil.month);
    return String(buf);
}

// ── лимиты: остаток, время сброса ──

float remaining_percent(float utilization_pct) {
    float clamped = utilization_pct;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > 100.0f) clamped = 100.0f;
    return 100.0f - clamped;
}

bool parse_iso8601_utc(const String& iso, uint32_t& unix_out) {
    std::string s(iso.c_str());
    if (s.size() < 19) return false;

    int year, month, day, hour, minute, second;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) != 6) {
        return false;
    }

    int32_t offset_seconds = 0;
    // Дата сама содержит "-" (разделители года/месяца/дня), поэтому ищем
    // признак зоны только начиная с позиции 19 ("YYYY-MM-DDTHH:MM:SS" — ровно
    // 19 символов), где "-" уже точно не может быть частью даты.
    size_t tz_pos = s.find_first_of("Z+-", 19);
    if (tz_pos != std::string::npos && s[tz_pos] != 'Z') {
        int off_h = 0, off_m = 0;
        if (std::sscanf(s.c_str() + tz_pos + 1, "%d:%d", &off_h, &off_m) >= 1) {
            offset_seconds = (off_h * 3600 + off_m * 60) * (s[tz_pos] == '-' ? -1 : 1);
        }
    }

    int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    int64_t utc = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
    if (utc < 0) return false;
    unix_out = static_cast<uint32_t>(utc);
    return true;
}

bool parse_claude_reset_times(const String& json_body, String& five_hour_resets_at,
                              String& week_resets_at) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;

    JsonVariantConst five = doc["five_hour"]["resets_at"];
    if (!five.is<const char*>()) return false;
    five_hour_resets_at = String(five.as<const char*>());

    JsonVariantConst week = doc["seven_day"]["resets_at"];
    week_resets_at = week.is<const char*>() ? String(week.as<const char*>()) : String("");
    return true;
}

bool parse_codex_reset(const String& json_body, uint32_t now, uint32_t& reset_unix_out) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;

    JsonVariantConst window = doc["rate_limit"]["primary_window"];
    if (!window.is<JsonObjectConst>()) return false;

    // reset_at — абсолютное unix-время; при его отсутствии используем
    // reset_after_seconds — относительное смещение от now (now передаёт
    // вызывающий код, а не сама функция, чтобы разбор оставался чистым).
    JsonVariantConst reset_at = window["reset_at"];
    if (reset_at.is<uint32_t>() || reset_at.is<double>()) {
        reset_unix_out = static_cast<uint32_t>(reset_at.as<double>());
        return true;
    }
    JsonVariantConst after = window["reset_after_seconds"];
    if (after.is<uint32_t>() || after.is<double>()) {
        reset_unix_out = now + static_cast<uint32_t>(after.as<double>());
        return true;
    }
    return false;
}

namespace {

String format_hm_countdown(uint32_t seconds_left) {
    uint32_t total_minutes = seconds_left / 60;
    uint32_t hours = total_minutes / 60;
    uint32_t minutes = total_minutes % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u:%02u", static_cast<unsigned>(hours),
                  static_cast<unsigned>(minutes));
    return String(buf);
}

String format_days_hours(uint32_t seconds_left) {
    uint32_t total_hours = seconds_left / 3600;
    uint32_t days = total_hours / 24;
    uint32_t hours = total_hours % 24;
    char buf[24];
    std::snprintf(buf, sizeof(buf), i18n::tr(i18n::Str::kDaysHoursFmt), static_cast<unsigned>(days),
                  static_cast<unsigned>(hours));
    return String(buf);
}

uint32_t seconds_until(uint32_t target, uint32_t now) { return target > now ? target - now : 0; }

}  // namespace

String format_claude_reset(uint32_t five_hour_reset, bool has_five, uint32_t week_reset,
                           bool has_week, uint32_t now) {
    // Каждое окно подписано своим именем: строка «сброс через 4:54 · 2 дн 7 ч»
    // не говорила, какая цифра к какому окну, и владелец не нашёл на экране
    // таймер пятичасового — хотя он там был, первым числом.
    if (!has_five && !has_week) return String("");
    std::string result;
    if (has_five) {
        result += std::string(i18n::tr(i18n::Str::kFiveHourReset)) + std::string(format_hm_countdown(seconds_until(five_hour_reset, now)).c_str());
    }
    if (has_week) {
        if (has_five) result += " · ";
        result += std::string(i18n::tr(i18n::Str::kWeekReset)) + std::string(format_days_hours(seconds_until(week_reset, now)).c_str());
    }
    return String(result.c_str());
}

String format_codex_reset(uint32_t reset_unix, uint32_t now, int16_t timezone_minutes) {
    CivilTime local = civil_from_unix(reset_unix, timezone_minutes);
    char buf[64];
    std::snprintf(buf, sizeof(buf), i18n::tr(i18n::Str::kCodexResetFmt), local.hour, local.minute,
                  format_days_hours(seconds_until(reset_unix, now)).c_str());
    return String(buf);
}

// ── погода, геокодинг ──

const char* wmo_to_text(int code) {
    // Таблица WMO 4677, кратко — только то, что различает виджет «Сегодня»
    // (docs decisions/constructor не разбивают её мельче).
    if (code == 0) return i18n::tr(i18n::Str::kWmoClear);
    if (code == 1) return i18n::tr(i18n::Str::kWmoCloudy);
    if (code == 2) return i18n::tr(i18n::Str::kWmoPartlyCloudy);
    if (code == 3) return i18n::tr(i18n::Str::kWmoCloudy);
    if (code == 45 || code == 48) return i18n::tr(i18n::Str::kWmoFog);
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return i18n::tr(i18n::Str::kWmoRain);
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return i18n::tr(i18n::Str::kWmoSnow);
    if (code >= 95 && code <= 99) return i18n::tr(i18n::Str::kWmoStorm);
    return "?";
}

bool parse_geocode_response(const String& json_body, float& lat_out, float& lon_out,
                            String& name_out) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return false;

    JsonVariantConst results = doc["results"];
    if (!results.is<JsonArrayConst>() || results.size() == 0) return false;

    JsonVariantConst first = results[0];
    JsonVariantConst lat = first["latitude"];
    JsonVariantConst lon = first["longitude"];
    if (!(lat.is<double>() || lat.is<int>()) || !(lon.is<double>() || lon.is<int>())) return false;

    lat_out = static_cast<float>(lat.as<double>());
    lon_out = static_cast<float>(lon.as<double>());
    name_out = first["name"].is<const char*>() ? String(first["name"].as<const char*>()) : String("");
    return true;
}

// ── история для спарклайна: настоящие данные источника, не накопление опроса ──

uint8_t extract_history_array(const String& json_body, const String& history_source,
                              const String& history_item, float* out, uint8_t max_len) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return 0;

    JsonVariantConst array_node = doc.as<JsonVariantConst>();
    std::string src(history_source.c_str());
    if (!src.empty()) {
        for (const std::string& seg : split_path(src)) {
            size_t idx;
            array_node = as_index(seg, idx) ? array_node[idx] : array_node[seg.c_str()];
            if (array_node.isNull()) return 0;
        }
    }

    JsonArrayConst arr = array_node.as<JsonArrayConst>();
    if (arr.isNull()) return 0;

    std::string item_path(history_item.c_str());
    uint8_t n = 0;
    for (JsonVariantConst element : arr) {
        if (n >= max_len) break;
        JsonVariantConst v = element;
        if (!item_path.empty()) {
            for (const std::string& seg : split_path(item_path)) {
                size_t idx;
                v = as_index(seg, idx) ? v[idx] : v[seg.c_str()];
                if (v.isNull()) break;
            }
        }
        slots::Slot tmp;
        if (!fill_scalar(v, tmp)) continue;  // "unavailable"/пропуск — не число, не считаем точкой
        out[n++] = tmp.number;
    }
    return n;
}

std::vector<ParsedHistory> parse_http_history(const String& json_body,
                                              const std::vector<config::SlotMapping>& map) {
    std::vector<ParsedHistory> result;
    for (const config::SlotMapping& m : map) {
        if (!m.has_history) continue;
        ParsedHistory h;
        h.id = m.slot;
        h.count = extract_history_array(json_body, m.history_source, m.history_item, h.values,
                                        slots::Slot::kHistoryCapacity);
        if (h.count > 0) result.push_back(h);
    }
    return result;
}

uint8_t decimate(const float* in, uint16_t in_count, float* out, uint8_t out_count) {
    if (out_count == 0) return 0;
    if (in_count <= out_count) {
        for (uint16_t i = 0; i < in_count; ++i) out[i] = in[i];
        return static_cast<uint8_t>(in_count);
    }
    // Последняя точка выхода — последняя точка входа: иначе свежие ~15 минут
    // истории отрезались бы (для 1245→24 индекс 23 попадал на 1193-ю запись).
    if (out_count == 1) {
        out[0] = in[in_count - 1];  // одна точка — самая свежая; делитель ниже был бы нулём
        return 1;
    }
    for (uint8_t i = 0; i < out_count; ++i) {
        size_t idx = static_cast<size_t>((static_cast<float>(i) * (in_count - 1)) / (out_count - 1));
        if (idx >= in_count) idx = in_count - 1;
        out[i] = in[idx];
    }
    return out_count;
}

uint16_t extract_ha_history_values(const String& json_body, float* out, uint16_t max_len) {
    JsonDocument doc;
    if (deserializeJson(doc, json_body.c_str())) return 0;

    JsonArrayConst outer = doc.as<JsonArrayConst>();
    if (outer.isNull() || outer.size() == 0) return 0;
    JsonArrayConst points = outer[0].as<JsonArrayConst>();
    if (points.isNull()) return 0;

    uint16_t n = 0;
    for (JsonVariantConst item : points) {
        if (n >= max_len) break;
        JsonVariantConst state = item["state"];
        if (!state.is<const char*>()) continue;
        const char* text = state.as<const char*>();
        char* end = nullptr;
        double v = std::strtod(text, &end);
        if (end == text) continue;  // "unavailable"/"unknown" — не число
        out[n++] = static_cast<float>(v);
    }
    return n;
}

namespace {

bool has_prefix(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

}  // namespace

bool provides(const config::Connector& c, const String& slot) {
    // weather пишет weather.summary мимо карты (собирает из полей ответа) —
    // считаем его владельцем всего пространства weather.*, не только map[].
    if (c.kind == "weather" && has_prefix(slot.c_str(), "weather")) return true;
    if (c.kind == "http" || c.kind == "homeassistant" || c.kind == "weather") {
        for (const config::SlotMapping& m : c.map) {
            if (m.slot == slot) return true;
            // Префикс "m.slot." — карта может описывать не сам лист, а его
            // "пространство" (например slot="weather" покрывал бы и
            // "weather.temp", и "weather.summary" одной записью), хотя
            // заводские мэппинги сейчас всегда указывают лист напрямую.
            std::string prefix = std::string(m.slot.c_str()) + ".";
            if (has_prefix(slot.c_str(), prefix.c_str())) return true;
        }
        return false;
    }
    if (c.kind == "anthropic") return has_prefix(slot.c_str(), "limit.claude");
    if (c.kind == "codex") return has_prefix(slot.c_str(), "limit.codex");
    if (c.kind == "imap") return has_prefix(slot.c_str(), "mail");
    // geocode сам не пишет ни одного слота — он готовит координаты для
    // "weather" (Settings.city_lat/lon). Виджету нужен weather.* -> нужен и
    // geocode, иначе координаты не обновятся при смене города.
    if (c.kind == "geocode") return has_prefix(slot.c_str(), "weather");
    return false;
}

bool Schedule::should_poll(const String& id, uint32_t interval, uint32_t now) const {
    const std::string key(id.c_str());
    auto held = hold_until_.find(key);
    if (held != hold_until_.end() && now < held->second) return false;
    auto it = last_polled_.find(key);
    if (it == last_polled_.end()) return true;
    return now - it->second >= interval;
}

void Schedule::mark_polled(const String& id, uint32_t now) {
    last_polled_[std::string(id.c_str())] = now;
}

void Schedule::hold(const String& id, uint32_t until) {
    hold_until_[std::string(id.c_str())] = until;
}

uint32_t Schedule::held_until(const String& id) const {
    auto it = hold_until_.find(std::string(id.c_str()));
    return it == hold_until_.end() ? 0 : it->second;
}

void Schedule::forget_missing(const std::vector<config::Connector>& configured) {
    auto sweep = [&configured](std::map<std::string, uint32_t>& m) {
        for (auto it = m.begin(); it != m.end();) {
            bool still_configured = false;
            for (const auto& c : configured) {
                if (std::string(c.id.c_str()) == it->first) { still_configured = true; break; }
            }
            it = still_configured ? std::next(it) : m.erase(it);
        }
    };
    sweep(last_polled_);
    sweep(hold_until_);
}

}  // namespace connectors

#ifdef NATIVE_BUILD

namespace connectors {

// На хосте сети нет и не нужно: расписание и HTTP здесь не тестируются,
// только разбор ответа и provides() (функции выше). Пустая реализация нужна
// лишь для линковки — вызвать её из теста нельзя, она ничего не делает;
// widgets::Demand достаточно объявления (connectors.h) — тело её не трогает.
void poll_due(const config::Settings&, slots::Store&, uint32_t, const widgets::Demand&) {}

}  // namespace connectors

#else  // !NATIVE_BUILD

#include "netman.h"          // только на устройстве: persist_* → netman::reload()
#include "widgets/demand.h"  // widgets::Demand — полное определение нужно poll_due ниже

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// Набор корневых сертификатов, зашитый в прошивку через board_build.embed_files
// (см. platformio.ini и firmware/certs/regenerate.sh). objcopy формирует имя
// символа из относительного пути файла — проверено сборкой и разбором nm по
// .pio/build/xiao-esp32s3/x509_crt_bundle.bin.txt.o, а не угадано из документации.
extern "C" const uint8_t x509_crt_bundle_start[] asm(
    "_binary_firmware_certs_x509_crt_bundle_bin_start");

#include "pinned_roots.h"

namespace connectors {

namespace {

// Расписание опроса (см. Schedule в connectors.h). Живёт в ОЗУ ровно как и
// слоты — после перезагрузки опрос просто начинается заново.
Schedule schedule_;

// http.getString() тянет всё тело в кучу разом — на ESP32 это 320 КБ RAM
// суммарно, и один источник с раздутым ответом (или чужой сервер, отданный по
// ошибке вместо API) способен съесть её целиком. Ограничиваем по
// Content-Length там, где сервер его прислал — а он присылает его в
// подавляющем большинстве случаев для JSON-ответов таких размеров.
constexpr int kMaxResponseBytes = 32 * 1024;

// Один GET с таймаутом. https определяется по схеме в адресе: у Home
// Assistant в локальной сети обычно http, у публичных API — https.
//
// insecure — явная галочка коннектора (config::Connector::insecure), а не
// решение этой функции: по умолчанию цепочка сертификата проверяется по
// встроенному бандлу, и токен уходит только серверу, прошедшему проверку
// (docs/decisions.md, п.9). Если проверка не пройдена, WiFiClientSecure не
// установит соединение вовсе — http.GET() ниже вернёт ошибку до отправки
// заголовков, значит Bearer-токен на сторону атакующего не уйдёт.
bool fetch(const String& url, const String& bearer_token, bool insecure, String& body_out,
          int max_bytes = kMaxResponseBytes) {
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
    if (code != 200) {
        // Первые байты тела при отказе — в лог. «403» само по себе не говорит,
        // кто отказал: сервер по токену или заслон Cloudflare по отпечатку
        // TLS-клиента. У второго тело узнаваемо («Just a moment», cf-ray).
        String head = http.getString();
        head = head.substring(0, 160);
        head.replace("\n", " ");
        Serial.printf("  ответ %d, тело: %s\n", code, head.c_str());
    }
    bool ok = code == HTTP_CODE_OK;
    if (ok) {
        int size = http.getSize();
        // getSize() == -1 — сервер не прислал Content-Length (chunked-ответ);
        // такое у наших источников не встречается, но на всякий случай не
        // отказываем, просто не можем отсечь заранее.
        if (size > max_bytes) {
            // Раньше это было тихим отказом: код 200, а body_out пуст без
            // единой строки в логе — «история не пришла» выглядело неотличимо
            // от битого ответа, пока не завели явную печать (поймано на
            // живом устройстве при отладке истории HA, см. Status Log).
            Serial.printf("  ответ 200, но тело %d байт > лимита %d — не читаю\n", size, max_bytes);
            ok = false;
        } else {
            body_out = http.getString();
        }
    }
    http.end();
    return ok;
}

// Адреса лимитов — константа кода, не поле настроек: в отличие от http/
// homeassistant, у этих коннекторов ровно один формат ответа, и вводить их
// на странице настройки нечем и незачем.
constexpr const char* kAnthropicUsageUrl = "https://api.anthropic.com/api/oauth/usage";
constexpr const char* kAnthropicTokenUrl = "https://console.anthropic.com/v1/oauth/token";
// Публичный идентификатор клиента Claude Code — тот же, что при обычном
// входе через `claude login`, не секрет сам по себе.
constexpr const char* kAnthropicClientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

// Обновление токена Codex — тот же OAuth refresh, что у Claude, только у
// OpenAI: эндпоинт и публичный client_id взяты из бинаря Codex CLI
// (`strings … | grep oauth/token`, `app_EMoamEEZ73f0CkXaXp7hrann` — это же
// значение стоит в поле client_id самого access-токена), scope — как в
// исходниках codex-rs (core/src/auth.rs, RefreshRequest). Без обновления
// токен Codex жил около десяти дней, потом блок гас до ручной подстановки.
constexpr const char* kCodexTokenUrl = "https://auth.openai.com/oauth/token";
constexpr const char* kCodexClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char* kCodexRefreshScope = "openid profile email";
constexpr const char* kCodexUsageUrl = "https://chatgpt.com/backend-api/wham/usage";

// Свежесть лимитов и почты — константа, не поле map: у этих коннекторов нет
// пользовательской карты слотов (см. выше про адреса), а значения без ttl не
// умеют стареть на кадре. Запас — на один пропущенный опрос сверх interval.
constexpr uint32_t kLimitTtlSeconds = 900;   // interval 300с — запас втрое
constexpr uint32_t kMailTtlSeconds = 1800;   // interval 900с — тот же запас

// Как fetch(), но возвращает код HTTP-ответа, а не bool: коннекторам лимитов
// важно отличить 401 (протухший токен, повод для refresh) от прочих ошибок,
// а fetch() выше отдаёт только успех/неуспех. Отдельная функция, а не
// расширение fetch() параметрами по умолчанию — здесь другой набор нужд:
// всегда https с проверкой сертификата (публичные API, самоподписанных
// сертификатов тут не бывает) и необязательный второй заголовок
// (anthropic-beta у Claude).
//
// pinned_ca — закреплённый корневой сертификат (pinned_roots.h) вместо
// общего бандла: у GTS Root R4 общий x509_crt_bundle.bin систематически не
// проходит проверку (тот же сбой, что уже был у CoinGecko — см. п.9
// decisions.md), а разбирать сам механизм бандла дороже, чем закрепить
// корень явно там, где он уже подтверждённо ломается. nullptr (по умолчанию)
// — прежнее поведение, общий бандл.
// retry_after_out — секунды из заголовка Retry-After при 429 (0 — не
// прислан); вызывающий код ставит паузу в Schedule ровно на них.
int fetch_status(const String& url, const String& bearer_token, const char* extra_header,
                  const char* extra_header_value, String& body_out,
                  const char* pinned_ca = nullptr, int* retry_after_out = nullptr) {
    WiFiClientSecure secure_client;
    HTTPClient http;
    if (pinned_ca != nullptr) {
        secure_client.setCACert(pinned_ca);
    } else {
        secure_client.setCACertBundle(x509_crt_bundle_start);
    }
    http.begin(secure_client, url);
    http.setConnectTimeout(REQUEST_TIMEOUT_MS);
    http.setTimeout(REQUEST_TIMEOUT_MS);
    if (bearer_token.length() > 0) {
        std::string auth = std::string("Bearer ") + bearer_token.c_str();
        http.addHeader("Authorization", auth.c_str());
    }
    if (extra_header != nullptr) {
        http.addHeader(extra_header, extra_header_value);
    }

    // Заголовки HTTPClient по умолчанию не сохраняет — просим Retry-After явно.
    const char* wanted_headers[] = {"Retry-After"};
    http.collectHeaders(wanted_headers, 1);

    int code = http.GET();
    if (retry_after_out != nullptr) {
        *retry_after_out = http.header("Retry-After").toInt();
    }
    // Тело читаем РОВНО ОДИН РАЗ. Второй getString() на уже вычитанном теле
    // ждёт закрытия соединения, а при keep-alive от Cloudflare оно не
    // закрывается — главный цикл замирал на минуты сразу после лога «ответ
    // 429» (поймано трассировкой порта с первой секунды).
    if (code > 0) {
        int size = http.getSize();
        if (size <= kMaxResponseBytes) {
            body_out = http.getString();
        }
    }
    if (code != 200 && code != 401) {
        // Первые байты тела при отказе — в лог. «403» само по себе не говорит,
        // кто отказал: у сервера на негодный токен ответ 401, а 403 — заслон
        // перед ним, и его тело узнаваемо (Cloudflare: «Just a moment», cf-ray).
        String head = body_out.substring(0, 200);
        head.replace("\n", " ");
        Serial.printf("  ответ %d, тело: %s\n", code, head.c_str());
    }
    http.end();
    return code;
}

// Один POST на обновление токена Claude — ровно один раз, без ретраев и без
// цикла: если он не удался, вызывающий код (poll_due) просто гасит слоты до
// следующего планового опроса, а не пытается снова прямо сейчас.
// Один OAuth-refresh для любого из двух источников: url/client_id/scope
// различаются, тело и разбор ответа (parse_oauth_refresh) — общие. scope
// nullptr — поле не отправляется (Anthropic его не ждёт). pinned_ca nullptr —
// общий бандл; иначе закреплённый корень (см. pinned_roots.h, п.9).
bool refresh_oauth_token(const char* token_url, const char* client_id, const char* scope,
                         const char* pinned_ca, const String& refresh_token, String& new_access,
                         String& new_refresh) {
    if (refresh_token.length() == 0) return false;

    JsonDocument req;
    req["grant_type"] = "refresh_token";
    req["refresh_token"] = refresh_token.c_str();
    req["client_id"] = client_id;
    if (scope != nullptr) req["scope"] = scope;
    size_t needed = measureJson(req) + 1;
    std::vector<char> buf(needed);
    serializeJson(req, buf.data(), needed);

    WiFiClientSecure secure_client;
    HTTPClient http;
    if (pinned_ca != nullptr) {
        secure_client.setCACert(pinned_ca);
    } else {
        secure_client.setCACertBundle(x509_crt_bundle_start);
    }
    http.begin(secure_client, token_url);
    http.setConnectTimeout(REQUEST_TIMEOUT_MS);
    http.setTimeout(REQUEST_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");

    // needed включает завершающий '\0' от serializeJson — в теле запроса он
    // не нужен, поэтому длина without него (needed - 1).
    int code = http.POST(reinterpret_cast<uint8_t*>(buf.data()), needed - 1);
    String body;
    body = http.getString();  // и при ошибке: у OAuth в теле причина — invalid_grant или rate_limit
    http.end();

    if (code != HTTP_CODE_OK) {
        // Код нужен в логе: 401 — refresh недействителен (ротирован CLI на
        // Mac), 429 — Anthropic ограничил частоту, и это лечится ожиданием,
        // а не новым токеном. Без кода оба выглядели одинаково.
        String head = body.substring(0, 200);
        head.replace("\n", " ");
        Serial.printf("  refresh: сервер ответил %d, тело: %s\n", code, head.c_str());
        return false;
    }
    return parse_oauth_refresh(body, refresh_token, new_access, new_refresh);
}

// Кладёт обновлённую пару токенов в NVS — единственный способ пережить
// перезагрузку устройства. Читает и пишет полный config::Settings (а не
// один коннектор): config::save() всегда пишет целиком, см. шапку config.h
// про «один владелец на запись». Вызывается редко (раз в ~8 часов на один
// протухший токен), коллизия с сохранением формы настройки из портала
// (тоже редкое событие) — не защищена мьютексом на саму запись в NVS, тот
// же уровень риска, что и у остальных редких обращений к Preferences в
// этом проекте.
void persist_tokens(const String& connector_id, const String& access, const String& refresh) {
    config::Settings settings = config::load();
    for (auto& conn : settings.connectors) {
        if (conn.id == connector_id) {
            conn.token = access;
            conn.refresh_token = refresh;
            break;
        }
    }
    if (config::save(settings)) {
        // Главный цикл должен увидеть новые настройки сразу, а не после
        // перезагрузки: reload() ставит флаг, перечитывание — в том же цикле.
        netman::reload(settings);
    }
}

// Обновлённые в рантайме токены Claude/Codex — коннектор, переданный в
// poll_due, это копия из netman::settings() и меняется только при следующей
// перезагрузке/сохранении формы (см. комментарий у netman::reload()).
// Без этого кеша каждый опрос после первого refresh снова получал бы 401 по
// уже устаревшему в памяти токену и снова обновлял бы его — лишний раунд
// сети на каждые 5 минут вместо одного раза в ~8 часов. persist_tokens
// выше отвечает за то, чтобы это же значение пережило перезагрузку.
std::map<std::string, String> access_override_;
std::map<std::string, String> refresh_override_;

String access_token_for(const config::Connector& c) {
    auto it = access_override_.find(std::string(c.id.c_str()));
    return it != access_override_.end() ? it->second : c.token;
}

String refresh_token_for(const config::Connector& c) {
    auto it = refresh_override_.find(std::string(c.id.c_str()));
    return it != refresh_override_.end() ? it->second : c.refresh_token;
}

// Обновление — не чаще раза в час после неудачи, на каждый коннектор своё:
// Anthropic на частые refresh отвечает 429, и каждая попытка продлевает запрет
// (упёрлись на живом после серии перепрошивок); у OpenAI мёртвый refresh
// тоже незачем дёргать каждые пять минут.
std::map<std::string, uint32_t> refresh_backoff_until_;

// Общий сценарий «получили 401»: попытка обновить пару с учётом бэкоффа.
// true — access обновлён, запрос можно повторить; false — повторять нечего,
// слоты уже погашены с причиной, вызывающий код делает continue.
bool try_refresh(const config::Connector& c, uint32_t now, const char* token_url,
                 const char* client_id, const char* scope, const char* pinned_ca, String& access,
                 slots::Store& store) {
    const std::string key(c.id.c_str());
    auto held = refresh_backoff_until_.find(key);
    if (held != refresh_backoff_until_.end() && now < held->second) {
        Serial.printf("коннектор «%s»: токен протух, обновление отложено ещё на %u мин\n",
                      c.id.c_str(), static_cast<unsigned>((held->second - now + 59) / 60));
        store.mark_failed(c.id, i18n::tr(i18n::Str::kTokenRefreshPostponed));
        return false;
    }
    const String refresh = refresh_token_for(c);
    if (refresh.length() == 0) {
        Serial.printf("коннектор «%s»: токен протух, refresh-токен не задан\n", c.id.c_str());
        store.mark_failed(c.id, i18n::tr(i18n::Str::kTokenNoRefresh));
        return false;
    }
    Serial.printf("коннектор «%s»: токен протух, обновляю через refresh_token\n", c.id.c_str());
    String new_access, new_refresh;
    if (!refresh_oauth_token(token_url, client_id, scope, pinned_ca, refresh, new_access, new_refresh)) {
        refresh_backoff_until_[key] = now + 3600;
        Serial.printf("коннектор «%s»: обновить токен не удалось, следующая попытка через час\n",
                      c.id.c_str());
        store.mark_failed(c.id, i18n::tr(i18n::Str::kTokenRefreshFailed));
        return false;
    }
    refresh_backoff_until_.erase(key);
    access = new_access;
    access_override_[key] = new_access;
    refresh_override_[key] = new_refresh;
    persist_tokens(c.id, new_access, new_refresh);
    return true;
}

// Читает одну строку ответа IMAP (до \n) с таймаутом — тот же приём, что и у
// HTTPClient::setTimeout() в fetch(): сервер, который не прислал перевод
// строки, не должен держать главный цикл дольше отведённого.
bool imap_read_line(WiFiClientSecure& client, String& line, uint32_t timeout_ms) {
    line = "";
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        while (client.available()) {
            char c = client.read();
            if (c == '\n') {
                if (line.endsWith("\r")) line.remove(line.length() - 1);
                return true;
            }
            line += c;
        }
        if (!client.connected()) return false;
        delay(5);
    }
    return false;
}

// Читает строки, пока не встретит тегированный ответ на команду (например,
// "a2 OK ..."). lines собирает всё по пути — SEARCH присылает данные отдельной
// строкой "* SEARCH ...", а тег с OK/NO только в конце подтверждает исход.
bool imap_wait_tagged(WiFiClientSecure& client, const char* tag, std::vector<String>& lines,
                      uint32_t timeout_ms) {
    uint32_t start = millis();
    String line;
    while (millis() - start < timeout_ms) {
        uint32_t elapsed = millis() - start;
        uint32_t remaining = elapsed < timeout_ms ? timeout_ms - elapsed : 0;
        if (!imap_read_line(client, line, remaining)) return false;
        lines.push_back(line);
        if (line.startsWith(tag)) {
            return line.indexOf("OK") >= 0;
        }
    }
    return false;
}

// Один разобранный конверт — то, что реально попадает на экран (mail.N.*).
struct MailSummary {
    String from;
    String subject;
    String time;
};

// Собирает FROM/SUBJECT/DATE письма с id message_id в один блок заголовков:
// FETCH присылает объявление литерала ("* N FETCH (BODY[...] {123}"), сами
// байты заголовков и завершающую ")" перед тегированным OK — здесь достаточно
// собрать всё между ними построчно (см. imap_wait_tagged): переносы внутри
// самих заголовков (RFC 5322 folding) extract_mail_headers разбирает сама, а
// точный побайтовый литерал не нужен, раз FROM/SUBJECT/DATE — обычный текст
// без интересующих нас управляющих символов.
bool imap_fetch_headers(WiFiClientSecure& client, int message_id, const char* tag,
                        String& header_block_out) {
    std::string cmd = std::string(tag) +
                       " FETCH " + std::to_string(message_id) +
                       " (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n";
    client.print(cmd.c_str());

    std::vector<String> lines;
    if (!imap_wait_tagged(client, tag, lines, REQUEST_TIMEOUT_MS)) return false;

    // Первая строка — объявление литерала, последняя (перед тегом) — ")",
    // закрывающая FETCH; заголовки — всё, что между ними.
    std::string block;
    for (size_t i = 1; i + 1 < lines.size(); ++i) {
        block += std::string(lines[i].c_str());
        block += "\n";
    }
    header_block_out = String(block.c_str());
    return true;
}

// Три команды текстового протокола (LOGIN/SELECT/SEARCH UNSEEN) поверх
// WiFiClientSecure, затем FETCH заголовков последних до 4 непрочитанных —
// тащить полноценную IMAP-библиотеку ради этого не нужно (см. требование
// задачи буквально). Соединение открывается и закрывается внутри одного
// вызова: держать его между опросами (раз в 15 минут) — тратить те же
// 40+ КБ кучи впустую.
//
// pinned_ca — см. комментарий у fetch_status(): imap.gmail.com цепляется к
// GTS Root R1, тому же семейству, что систематически не проходит проверку
// через общий бандл. recent_out — самые новые письма ПЕРВЫМИ (для mail.1.*),
// в отличие от порядка SEARCH (там самые новые — последние в списке).
// reason_out — причина отказа человеческим языком: уходит в
// Store::mark_failed и дальше на панель и на карточку источника. Без неё
// погасшая почта выглядела как «блок пропал», а не «LOGIN отвергнут»
// (2026-09-23, после двух дней работы — владелец увидел пустое место).
bool imap_fetch_mailbox(const String& host, uint16_t port, const String& user,
                        const String& password, int& unread_out,
                        std::vector<MailSummary>& recent_out, uint32_t now,
                        int16_t timezone_minutes, const char* pinned_ca, String& reason_out) {
    WiFiClientSecure client;
    if (pinned_ca != nullptr) {
        client.setCACert(pinned_ca);
    } else {
        client.setCACertBundle(x509_crt_bundle_start);
    }
    if (!client.connect(host.c_str(), port, REQUEST_TIMEOUT_MS)) {
        Serial.println("  imap: TLS-соединение не установлено (сеть, сертификат или таймаут)");
        reason_out = i18n::tr(i18n::Str::kImapNoTls);
        return false;
    }

    String line;
    // Приветствие сервера — не ответ на команду, тега ждать не нужно, но
    // прочитать обязательно: иначе оно попадёт в начало разбора LOGIN.
    imap_read_line(client, line, REQUEST_TIMEOUT_MS);

    std::vector<String> lines;
    std::string login_cmd = "a1 LOGIN \"" + std::string(user.c_str()) + "\" \"" +
                             std::string(password.c_str()) + "\"\r\n";
    client.print(login_cmd.c_str());
    if (!imap_wait_tagged(client, "a1 ", lines, REQUEST_TIMEOUT_MS)) {
        client.stop();
        Serial.println("  imap: LOGIN отвергнут (логин или пароль приложения)");
        reason_out = i18n::tr(i18n::Str::kImapLoginRejected);
        return false;
    }

    lines.clear();
    client.print("a2 SELECT INBOX\r\n");
    if (!imap_wait_tagged(client, "a2 ", lines, REQUEST_TIMEOUT_MS)) {
        client.stop();
        Serial.println("  imap: SELECT INBOX не удался");
        reason_out = i18n::tr(i18n::Str::kImapSelectFailed);
        return false;
    }

    lines.clear();
    client.print("a3 SEARCH UNSEEN\r\n");
    if (!imap_wait_tagged(client, "a3 ", lines, REQUEST_TIMEOUT_MS)) {
        client.stop();
        Serial.println("  imap: SEARCH UNSEEN не удался");
        reason_out = i18n::tr(i18n::Str::kImapSearchFailed);
        return false;
    }

    std::vector<int> unseen_ids;
    for (const String& l : lines) {
        unseen_ids = parse_imap_search_ids(l);
        if (count_imap_unseen(l) >= 0) break;  // нашли строку "* SEARCH ..."
    }

    // Последние 4 идентификатора — самые новые письма (SEARCH отдаёт их по
    // возрастанию номера, см. connectors.h у parse_imap_search_ids).
    size_t take = unseen_ids.size() < 4 ? unseen_ids.size() : 4;
    std::vector<int> to_fetch(unseen_ids.end() - static_cast<long>(take), unseen_ids.end());

    int tag_counter = 4;
    for (auto it = to_fetch.rbegin(); it != to_fetch.rend(); ++it) {
        // rbegin..rend — от самого нового к более старому, ровно порядок,
        // который ждёт mail.1.*..mail.4.* в раскладке (layout.cpp).
        char tag[8];
        std::snprintf(tag, sizeof(tag), "a%d", tag_counter++);
        String header_block;
        if (!imap_fetch_headers(client, *it, tag, header_block)) continue;

        String from_raw, subject_raw, date_raw;
        if (!extract_mail_headers(header_block, from_raw, subject_raw, date_raw)) continue;

        MailSummary summary;
        summary.from = extract_sender_name(from_raw);
        String subject = decode_mime_header(subject_raw);
        // Обрезка темы — виджет всё равно обрежет по ширине (truncate_to_width
        // в layout.cpp), но тащить в память лишние килобайты длинного письма
        // незачем: считаем кодовые точки UTF-8, чтобы не разрубить кириллицу.
        constexpr size_t kMaxSubjectCodepoints = 80;
        std::string subj_std(subject.c_str());
        size_t cp = 0, byte_pos = 0;
        while (byte_pos < subj_std.size() && cp < kMaxSubjectCodepoints) {
            unsigned char c = static_cast<unsigned char>(subj_std[byte_pos]);
            byte_pos += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            ++cp;
        }
        summary.subject = String(subj_std.substr(0, byte_pos).c_str());

        uint32_t email_unix;
        summary.time = parse_rfc822_date(date_raw, email_unix)
                           ? format_mail_time(email_unix, now, timezone_minutes)
                           : String("");
        recent_out.push_back(summary);
    }

    char logout_tag[8];
    std::snprintf(logout_tag, sizeof(logout_tag), "a%d", tag_counter);
    std::string logout_cmd = std::string(logout_tag) + " LOGOUT\r\n";
    client.print(logout_cmd.c_str());
    client.stop();

    unread_out = static_cast<int>(unseen_ids.size());
    return true;
}

// Кладёт геокодированные координаты в NVS — тем же приёмом, что
// persist_tokens: config::save() пишет полный Settings, «один
// владелец на запись» (config.h). city_resolved = city — отметка «для этого
// названия координаты уже есть», иначе геокодинг повторялся бы на каждый
// опрос до следующей перезагрузки.
void persist_city(const String& city, float lat, float lon) {
    config::Settings settings = config::load();
    settings.city = city;
    settings.city_resolved = city;
    settings.city_lat = lat;
    settings.city_lon = lon;
    if (config::save(settings)) {
        // Главный цикл должен увидеть новые настройки сразу, а не после
        // перезагрузки: reload() ставит флаг, перечитывание — в том же цикле.
        netman::reload(settings);
    }
}

void persist_timezone(int16_t minutes) {
    config::Settings settings = config::load();
    settings.timezone_minutes = minutes;
    if (config::save(settings)) {
        // Главный цикл должен увидеть новые настройки сразу, а не после
        // перезагрузки: reload() ставит флаг, перечитывание — в том же цикле.
        netman::reload(settings);
    }
}

// Геокодинг этой сессии — отдельно от settings.city_resolved (которое живёт
// в NVS и обновляется только после успешного persist_city): без своего кеша
// каждый следующий опрос видел бы через netman::settings() всё тот же
// «устаревший» Settings (см. комментарий у access_override_ выше —
// тот же класс проблемы) и заново дёргал бы геокодинг каждый interval, пока
// устройство не перезагрузят. Пустая строка — этой сессией город ещё не
// резолвился, сверяемся с тем, что принесли из NVS через settings.city_resolved.
String resolved_city_cache_;

// UTF-8-байты города — процентная кодировка для query-параметра (кириллица,
// пробелы). Свой urlencode, не printf: строка приходит от владельца через
// форму, доверять ей как готовому URL нельзя.
std::string url_encode_utf8(const String& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string in(s.c_str());
    std::string out;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

// История HA — свежий второй запрос на entity с has_history=true, за
// HISTORY_WINDOW часов, прореженный до Slot::kHistoryCapacity точек (тот же
// период и приём, что trmnl-ink/renderer/app/providers/air.py — HISTORY_WINDOW,
// _decimate). Отдельная функция, а не встраивание в цикл ниже — вызывается
// из двух мест не будет, но название и границы понятнее отдельно.
constexpr uint32_t kHaHistoryWindowSeconds = 6 * 3600;

void fetch_and_store_ha_history(const config::Connector& c, const config::SlotMapping& m,
                                uint32_t now, slots::Store& store) {
    CivilTime start_civil = civil_from_unix(now - kHaHistoryWindowSeconds, 0);
    char iso[24];
    std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ", start_civil.year,
                  start_civil.month, start_civil.day, start_civil.hour, start_civil.minute,
                  start_civil.second);

    std::string url = std::string(c.url.c_str()) + "/api/history/period/" + iso +
                       "?filter_entity_id=" + m.source.c_str() +
                       "&minimal_response&no_attributes";
    // Лимит выше общего kMaxResponseBytes (32 КБ): 6 часов истории датчика,
    // обновляющегося каждые несколько секунд, — это тысячи записей, и даже
    // minimal_response легко превышает 32 КБ (пойман на живом устройстве —
    // молчаливый отказ по размеру выглядел как «истории нет вовсе», а один
    // из двух датчиков кабинета оказался настолько частым, что 128 КБ тоже
    // не хватило — честно остаётся без графика, а не выдумывает его). Это
    // доверенный локальный сервер (Home Assistant в своей сети), не
    // публичный API, и 8 МБ PSRAM платы держат такой буфер без напряжения.
    constexpr int kHaHistoryMaxBytes = 256 * 1024;
    String body;
    if (!fetch(String(url.c_str()), c.token, c.insecure, body, kHaHistoryMaxBytes)) {
        // Диагностика: молчаливый провал здесь неотличим от «истории нет»,
        // а разница важна (сеть/размер ответа vs пустой массив у HA).
        Serial.printf("  история «%s»: запрос не удался\n", m.slot.c_str());
        return;
    }

    // Буфер на кучу (не стек — loopTask не резиновый), с запасом на реально
    // частые датчики: extract_ha_history_values сама остановится на первых
    // max_len записях, но раньше max_len был всего 128 (uint8_t) — при более
    // чем 128 точках в ответе спарклайн строился по самому НАЧАЛУ 6-часового
    // окна, а не по всему периоду (пойман на живом устройстве, см. Status
    // Log). 4096 точек с большим запасом покрывает даже секундный интервал
    // обновления за 6 часов (21600с).
    constexpr uint16_t kMaxRawPoints = 4096;
    std::vector<float> raw(kMaxRawPoints);
    uint16_t raw_n = extract_ha_history_values(body, raw.data(), kMaxRawPoints);
    if (raw_n == 0) {
        Serial.printf("  история «%s»: 0 точек в ответе (%u байт тела)\n", m.slot.c_str(),
                      static_cast<unsigned>(body.length()));
        return;
    }

    float decimated[slots::Slot::kHistoryCapacity];
    uint8_t n = decimate(raw.data(), raw_n, decimated, slots::Slot::kHistoryCapacity);
    store.put_history(m.slot, decimated, n, c.id);
    Serial.printf("  история «%s»: %u -> %u точек\n", m.slot.c_str(),
                  static_cast<unsigned>(raw_n), static_cast<unsigned>(n));
}

void poll_due(const config::Settings& settings, slots::Store& store, uint32_t now,
              const widgets::Demand& demand) {
    const std::vector<config::Connector>& list = settings.connectors;

    schedule_.forget_missing(list);

    for (const config::Connector& c : list) {
        // Коннектор без потребности не опрашивается вовсе — ни один виджет
        // ни на одном из трёх дашбордов не читает ни одного его слота
        // (widgets::compute_demand, main.cpp зовёт его перед poll_due).
        if (!demand.needs(c.id)) continue;
        // Эффективный интервал — виджет просит чаще, источник ограничивает
        // (Claude не чаще 300 с из-за TLS, почта — 900): 0 у demand означает
        // «ни один виджет не сужал», берём c.interval как раньше.
        const uint32_t requested = demand.refresh_seconds(c.id);
        const uint32_t effective_interval = requested > c.interval ? requested : c.interval;
        if (!schedule_.should_poll(c.id, effective_interval, now)) continue;
        schedule_.mark_polled(c.id, now);

        if (c.kind == "http") {
            String body;
            if (!fetch(c.url, c.token, c.insecure, body)) {
                // Отказ источника — рабочая ситуация, а не авария: слоты
                // гаснут, блок уходит с кадра, остальное живёт. Но в лог это
                // писать обязательно: без строки здесь «почему пусто на
                // экране» выясняется только разбором с кабелем.
                Serial.printf("коннектор «%s»: источник не ответил\n", c.id.c_str());
                store.mark_failed(c.id, i18n::tr(i18n::Str::kSourceNoAnswer));
                continue;
            }
            size_t taken = 0;
            for (const ParsedSlot& parsed : parse_http_response(body, c.map)) {
                slots::Slot value = parsed.value;
                value.at = now;
                store.put(parsed.id, value, c.id);
                ++taken;
            }
            // История (btc_history: klines) — тем же телом, отдельным
            // разбором (parse_http_history пропускает мэппинги без
            // has_history, обычные http-коннекторы просто получают пустой
            // список и не платят за это ничем, кроме одного пустого прохода
            // по map).
            for (const ParsedHistory& h : parse_http_history(body, c.map)) {
                store.put_history(h.id, h.values, h.count, c.id);
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
            // коннектора целиком: сенсоры с has_history шлют по ДВА запроса
            // (текущее состояние + история за 6 часов), и бюджет расширен
            // вдвое против прежнего, чтобы обоим сенсорам хватило места на
            // оба запроса при обычной домашней задержке. Мёртвый сервер всё
            // равно не удержит цикл дольше этого бюджета — оставшиеся entity
            // просто подождут следующего опроса, это не авария, а частичный
            // неуспех (см. ниже).
            constexpr uint32_t kConnectorBudgetMs = REQUEST_TIMEOUT_MS * 4;
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

                if (m.has_history) fetch_and_store_ha_history(c, m, now, store);
            }
            // Ни один entity не ответил — считаем коннектор недоступным
            // целиком. Частичный неуспех (не все entity) не гасит остальные:
            // неполученные значения просто не обновляются и стареют по ttl.
            if (succeeded == 0 && !c.map.empty()) {
                Serial.printf("коннектор «%s»: ни один датчик не ответил\n", c.id.c_str());
                store.mark_failed(c.id, i18n::tr(i18n::Str::kNoSensorAnswer));
            } else {
                Serial.printf("коннектор «%s»: получено значений %u из %u\n", c.id.c_str(),
                              static_cast<unsigned>(succeeded),
                              static_cast<unsigned>(c.map.size()));
            }
        } else if (c.kind == "anthropic") {
            // Лимиты Claude — OAuth. access_token живёт около восьми часов,
            // дальше сервер отвечает 401: обновляем его через refresh_token
            // ровно один раз и повторяем сам запрос usage тоже один раз —
            // без циклов (см. connectors.h, requirement задачи буквально;
            // Anthropic агрессивно ограничивает частый опрос 429-м).
            String access = access_token_for(c);

            String body;
            int retry_after = 0;
            int status = fetch_status(kAnthropicUsageUrl, access, "anthropic-beta",
                                       "oauth-2025-04-20", body, kGtsRootR4Pem, &retry_after);

            if (status == HTTP_CODE_UNAUTHORIZED) {
                // access_token живёт около восьми часов — обновляем пару ровно
                // один раз и повторяем сам запрос тоже один раз, без циклов.
                if (!try_refresh(c, now, kAnthropicTokenUrl, kAnthropicClientId, nullptr, nullptr,
                                 access, store)) {
                    continue;
                }
                status = fetch_status(kAnthropicUsageUrl, access, "anthropic-beta",
                                      "oauth-2025-04-20", body, kGtsRootR4Pem, &retry_after);
            }

            if (status == HTTP_CODE_OK) {
                slots::Slot five_hour, week;
                if (!parse_claude_usage(body, five_hour, week)) {
                    Serial.printf("коннектор «%s»: не удалось разобрать ответ\n", c.id.c_str());
                    store.mark_failed(c.id, i18n::tr(i18n::Str::kParseFailed));
                } else {
                    size_t taken = 0;
                    // Владелец хочет ОСТАТОК, не использование (как
                    // интерфейс Anthropic: «Remaining») — remaining_percent
                    // переводит одно в другое здесь, а не в parse_claude_usage,
                    // чтобы не ломать её протестированный контракт (usage).
                    five_hour.number = remaining_percent(five_hour.number);
                    five_hour.text = format_number(five_hour.number);
                    five_hour.at = now;
                    five_hour.ttl = kLimitTtlSeconds;
                    store.put("limit.claude.5h", five_hour, c.id);
                    ++taken;
                    if (week.ok) {
                        week.number = remaining_percent(week.number);
                        week.text = format_number(week.number);
                        week.at = now;
                        week.ttl = kLimitTtlSeconds;
                        store.put("limit.claude.week", week, c.id);
                        ++taken;
                    }

                    String five_reset_iso, week_reset_iso;
                    if (parse_claude_reset_times(body, five_reset_iso, week_reset_iso)) {
                        uint32_t five_reset_unix = 0, week_reset_unix = 0;
                        bool has_five_reset = parse_iso8601_utc(five_reset_iso, five_reset_unix);
                        bool has_week_reset =
                            week_reset_iso.length() > 0 &&
                            parse_iso8601_utc(week_reset_iso, week_reset_unix);
                        String reset_text = format_claude_reset(five_reset_unix, has_five_reset,
                                                                 week_reset_unix, has_week_reset,
                                                                 now);
                        if (reset_text.length() > 0) {
                            slots::Slot reset_slot;
                            reset_slot.text = reset_text;
                            reset_slot.ok = true;
                            reset_slot.at = now;
                            reset_slot.ttl = kLimitTtlSeconds;
                            store.put("limit.claude.reset", reset_slot, c.id);
                        }
                    }

                    Serial.printf("коннектор «%s»: получено значений %u\n", c.id.c_str(),
                                  static_cast<unsigned>(taken));
                }
            } else if (status == HTTP_CODE_TOO_MANY_REQUESTS) {
                // Пауза по Retry-After (замер на живом: ≈340 с при опросе раз в
                // 300 с — каждый опрос попадал в ещё открытое окно, и 429 не
                // кончался часами, см. Schedule::hold в connectors.h). Минимум
                // минута, плюс запас: часы устройства и сервера не совпадают
                // секунда в секунду.
                const uint32_t pause = static_cast<uint32_t>(retry_after > 60 ? retry_after : 60) + 30;
                schedule_.hold(c.id, now + pause);
                Serial.printf("коннектор «%s»: сервер просит подождать (429), пауза %u с\n",
                              c.id.c_str(), static_cast<unsigned>(pause));
                store.mark_failed(c.id, i18n::tr(i18n::Str::kRateLimited));
            } else if (status == 403) {
                // docs/decisions.md, п.8а: 403 у Anthropic — не про токен, а
                // про то, что российский адрес отрезан целиком. Показать это
                // честно, а не как «источник не ответил» — иначе владелец
                // будет искать причину в токене, как искали мы сами.
                Serial.printf("коннектор «%s»: недоступен из этой страны — нужен VPN\n",
                              c.id.c_str());
                store.mark_failed(c.id, i18n::tr(i18n::Str::kRegionBlocked));
            } else {
                Serial.printf("коннектор «%s»: источник не ответил (код %d)\n", c.id.c_str(),
                              status);
                char reason[64];
                snprintf(reason, sizeof(reason), i18n::tr(i18n::Str::kSourceNoAnswerCodeFmt), status);
                store.mark_failed(c.id, reason);
            }
        } else if (c.kind == "codex") {
            // Лимиты Codex — Bearer-токен ChatGPT; протухший обновляется через
            // refresh-токен (try_refresh ниже), как у Claude.
            String access = access_token_for(c);
            String body;
            int status = fetch_status(kCodexUsageUrl, access, nullptr, nullptr, body, kGtsRootR4Pem);

            if (status == HTTP_CODE_UNAUTHORIZED) {
                // Токен Codex живёт около десяти дней; refresh-токен из auth.json
                // Codex CLI позволяет обновлять пару так же, как у Claude.
                if (!try_refresh(c, now, kCodexTokenUrl, kCodexClientId, kCodexRefreshScope,
                                 kIsrgRootsPem, access, store)) {
                    continue;
                }
                status = fetch_status(kCodexUsageUrl, access, nullptr, nullptr, body, kGtsRootR4Pem);
            }

            if (status == HTTP_CODE_OK) {
                slots::Slot limit;
                if (!parse_codex_usage(body, limit)) {
                    Serial.printf("коннектор «%s»: не удалось разобрать ответ\n", c.id.c_str());
                    store.mark_failed(c.id, i18n::tr(i18n::Str::kParseFailed));
                } else {
                    limit.number = remaining_percent(limit.number);
                    limit.text = format_number(limit.number);
                    limit.at = now;
                    limit.ttl = kLimitTtlSeconds;
                    store.put("limit.codex", limit, c.id);

                    uint32_t reset_unix;
                    if (parse_codex_reset(body, now, reset_unix)) {
                        slots::Slot reset_slot;
                        reset_slot.text = format_codex_reset(reset_unix, now, settings.timezone_minutes);
                        reset_slot.ok = true;
                        reset_slot.at = now;
                        reset_slot.ttl = kLimitTtlSeconds;
                        store.put("limit.codex.reset", reset_slot, c.id);
                    }

                    Serial.printf("коннектор «%s»: получено значений 1\n", c.id.c_str());
                }
            } else if (status == HTTP_CODE_UNAUTHORIZED) {
                // Сюда попадаем только после удачного refresh: новый токен и
                // тот отвергнут — дело не в сроке жизни.
                Serial.printf("коннектор «%s»: новый токен тоже отвергнут (401)\n", c.id.c_str());
                store.mark_failed(c.id, i18n::tr(i18n::Str::kTokenRejected));
            } else if (status == 403) {
                // docs/decisions.md, п.8а — тот же диагноз, что у Claude выше.
                Serial.printf("коннектор «%s»: недоступен из этой страны — нужен VPN\n",
                              c.id.c_str());
                store.mark_failed(c.id, i18n::tr(i18n::Str::kRegionBlocked));
            } else {
                Serial.printf("коннектор «%s»: источник не ответил (код %d)\n", c.id.c_str(),
                              status);
                // Причина — на страницу тоже: без неё погасший блок выглядит
                // как «данных нет», и владелец ищет ошибку в токене, а не в
                // сети (так и вышло на живом 2026-09-21).
                char reason[64];
                snprintf(reason, sizeof(reason), i18n::tr(i18n::Str::kSourceNoAnswerCodeFmt), status);
                store.mark_failed(c.id, reason);
            }
        } else if (c.kind == "imap") {
            // Список писем — три команды текстового протокола плюс FETCH
            // заголовков последних непрочитанных (см. imap_fetch_mailbox
            // выше). Порт 993 — IMAP поверх TLS сразу с установки соединения
            // (не STARTTLS).
            int unread = 0;
            std::vector<MailSummary> recent;
            String reason;
            if (!imap_fetch_mailbox(c.url, 993, c.username, c.token, unread, recent, now,
                                    settings.timezone_minutes, kGtsRootR1Pem, reason)) {
                Serial.printf("коннектор «%s»: почтовый сервер не ответил\n", c.id.c_str());
                store.mark_failed(c.id, reason.length() > 0 ? reason : String(i18n::tr(i18n::Str::kMailNoAnswer)));
            } else {
                slots::Slot value;
                value.number = static_cast<float>(unread);
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", unread);
                value.text = String(buf);
                value.ok = true;
                value.at = now;
                value.ttl = kMailTtlSeconds;
                store.put("mail.unread", value, c.id);

                // Слоты за пределами свежего списка гасим: иначе прочитанное
                // письмо оставалось на кадре до истечения ttl, а потом вечно
                // как «устаревшее» — has_data() его по-прежнему считало данными.
                for (size_t i = recent.size(); i < 4; ++i) {
                    char id[24];
                    for (const char* part : {"from", "subject", "time"}) {
                        snprintf(id, sizeof(id), "mail.%u.%s", static_cast<unsigned>(i + 1), part);
                        slots::Slot gone;
                        gone.ok = false;
                        gone.error = i18n::tr(i18n::Str::kNoMessage);  // в /api/status это не отказ, а пустое место в списке
                        gone.at = now;
                        store.put(String(id), gone, c.id);
                    }
                }

                for (size_t i = 0; i < recent.size() && i < 4; ++i) {
                    char from_id[24], subj_id[24], time_id[24];
                    snprintf(from_id, sizeof(from_id), "mail.%u.from", static_cast<unsigned>(i + 1));
                    snprintf(subj_id, sizeof(subj_id), "mail.%u.subject",
                             static_cast<unsigned>(i + 1));
                    snprintf(time_id, sizeof(time_id), "mail.%u.time", static_cast<unsigned>(i + 1));

                    slots::Slot from_slot, subj_slot, time_slot;
                    from_slot.text = recent[i].from;
                    from_slot.ok = from_slot.text.length() > 0;
                    from_slot.at = now;
                    from_slot.ttl = kMailTtlSeconds;
                    store.put(String(from_id), from_slot, c.id);

                    subj_slot.text = recent[i].subject;
                    subj_slot.ok = subj_slot.text.length() > 0;
                    subj_slot.at = now;
                    subj_slot.ttl = kMailTtlSeconds;
                    store.put(String(subj_id), subj_slot, c.id);

                    time_slot.text = recent[i].time;
                    time_slot.ok = time_slot.text.length() > 0;
                    time_slot.at = now;
                    time_slot.ttl = kMailTtlSeconds;
                    store.put(String(time_id), time_slot, c.id);
                }

                Serial.printf("коннектор «%s»: получено значений %u, писем в списке %u\n",
                              c.id.c_str(), static_cast<unsigned>(1),
                              static_cast<unsigned>(recent.size()));
            }
        } else if (c.kind == "weather") {
            // Город не задан (заводской дефолт пуст) — в сеть не ходим, блок
            // «Сегодня» покажет причину.
            if (settings.city.length() == 0) {
                store.mark_failed(c.id, i18n::tr(i18n::Str::kCityNotSet));
                continue;
            }
            // Координаты ещё не определены (город не геокодирован ни разу) —
            // не авария, а нормальное состояние до первого выхода в сеть
            // (docs/constructor.md, «Тонкость про режим точки доступа»).
            if (settings.city_lat == 0.0f && settings.city_lon == 0.0f) continue;

            char url[256];
            std::snprintf(url, sizeof(url),
                          "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                          "&current=temperature_2m,weather_code"
                          "&daily=temperature_2m_min,temperature_2m_max"
                          "&forecast_days=1&timezone=auto",
                          static_cast<double>(settings.city_lat),
                          static_cast<double>(settings.city_lon));
            String body;
            if (!fetch(String(url), "", false, body)) {
                Serial.printf("коннектор «%s»: источник не ответил\n", c.id.c_str());
                store.mark_failed(c.id, i18n::tr(i18n::Str::kSourceNoAnswer));
                continue;
            }

            size_t taken = 0;
            for (const ParsedSlot& parsed : parse_http_response(body, c.map)) {
                slots::Slot value = parsed.value;
                value.at = now;
                store.put(parsed.id, value, c.id);
                ++taken;
            }

            // weather_code -> текст словом (ЯСНО/ОБЛАЧНО/...) — не покрыто
            // общим path-мэппингом (там только скаляры путём, здесь нужна
            // таблица), поэтому разбирается тут же, отдельным полем.
            JsonDocument doc;
            if (!deserializeJson(doc, body.c_str())) {
                JsonVariantConst code = doc["current"]["weather_code"];
                if (code.is<int>()) {
                    slots::Slot summary;
                    summary.text = String(wmo_to_text(code.as<int>()));
                    summary.ok = true;
                    summary.at = now;
                    summary.ttl = 3600;
                    store.put("weather.summary", summary, c.id);
                    ++taken;
                }
                JsonVariantConst offset = doc["utc_offset_seconds"];
                if (offset.is<int>()) {
                    int16_t minutes = static_cast<int16_t>(offset.as<int>() / 60);
                    if (minutes != settings.timezone_minutes) persist_timezone(minutes);
                }
            }

            Serial.printf("коннектор «%s»: получено значений %u\n", c.id.c_str(),
                          static_cast<unsigned>(taken));
        } else if (c.kind == "geocode") {
            // Молчит, пока текущий город уже геокодирован — либо этой же
            // сессией (resolved_city_cache_), либо раньше и сохранён в NVS
            // (settings.city_resolved). Без своего кеша сессии этот коннектор
            // повторял бы запрос на каждый interval до перезагрузки — та же
            // проблема, что решает access_override_ выше.
            if (settings.city.length() == 0) continue;
            const String& already_resolved =
                resolved_city_cache_.length() > 0 ? resolved_city_cache_ : settings.city_resolved;
            if (already_resolved == settings.city) continue;

            std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                               url_encode_utf8(settings.city) + "&count=1&language=ru&format=json";
            String body;
            if (!fetch(String(url.c_str()), "", false, body)) {
                Serial.printf("коннектор «geocode»: источник не ответил\n");
                continue;
            }

            float lat, lon;
            String name;
            if (!parse_geocode_response(body, lat, lon, name)) {
                Serial.printf("коннектор «geocode»: город «%s» не найден\n",
                              settings.city.c_str());
                continue;
            }

            resolved_city_cache_ = settings.city;
            persist_city(settings.city, lat, lon);
            Serial.printf("коннектор «geocode»: «%s» -> %.4f, %.4f\n", settings.city.c_str(),
                          static_cast<double>(lat), static_cast<double>(lon));
        }
        // Прочие kind (например "none") намеренно не опрашиваются.
    }
}

}  // namespace connectors

#endif  // NATIVE_BUILD
