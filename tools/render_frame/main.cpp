// Инструмент снятия кадра на хосте — без него типографику пришлось бы
// проверять только на живой панели, а она либо занята другим проектом, либо
// физически недоступна (docs/decisions.md, п.6). Три сценария демонстрируют
// главное архитектурное свойство раскладки: сетка пересобирается по факту
// наличия данных, без дыр и без рамок «нет данных».
//
// Собирается напрямую g++, в обход PlatformIO: это не прошивка и не
// unit-тест на unity, а обычная хостовая программа. См. build_and_run.sh.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../firmware/src/canvas_mem.h"
// config.cpp, не config.h: defaults() (заводские дашборды) — внутренний
// помощник в анонимном namespace config.cpp, с внешней линковкой недоступен
// другой единице трансляции — тем же приёмом, что тесты (test_config)
// подключают config.cpp исходником, а не линкуют отдельно.
#include "../../firmware/src/config.cpp"
#include "../../firmware/src/i18n.h"
#include "../../firmware/src/layout.h"
#include "../../firmware/src/slots.h"

namespace {

// Время кадра — одна отметка на весь инструмент, чтобы `at` в демо-данных
// можно было задавать смещением от неё ("минуту назад", "час назад"), а не
// произвольными маленькими числами: слот сравнивает возраст как `now - at`,
// и если `now` — настоящий unix-timestamp, а `at` — что-то вроде 1000, слот
// мгновенно оказывается «древним» и рисуется с «≈» всегда, что не показывает
// ничего интересного про раскладку.
constexpr uint32_t kNow = 1758000000;  // фиксированная отметка — кадр воспроизводим

void put_number(slots::Store& store, const char* id, float number, float delta = 0,
                uint32_t age_seconds = 0, uint32_t ttl = 300, const char* connector = "demo") {
    slots::Slot s;
    s.number = number;
    s.delta = delta;
    s.at = kNow - age_seconds;
    s.ttl = ttl;
    s.ok = true;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(number));
    s.text = String(buf);
    store.put(id, s, connector);
}

void put_text(slots::Store& store, const char* id, const char* text, uint32_t age_seconds = 0,
             uint32_t ttl = 300, const char* connector = "demo") {
    slots::Slot s;
    s.text = String(text);
    s.ok = true;
    s.at = kNow - age_seconds;
    s.ttl = ttl;
    store.put(id, s, connector);
}

// История для спарклайна — явным put_history(), как теперь и делают
// коннекторы (btc_history/klines, HA /api/history/period): Store::put()
// больше не копит её сам из повторных put() (см. Status Log — накопление из
// пятиминутных опросов подписывало «ЗА 24 Ч» под сорока минутами реальных
// данных). Последняя точка values — это ЕЩЁ НЕ текущее значение, текущее
// задаётся отдельным put_number() с дельтой, как и раньше.
void put_series(slots::Store& store, const char* id, std::initializer_list<float> values,
                uint32_t age_seconds, uint32_t ttl, const char* connector) {
    (void)age_seconds;
    (void)ttl;
    std::vector<float> v(values);
    store.put_history(id, v.data(), static_cast<uint8_t>(v.size()), connector);
}

