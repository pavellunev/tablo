// Опрос источников: HTTP/JSON, Home Assistant, лимиты Claude/Codex (OAuth) и
// почта (IMAP).
//
// Разбор ответа и сам HTTP-запрос разнесены по разным функциям намеренно:
// запрос требует сети и живёт только на устройстве, а разбор путей и JSON —
// чистая логика, которую нужно гонять сотнями прогонов на хосте при каждой
// правке. Смешай их в одну функцию — и тестировать разбор станет нечем, кроме
// самого устройства.
#pragma once

#include <Arduino.h>

#include <vector>

#include "config.h"
#include "slots.h"

namespace connectors {

// Таймаут одного HTTP-запроса. Без него зависший сервер держит весь цикл
// опроса, пока не откажет TCP, — а это могут быть минуты.
constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;

// Слот, разобранный из ответа: имя + значение. Времени получения (at) здесь
// нет — его проставляет вызывающий код в момент опроса, чтобы сам разбор
// оставался чистой функцией и не зависел от часов.
struct ParsedSlot {
    String id;
    slots::Slot value;
};

// Достаёт значение по пути вида "bitcoin.usd" или "items.0.price" из тела
// http-ответа: точка — вложенность объекта, число — индекс массива.
// Отсутствующий путь — не авария разбора всего ответа: возвращает false,
// `out` не помечается как ok, вызывающий код просто не получает этот слот.
bool extract_http_path(const String& json_body, const String& path, slots::Slot& out);

// Разбирает ответ Home Assistant `/api/states/{entity_id}`: значение — поле
// state, единица измерения — attributes.unit_of_measurement, добавляется к
// тексту через пробел, если она есть.
bool extract_homeassistant_state(const String& json_body, slots::Slot& out);

// Разбор одного http-ответа сразу по всей карте коннектора: одно тело ответа
// обычно кормит несколько слотов ("bitcoin.usd" и "usd.rub" из одного GET).
// Путь, который не нашёлся, просто не попадает в результат. Мэппинги с
// has_history=true (см. config.h) сюда не попадают вовсе — они не несут
// текущего значения, только историю, см. parse_http_history ниже. Дельта
// считается тут же: delta_source — путь к готовому проценту изменения (как у
// Binance) или, при delta_is_previous=true, к ПРЕДЫДУЩЕМУ значению того же
// показателя (как Valute.USD.Previous у ЦБ) — тогда дельта в процентах
// считается как (value-previous)/previous*100.
std::vector<ParsedSlot> parse_http_response(const String& json_body,
                                             const std::vector<config::SlotMapping>& map);

// Точка истории, извлечённая для одного слота: значения — как их прислал
// источник (Store::put_history ниже не переупорядочивает и не сглаживает).
struct ParsedHistory {
    String id;
    float values[slots::Slot::kHistoryCapacity];
    uint8_t count = 0;
};

// Мэппинги с has_history=true: history_source — путь до массива точек в теле
// ответа (пусто — корень ответа уже массив, как у Binance klines),
// history_item — путь ВНУТРИ каждого элемента массива (пусто — элемент сам
// скаляр). Обрезает до slots::Slot::kHistoryCapacity, лишние точки в начале
// массива отбрасываются (Store хранит последние, самые свежие).
std::vector<ParsedHistory> parse_http_history(const String& json_body,
                                              const std::vector<config::SlotMapping>& map);

// Достаёт до max_len чисел из массива в JSON: history_source — путь до
// массива (пусто — сам корень документа), history_item — путь ВНУТРИ каждого
// элемента (пусто — элемент сам скаляр). Нечисловые/отсутствующие элементы
// пропускаются, не прерывая разбор остальных. Общая для parse_http_history
// (Binance klines) и extract_ha_history_values (Home Assistant) — обе сводят
// задачу к «достать число по пути из каждого элемента массива».
uint8_t extract_history_array(const String& json_body, const String& history_source,
                              const String& history_item, float* out, uint8_t max_len);

// Прореживает in_count точек до out_count равномерно по индексу (не
// усреднение соседей) — тот же приём, что _decimate в trmnl-ink
// (renderer/app/providers/air.py): если исходных точек меньше или столько
// же, сколько нужно, просто копирует все. Возвращает фактическое число точек
// в out (<= out_count). in_count — uint16_t, не uint8_t: у Home Assistant
// частый датчик за 6 часов легко даёт больше 255 записей, урезание входа до
// 255 давало бы спарклайн из самого НАЧАЛА окна вместо всего периода
// (поймано на живом устройстве, см. Status Log).
uint8_t decimate(const float* in, uint16_t in_count, float* out, uint8_t out_count);

// Разбирает ответ Home Assistant `/api/history/period` (`minimal_response`):
// массив массивов состояний, берём значения `state` первого (единственного
// при filter_entity_id с одним entity) элемента внешнего массива, в порядке
// от старых к новым, как их прислал HA. "unavailable"/"unknown" — не число,
// такие точки пропускаются, а не обрывают разбор. max_len — uint16_t: частый
// датчик за 6 часов легко даёт больше 255 записей (см. decimate выше).
uint16_t extract_ha_history_values(const String& json_body, float* out, uint16_t max_len);

// Опрашивает коннекторы, у которых истёк interval с прошлого опроса. Ошибка
// одного коннектора не должна ронять остальные: поймали — пометили его слоты
// через Store::mark_failed и пошли дальше. now — unix-время, передаётся
// снаружи, чтобы расписание опроса не зависело от системных часов напрямую.
// Полные Settings, а не только connectors: kind="weather"/"geocode" читают
// город и координаты устройства (settings.city*) — это настройка уровня
// устройства, а не отдельного коннектора (docs/constructor.md).
void poll_due(const config::Settings& settings, slots::Store& store, uint32_t now);

// ── разбор ответов лимитов/почты — чистые функции, тестируются на хосте ──

// GET https://api.anthropic.com/api/oauth/usage. five_hour/week заполняются
// независимо: у five_hour.ok=true всегда, если разобрать вообще получилось
// (week может отсутствовать в ответе — тогда week.ok остаётся false, а не
// проваливает разбор целиком).
bool parse_claude_usage(const String& json_body, slots::Slot& five_hour, slots::Slot& week);

// GET https://chatgpt.com/backend-api/wham/usage.
bool parse_codex_usage(const String& json_body, slots::Slot& out);

// POST https://console.anthropic.com/v1/oauth/token, grant_type=refresh_token.
// previous_refresh_token — на случай, если сервер не прислал новый: ротация
// refresh_token не гарантирована документацией, и потерять единственный
// рабочий токен из-за отсутствия поля в ответе нельзя.
bool parse_oauth_refresh(const String& json_body, const String& previous_refresh_token,
                          String& access_token_out, String& refresh_token_out);

// Считает идентификаторы в ответе IMAP `SEARCH`: "* SEARCH 12 45 90" -> 3,
// "* SEARCH" без чисел -> 0. -1 — строка не похожа на ответ SEARCH вовсе,
// вызывающий код должен отличать это от «непрочитанных нет».
int count_imap_unseen(const String& line);

// Все идентификаторы из ответа `SEARCH`, в порядке как прислал сервер
// (по IMAP — по возрастанию номера, то есть самые новые письма — в конце).
// Пустой вектор — и для «непрочитанных нет», и для «это не ответ SEARCH
// вовсе»: вызывающему коду (сколько писем показать) разница не важна, в
// обоих случаях показывать нечего.
std::vector<int> parse_imap_search_ids(const String& line);

// ── список писем: разбор заголовков FETCH, чистые функции ──

// Декодирует MIME encoded-word (RFC 2047): "=?UTF-8?B?...?=" (Base64) и
// "=?UTF-8?Q?...?=" (Quoted-Printable, "_" — пробел). Строка без такого
// вхождения возвращается как есть — большинство FROM приходит уже открытым
// текстом, кодируется обычно только Subject с кириллицей.
String decode_mime_header(const String& raw);

// "Имя Фамилия <addr@example.com>" -> "Имя Фамилия" (с MIME-декодированием
// самого имени); "addr@example.com" без display-name -> "addr" (часть до
// "@" — короче, чем весь адрес, и не требует места под домен в узкой колонке
// виджета почты).
String extract_sender_name(const String& from_header);

// Разбирает блок RFC822-заголовков (несколько строк "Имя: значение",
// возможен перенос значения на следующую строку с ведущим пробелом/табом —
// RFC 5322 folding) и достаёт три поля по имени, без учёта регистра в имени
// заголовка. Поле, которого нет в блоке, остаётся пустой строкой — вызывающий
// код решает сам, годится ли письмо без него.
bool extract_mail_headers(const String& header_block, String& from_out, String& subject_out,
                          String& date_out);

// RFC822/2822 Date: "Mon, 21 Sep 2026 10:15:32 +0500" (день недели и запятая
// необязательны) -> unix-время (UTC, смещение зоны учтено).
bool parse_rfc822_date(const String& date_header, uint32_t& unix_out);

// «ЧЧ:ММ» (в часовом поясе устройства), если письмо получено сегодня;
// «Вчера» — вчера; иначе «Д.ММ» (день без ведущего нуля, месяц с ним).
String format_mail_time(uint32_t email_unix, uint32_t now, int16_t timezone_minutes);

// ── лимиты: остаток вместо использования, время сброса ──

// 100 - utilization, зажатое в [0, 100]. Слот хранит именно остаток (число и
// текст), не использование — так просил владелец: то же, что показывает
// интерфейс Anthropic («Remaining»).
float remaining_percent(float utilization_pct);

// ISO-8601 с дробными секундами и смещением зоны (или "Z"):
// "2026-09-20T23:40:00.036044+00:00" -> unix-время (UTC).
bool parse_iso8601_utc(const String& iso, uint32_t& unix_out);

// Достаёт `resets_at` (ISO-8601) для five_hour/seven_day из ответа лимитов
// Claude — отдельно от parse_claude_usage (которая уже возвращает utilization
// и не должна менять свой протестированный контракт).
bool parse_claude_reset_times(const String& json_body, String& five_hour_resets_at,
                              String& week_resets_at);

// Абсолютное время сброса окна Codex: `rate_limit.primary_window.reset_at`
// (unix-секунды), а при его отсутствии — `reset_after_seconds` (секунды от
// now, now передаётся вызывающим кодом, а не читается самой функцией).
bool parse_codex_reset(const String& json_body, uint32_t now, uint32_t& reset_unix_out);

// «сброс через 3:06 · 2 дн 22 ч» — обе половины сразу, как в эталоне:
// пятичасовое окно часами:минутами, недельное днями и часами. Часть, для
// которой has_* == false, в строку не попадает; обе false — пустая строка
// (виджет просто не рисует строку сброса, has_data() отфильтрует).
String format_claude_reset(uint32_t five_hour_reset, bool has_five, uint32_t week_reset,
                           bool has_week, uint32_t now);

// «неделя · сброс 13:01 · через 2 дн 21 ч» — время сброса часами:минутами в
// часовом поясе устройства плюс отсчёт до него днями и часами.
String format_codex_reset(uint32_t reset_unix, uint32_t now, int16_t timezone_minutes);

// ── погода: код WMO -> текст, разбор ответа геокодинга ──

// Таблица WMO 4677 (кратко, только то, что различает виджет «Сегодня»):
// 0 — ЯСНО; 1-3 — ОБЛАЧНО/ПЕР. ОБЛ.; 45/48 — ТУМАН; 51-67/80-82 — ДОЖДЬ;
// 71-77/85-86 — СНЕГ; 95-99 — ГРОЗА. Код вне таблицы -> "?".
const char* wmo_to_text(int code);

// GET geocoding-api.open-meteo.com/v1/search: `results.0.{latitude,
// longitude,name}`. false — город не найден (пустой `results`) или ответ не
// разобрать.
bool parse_geocode_response(const String& json_body, float& lat_out, float& lon_out,
                            String& name_out);

}  // namespace connectors
