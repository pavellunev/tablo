// Общие примитивы отрисовки — перенос анонимного namespace layout.cpp,
// пиксели не меняются (см. .claude/plans/constructor.md). Используются
// несколькими виджетами: эйброу-заголовок, полосы-шкалы, спарклайн,
// инверсная плашка, бейдж состояния, обрезка текста по ширине, лимиты+воздух
// одной колонкой (widgets/w_limits.cpp, w_air.cpp, w_limits_air.cpp,
// w_markets.cpp, w_metric.cpp).
#pragma once

#include <cstdint>

#include "../canvas.h"
#include "../font.h"
#include "../layout.h"
#include "../slots.h"

namespace widgets::prims {

// r.y -> базовая линия текста эйброу / подчёркивающая линия (draw_eyebrow).
constexpr int16_t kEyebrowTextOffset = 14;
constexpr int16_t kEyebrowLineOffset = 8;

void draw_eyebrow(canvas::Canvas& c, layout::Rect r, const char* label);

// "▼0,37%" — стрелка направления плюс модуль изменения. Общая для котировок
// и показателя (widgets::metric).
String format_delta(float delta_pct);
// "▲92/ч" — та же стрелка, но единица измерения другая (воздух: изменение в
// час, не в процентах).
String format_delta_per_hour(float delta_per_hour);

// Обрезает строку по ширине в пикселях, дописывая многоточие; режет по
// кодовым точкам UTF-8, не по байтам.
String truncate_to_width(const fonts::GFXfont& font, const char* utf8, int16_t max_width);

// Сеточная штриховка «в горошек» — сегмент шкалы, заполненный не целиком.
void fill_hatched(canvas::Canvas& c, int16_t x, int16_t y, int16_t w, int16_t h);
// Полосы-шкала: сплошная заливка на полностью занятые деления, штриховка на
// дробный остаток, пустой контур на прочие.
void draw_segbar(canvas::Canvas& c, layout::Rect r, float pct, uint8_t segments);

// Ряд точек для спарклайна, заканчивающийся текущим значением слота.
uint8_t build_spark(const slots::Slot& s, float* out, uint8_t max_len);
void draw_sparkline(canvas::Canvas& c, layout::Rect r, const float* raw_values, uint8_t count,
                    bool fill_below = false);

// Инверсная плашка — белым по чёрному (Почта). Возвращает ширину плашки в
// пикселях — вызывающий код ставит следующий текст сразу за ней.
int16_t draw_inverse_label(canvas::Canvas& c, int16_t x, int16_t baseline_y, const char* label);

// Индикатор нормы: тревога — инверсная плашка, норма — рамка, свежо/тихо —
// голый текст (см. w_air.cpp).
void draw_state_tag(canvas::Canvas& c, int16_t right_x, int16_t baseline_y, const char* label,
                    bool alarm, bool quiet);

// Одна строка лимита (подпись окна + шкала + процент остатка) — общая для
// Claude 5ч/недели (widgets/w_limits.cpp).
void draw_limit_row(canvas::Canvas& c, layout::Rect area, const char* window_label,
                    const slots::Slot* s, uint32_t now, uint8_t segments);

bool co2_alarm(float v);
bool co2_quiet(float v);
bool tvoc_alarm(float v);
bool tvoc_quiet(float v);

// Один показатель воздуха (эйброу+значение+спарклайн+тег) — общая для CO₂ и
// TVOC (widgets/w_air.cpp).
void draw_air_metric(canvas::Canvas& c, layout::Rect area, const char* label, const char* unit,
                     const slots::Slot* s, uint32_t now, bool (*is_alarm)(float),
                     bool (*is_quiet)(float));

// Лимиты + воздух одной колонкой — draw_limits_and_air предшественника.
// show_limits/show_air гасят свою половину независимо от того, есть ли для
// неё данные: так widgets::limits и widgets::air переиспользуют один и тот
// же код рисования, не показывая чужую рубрику, а widgets::limits_air
// включает обе (как было до разделения на отдельные типы виджетов).
// true — у коннектора есть объяснение отказа (`<id>.status`, ok=false,
// error непустой); виджеты лимитов считают такой блок видимым.
bool has_failure_reason(const slots::Store& store, const char* status_slot);

// Причина отказа для блока: первый из status-слотов (`<коннектор>.status`,
// nullptr-terminated список) с ok=false и непустым error; nullptr — сказать
// нечего. Правило владельца: блок без данных не исчезает, а объясняет
// (2026-09-22, «как у лимитов — и для остальных блоков тоже»).
const char* failure_reason(const slots::Store& store, const char* const* status_slots);

// Блок-объяснение вместо данных: эйброу рубрики и строка причины Terminus14
// под ним, обрезанная по ширине. Один вид для всех виджетов — на однобитной
// панели разнобой заглушек читался бы как разные поломки.
void draw_reason_block(canvas::Canvas& c, layout::Rect r, const char* eyebrow, const char* reason);

void draw_limits_and_air(canvas::Canvas& c, const slots::Store& store,
                         const layout::DeviceInfo& d, layout::Rect r, bool show_limits,
                         bool show_air);

}  // namespace widgets::prims