// ── сценарий 1: все источники отвечают — все блоки на месте, верхний ряд
// в три колонки, нижний — в две.
slots::Store build_full_scenario() {
    slots::Store store;

    put_series(store, "btc", {61200, 61800, 60900, 62500, 63100, 62800}, 90, 300, "rates");
    // Последняя точка — отдельным put() с дельтой: put_series не занимается
    // дельтой, а история к этому моменту уже накоплена предыдущими вызовами.
    put_number(store, "btc", 64200, 2.35f, 20, 300, "rates");
    put_number(store, "usd_rub", 96.4f, 0.15f, 40, 900, "rates");
    put_number(store, "eur_rub", 104.8f, -0.30f, 40, 900, "rates");

    put_number(store, "limit.claude.5h", 42, 0, 30, 240, "limits");
    put_number(store, "limit.claude.week", 78, 0, 30, 240, "limits");
    put_text(store, "limit.claude.reset", "5h in 3:06 · week in 2d 22h", 30, 240, "limits");
    put_number(store, "limit.codex", 15, 0, 30, 240, "limits");
    put_text(store, "limit.codex.reset", "week · reset 13:01 · in 2d 21h", 30, 240,
             "limits");

    // НОРМА (не свежо, не тревога) — демонстрирует третье начертание тега
    // рядом с двумя другими (СВЕЖО у... впрочем ниже TVOC покажет тревогу).
    put_series(store, "co2", {620, 680, 750, 780}, 20, 300, "air");
    put_number(store, "co2", 798, 92, 20, 300, "air");  // последняя точка — с дельтой /ч
    // TVOC — сценарий тревоги (как в эталоне): высокое значение, инверсная
    // плашка "ПРОВЕТРИТЬ" вместо рамки "НОРМА".
    put_series(store, "tvoc", {4200, 4800, 5100, 5300}, 20, 300, "air");
    put_number(store, "tvoc", 5496, 5277, 20, 300, "air");

    put_number(store, "mail.unread", 7, 0, 60, 900, "mail");
    put_text(store, "mail.1.from", "Home Assistant", 60, 900, "mail");
    put_text(store, "mail.1.subject", "Air sensor battery is low", 60, 900, "mail");
    put_text(store, "mail.1.time", "14:32", 60, 900, "mail");
    put_text(store, "mail.2.from", "GitHub", 60, 900, "mail");
    put_text(store, "mail.2.subject", "[tablo] CI passed", 60, 900, "mail");
    put_text(store, "mail.2.time", "12:05", 60, 900, "mail");
    put_text(store, "mail.3.from", "Airline", 60, 900, "mail");
    put_text(store, "mail.3.subject", "Check-in is open, boarding pass inside", 60, 900,
            "mail");
    put_text(store, "mail.3.time", "yesterday", 60, 900, "mail");

    put_number(store, "weather.temp", -2, 0, 300, 3600, "today");
    put_text(store, "weather.summary", "-4…+1 · CLOUDY", 300, 3600, "today");
    put_text(store, "event.1.at", "10:00", 300, 3600, "today");
    put_text(store, "event.1.title", "Sync on tablo with myself", 300, 3600, "today");
    put_text(store, "event.2.at", "18:30", 300, 3600, "today");
    put_text(store, "event.2.title", "Pick up the parcel", 300, 3600, "today");

    return store;
}

// ── сценарий 2: часть источников отвалилась (лимиты и почта молчат) — их
// блоков нет, а рынки/воздух и погода занимают освободившееся место шире,
// без дыр в сетке (frame-contract.md, инвариант 1).
slots::Store build_degraded_scenario() {
    slots::Store store;

    put_number(store, "btc", 64200, -1.8f, 20, 300, "rates");
    put_number(store, "usd_rub", 96.4f, 0, 40, 900, "rates");

    // Лимит недели устарел (данные были, но TTL давно прошёл) — должен
    // показаться с "≈", а не исчезнуть и не обнулиться.
    put_number(store, "limit.claude.week", 91, 0, 4000, 240, "limits");
    // GPT данных нет, но коннектор объяснил почему — строка остаётся с текстом
    // причины вместо шкалы (prims.cpp, limits_error_text).
    store.mark_failed(String("codex"), String(i18n::tr(i18n::Str::kRateLimited)));

    put_series(store, "co2", {900, 1050, 1180, 1260}, 20, 300, "air");

    put_number(store, "weather.temp", 5, 0, 300, 3600, "today");
    put_text(store, "weather.summary", "+2…+7 · CLEAR", 300, 3600, "today");

    return store;
}

// ── сценарий 3: ни один коннектор ещё не настроен — кадр не падает и не
// рисует рамки "нет данных", просто шапка на пустом экране.
slots::Store build_empty_scenario() { return slots::Store(); }

