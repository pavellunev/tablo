// Раскладка кадра: единственный слой, который знает про пиксели
// (docs/architecture.md, «Раскладка»). Берёт слоты и рисует кадр 800x480,
// заданный контрактом trmnl-ink/docs/frame-contract.md, — сюда же перенесены
// его инварианты (кегли из сетки Terminus, прочерк вместо нуля, пометка
// устаревшего, ни одного обязательного блока).
//
// Соглашение об именах слотов, которые ищут виджеты (widgets/w_*.cpp;
// коннекторы настраивает владелец через страницу настройки, эти имена —
// просто то, что виджеты ищут в Store; коннектор без такой карты для слота
// просто не даёт виджету появиться):
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
//   weather.low/.high               — number: мин/макс за сутки; коннектор
//                                      их заполняет, но draw_today их пока не
//                                      рисует — эталон (cockpit.html) их не
//                                      показывает, добавлять неутверждённое
//                                      в кадр не стали (см. Status Log)
//   weather.summary                — text: краткое описание погоды
//   event.N.at / .title            — text, N = 1..3 (ближайшие события)
//
// Сетка собирается из активного дашборда (config::Dashboard, config.h) —
// два ряда виджетов, ширина каждого — токен S/M/flex (widgets::Size,
// layout_row ниже). Заводской «Стол» воспроизводит дословно
// trmnl-ink/renderer/app/templates/cockpit.html (эталон:
// reference/cockpit-reference.png, обмеры — Status Log в
// .claude/plans/tablo.md): верхний ряд — Рынки (M, 296px) слева и гибкая
// колонка Лимиты+Воздух справа, нижний — гибкая Почта слева и Сегодня (S,
// 202px) справа. Виджет без единого видимого слота не резервирует место —
// соседи по ряду делят освободившееся пространство (docs/widgets.md).
#pragma once

#include <cstdint>
#include <vector>

#include "canvas.h"
#include "slots.h"
#include "widgets/types.h"

// Только объявление типа — draw_frame() ниже берёт дашборд по ссылке, самого
// определения (config::Dashboard) заголовку знать не нужно. config.h тянет
// сети, коннекторы, NVS — раскладке из этого нужен только один тип, полный
// #include "config.h" был бы лишней связью на уровне заголовка.
namespace config {
struct Dashboard;
}

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
// Status Log в .claude/plans/tablo.md, что именно это значит на устройстве.
Civil to_civil(uint32_t unix_time, int16_t timezone_minutes);

// nullptr или Slot::empty() — трактуются одинаково: показывать нечего.
bool has_data(const slots::Slot* s);

// Общее форматирование значения: «—» на пустом слоте (ноль читается как
// «свободно», это уже вводило в заблуждение — docs/decisions.md, п.6), «≈»
// перед устаревшим (свежесть видна всегда), иначе просто число.
String format_percent(const slots::Slot* s, uint32_t now);
String format_value(const slots::Slot* s, uint32_t now, int decimals, const char* suffix);

// Видимость блока по наличию хотя бы одного из его слотов — раньше жила
// здесь пятью функциями (rates_visible/limits_visible/...), теперь это
// widgets::Spec::visible каждого виджета (widgets/w_*.cpp): раскладка не
// должна знать имена слотов конкретных блоков, это знание принадлежит
// самому виджету (docs/widgets.md). Источник отвалился целиком — виджет не
// резервирует место соседям (см. layout_row ниже).

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

// Раскладывает виджеты ОДНОГО ряда дашборда по токенам размера (S/M/flex —
// widgets::Size, docs/widgets.md, «Правило ряда»): видимые с фиксированным
// размером (S/M) берут свои пиксели, flex делят остаток поровну между собой;
// если среди видимых нет ни одного flex — видимые делят весь ряд поровну (так
// в эталоне Рынки занимают всю ширину верхнего ряда, когда правая колонка
// пропала: там n=1, «поровну» на одного — это вся ширина). Невидимый виджет
// получает нулевой Rect и не резервирует место соседям — тот же принцип, что
// у compute_columns выше, но по вектору Instance, а не по голому total.
// items и out — одной длины; visible — тем же индексом.
void layout_row(Rect row, int16_t gap, const std::vector<widgets::Instance>& items,
                 const std::vector<bool>& visible, Rect* out);

// ── рисование: единственная часть, которой нужна канва ──

// Кадр целиком: шапка + дашборд, собранный по факту наличия данных виджетов
// (widgets::Spec::visible/draw, widgets/registry.cpp). Рисуется всегда, даже
// когда store пуст, — контракт «пустой снапшот должен отрендериться без
// падения» (trmnl-ink/docs/frame-contract.md, инвариант 1); пустой дашборд
// (config::Dashboard с пустыми rows, заводской «Свой») даёт кадр с одной
// шапкой, ровно как раньше пустой store.
void draw_frame(canvas::Canvas& canvas, const slots::Store& store, const DeviceInfo& device,
                 const config::Dashboard& dashboard);

// Кадр с учётными данными точки доступа (docs/decisions.md, п.8): имя сети и
// пароль текстом — камера может не сработать, вводить руками должно быть чем
// — и рядом QR формата WIFI: для подключения наведением камеры. Через тот же
// Canvas, что и draw_frame, — тогда кадр снимается тем же хостовым
// инструментом (tools/render_frame), а не только на живой панели.
// Экран включения: имя устройства крупно, подпись и строка состояния —
// вместо тестового узора фазы 0. Тем же Canvas, что и остальные кадры, —
// снимается tools/render_frame (boot.png).
void draw_boot(canvas::Canvas& canvas, const char* status);

void draw_ap_credentials(canvas::Canvas& canvas, const String& ssid, const String& password);

}  // namespace layout
