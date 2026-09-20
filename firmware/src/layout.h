// Раскладка кадра: единственный слой, который знает про пиксели
// (docs/architecture.md, «Раскладка»). Берёт слоты и рисует кадр 800x480,
// заданный контрактом trmnl-ink/docs/frame-contract.md, — сюда же перенесены
// его инварианты (кегли из сетки Terminus, прочерк вместо нуля, пометка
// устаревшего, ни одного обязательного блока).
//
// Соглашение об именах слотов, которые ищет раскладка (коннекторы настраивает
// владелец через страницу настройки, эти имена — просто то, что блоки ищут в
// Store; коннектор без такой карты для блока просто не даёт ему появиться):
//
//   btc, usd_rub, eur_rub          — number: курс; delta: изменение за 24ч, %
//   limit.claude.5h                — number: занято окна 0..100
//   limit.claude.week              — number: занято окна 0..100
//   limit.claude.reset             — text: «сброс через 3:06 · 2 дн 22 ч» (опц.)
//   limit.codex                    — number: занято окна 0..100
//   limit.codex.reset              — text: «неделя · сброс 13:01 · через 2 дн 21 ч» (опц.)
//   co2 (ppm), tvoc (ppb)          — number: значение
//   mail.unread                    — number: всего непрочитанных в ящике
//   mail.N.from / .subject / .time — text, N = 1..4 (последние письма в кадре)
//   weather.temp                   — number: температура, °C
//   weather.summary                — text: краткое описание погоды
//   event.N.at / .title            — text, N = 1..3 (ближайшие события)
//
// Сетка — точное воспроизведение trmnl-ink/renderer/app/templates/cockpit.html
// (эталон: reference/cockpit-reference.png, обмеры — Status Log в
// .claude/plans/inkroam.md). Верхний ряд — ДВЕ колонки, не три: слева Рынки
// (фиксированная ширина ~296px, как flex:none в cockpit.html), справа гибкая
// колонка, где друг под другом стоят Лимиты и Воздух (сама эта колонка
// исчезает целиком, только если пропали оба). Нижний ряд — тоже две колонки,
// но наоборот: справа Сегодня фиксированной ширины (~202px), слева гибкая
// Почта. Колонка без единого видимого блока не резервирует место — сосед
// получает всю ширину ряда.
#pragma once

#include <cstdint>

#include "canvas.h"
#include "slots.h"

namespace layout {

struct Rect {
    int16_t x = 0, y = 0, w = 0, h = 0;
};

// Состояние устройства — не слот, а то, что раскладке передаёт вызывающий код
// напрямую: Wi-Fi и заряд читает main.cpp/netman, а не коннектор.
struct DeviceInfo {
    int32_t wifi_rssi = 0;        // дБм; 0 — нет сигнала/не в сети (сентинел)
    int8_t battery_pct = -1;      // -1 — неизвестно/питание от USB
    uint32_t now = 0;             // время кадра
    uint32_t next_update_at = 0;  // 0 — неизвестно
    int16_t timezone_minutes = 0;
};

// ── чистые функции: тестируются на хосте без канвы и без сети ──

// 0..4 — шкала как у телефона, dBm читателю ничего не говорит с ходу
// (trmnl-ink/docs/frame-contract.md, «Шапка»).
int8_t wifi_bars(int32_t rssi_dbm);

// Сколько сегментов шкалы лимита уместно на ширину w пикселей, не слипаясь в
// сплошную полосу: сегмент уже ~10px, зазор между ними (3px, см. draw_segbar
// в layout.cpp) виден на глаз. Диапазон 4..20.
uint8_t segments_for_width(int16_t w);

// Число для экрана: десятичный разделитель запятой, разряды целой части
// разбиты пробелом начиная с пяти цифр — как в эталоне («80 689», но «5496»).
String format_decimal(float value, int decimals);

struct Civil {
    int weekday = 0;  // 0=Пн .. 6=Вс
    int day = 1;
    int month = 1;  // 1..12
    int hour = 0;
    int minute = 0;
};

// Unix-время + смещение в минутах -> календарь. Правильно только сама
// арифметика; синхронизация часов (NTP) в проект пока не встроена — см.
// Status Log в .claude/plans/inkroam.md, что именно это значит на устройстве.
Civil to_civil(uint32_t unix_time, int16_t timezone_minutes);

// nullptr или Slot::empty() — трактуются одинаково: показывать нечего.
bool has_data(const slots::Slot* s);

// Общее форматирование значения: «—» на пустом слоте (ноль читается как
// «свободно», это уже вводило в заблуждение — docs/decisions.md, п.6), «≈»
// перед устаревшим (свежесть видна всегда), иначе просто число.
String format_percent(const slots::Slot* s, uint32_t now);
String format_value(const slots::Slot* s, uint32_t now, int decimals, const char* suffix);

// Видимость блоков верхнего и нижнего ряда — по наличию хотя бы одного из их
// слотов. Источник отвалился целиком — блок не резервирует место соседям.
bool rates_visible(const slots::Store& store);
bool limits_visible(const slots::Store& store);
bool air_visible(const slots::Store& store);
bool mail_visible(const slots::Store& store);
bool today_visible(const slots::Store& store);

// Раскладывает `total` мест в ряд `row` шириной row.w с зазором gap. Место
// резервируют только видимые (visible[i] == true) — невидимые получают
// нулевой Rect и не оставляют дыры для соседей. Равные доли — там, где в
// cockpit.html оба flex:1 (сейчас нигде в верхнем/нижнем ряду, но пригодится
// для колонок ВНУТРИ блока — например, CO2|TVOC).
void compute_columns(Rect row, int16_t gap, const bool* visible, uint8_t total, Rect* out);

// Две колонки, ПЕРВАЯ фиксированной ширины, вторая — всё, что осталось (как
// Рынки flex:none 296px / остальное flex:1 в верхнем ряду cockpit.html).
// Видима только одна — она получает всю ширину ряда, у второй — нулевой Rect.
void compute_columns_fixed_first(Rect row, int16_t gap, bool first_visible, bool second_visible,
                                  int16_t first_width, Rect* first, Rect* second);

// Зеркально: фиксированной ширины ВТОРАЯ колонка (как Сегодня flex:none
// ~202px / Почта flex:1 в нижнем ряду cockpit.html).
void compute_columns_fixed_second(Rect row, int16_t gap, bool first_visible, bool second_visible,
                                   int16_t second_width, Rect* first, Rect* second);

// Делит тело кадра на верхний и нижний ряд. Ряда, у которого нет ни одного
// видимого блока, попросту нет — второй ряд получает всё тело целиком, а не
// половину с пустой второй половиной экрана.
void compute_two_rows(Rect body, int16_t gap, bool row1_has, bool row2_has, Rect* row1,
                      Rect* row2);

// ── рисование: единственная часть, которой нужна канва ──

// Кадр целиком: шапка + пересобранная сетка. Рисуется всегда, даже когда
// store пуст, — контракт «пустой снапшот должен отрендериться без падения»
// (trmnl-ink/docs/frame-contract.md, инвариант 1).
void draw_frame(canvas::Canvas& canvas, const slots::Store& store, const DeviceInfo& device);

}  // namespace layout