bool render(const slots::Store& store, const layout::DeviceInfo& device, const std::string& path,
            const config::Dashboard& dashboard) {
    canvas::CanvasMemory canvas(800, 480);
    layout::draw_frame(canvas, store, device, dashboard);
    bool ok = canvas.save_png(path);
    std::printf("%s -> %s\n", ok ? "OK" : "FAIL", path.c_str());
    return ok;
}

// Кадр точки доступа (docs/decisions.md, п.8) — отдельный сценарий: своя
// раскладка (layout::draw_ap_credentials), не draw_frame. ssid/password —
// не выдумка, а то же самое значение, которое проверяется распознаванием QR
// со снятого PNG (см. Status Log в .claude/plans/tablo.md): если тут и в
// проверке разойдётся строка — расхождение сразу увидит тот, кто это читает.
bool render_boot(const std::string& path) {
    canvas::CanvasMemory canvas(800, 480);
    layout::draw_boot(canvas, i18n::tr(i18n::Str::kBootStatus));
    bool ok = canvas.save_png(path);
    std::printf("%s -> %s\n", ok ? "OK" : "FAIL", path.c_str());
    return ok;
}

// ── сценарий 4: все источники отвалились с причиной — ни один блок не
// исчезает, каждый объясняет (правило владельца, 2026-09-22).
slots::Store build_failures_scenario() {
    slots::Store store;
    store.mark_failed(String("btc"), String("source did not respond (code 503)"));
    store.mark_failed(String("home"), String("Home Assistant is not responding"));
    store.mark_failed(String("claude"), String(i18n::tr(i18n::Str::kRateLimited)));
    store.mark_failed(String("codex"), String("token expired — update it on the setup page"));
    store.mark_failed(String("mail"), String("imap: LOGIN rejected — check the app password"));
    store.mark_failed(String("weather"), String(i18n::tr(i18n::Str::kSourceNoAnswer)));
    return store;
}

bool render_ap_credentials(const std::string& path) {
    canvas::CanvasMemory canvas(800, 480);
    layout::draw_ap_credentials(canvas, "tablo-setup", "23456789AB");
    bool ok = canvas.save_png(path);
    std::printf("%s -> %s\n", ok ? "OK" : "FAIL", path.c_str());
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = argc > 1 ? argv[1] : "tools/render_frame/out";
    std::string mk = "mkdir -p " + out_dir;
    std::system(mk.c_str());

    i18n::set_lang(i18n::Lang::kEn);

    layout::DeviceInfo device;
    device.wifi_rssi = -62;
    device.battery_pct = 74;
    device.now = kNow;
    device.next_update_at = kNow + 1800;
    device.timezone_minutes = 180;  // MSK

    // Заводские дашборды (config::defaults()) — «Стол»/«Дорога»/«Свой»,
    // docs/widgets.md. full.png дословно равен dashboard-0.png (тот же
    // сценарий, тот же дашборд «Стол») — имя сохранено ради обратной
    // совместимости с тем, что уже проверяет tools/compare_frame.py.
    const config::Settings defaults = config::defaults();

    bool ok = true;
    ok &= render(build_full_scenario(), device, out_dir + "/full.png", defaults.dashboards[0]);
    ok &= render(build_degraded_scenario(), device, out_dir + "/degraded.png", defaults.dashboards[0]);
    ok &= render(build_empty_scenario(), device, out_dir + "/empty.png", defaults.dashboards[0]);
    ok &= render_ap_credentials(out_dir + "/ap_credentials.png");
    ok &= render_boot(out_dir + "/boot.png");
    ok &= render(build_failures_scenario(), device, out_dir + "/failures.png", defaults.dashboards[0]);

    // Три заводских дашборда, сценарий «все источники отвечают» — тот же
    // build_full_scenario(), меняется только то, какие виджеты в дашборде.
    ok &= render(build_full_scenario(), device, out_dir + "/dashboard-0.png", defaults.dashboards[0]);
    ok &= render(build_full_scenario(), device, out_dir + "/dashboard-1.png", defaults.dashboards[1]);
    ok &= render(build_full_scenario(), device, out_dir + "/dashboard-2.png", defaults.dashboards[2]);

    return ok ? 0 : 1;
}
