// Опрос источников: HTTP/JSON и Home Assistant.
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
// Путь, который не нашёлся, просто не попадает в результат.
std::vector<ParsedSlot> parse_http_response(const String& json_body,
                                             const std::vector<config::SlotMapping>& map);

// Опрашивает коннекторы, у которых истёк interval с прошлого опроса. Ошибка
// одного коннектора не должна ронять остальные: поймали — пометили его слоты
// через Store::mark_failed и пошли дальше. now — unix-время, передаётся
// снаружи, чтобы расписание опроса не зависело от системных часов напрямую.
void poll_due(const std::vector<config::Connector>& list, slots::Store& store, uint32_t now);

}  // namespace connectors
