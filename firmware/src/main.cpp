// Цикл устройства: подняться, найти сеть, опрашивать источники, рисовать.
//
// Раскладки пока нет (фаза 3) — на панель уходит служебный кадр с состоянием
// сети и списком полученных слотов. Он не временная заглушка ради галочки, а
// рабочий инструмент: по нему видно, отвечает ли коннектор и что именно он
// прислал, без подключения кабеля и чтения логов.

#include <Arduino.h>
#include <ArduinoJson.h>

#include <time.h>

#include "battery.h"
#include "board.h"
#include "buttons.h"
#include "config.h"
#include "connectors.h"
#include "display.h"
#include "layout.h"
#include "netman.h"
#include "portal.h"
#include "slots.h"
#include "widgets/demand.h"

namespace {

slots::Store g_slots;

// Расписание обновлений панели — по замечанию с живого экрана: частые
// частичные обновления делают картинку блеклой (остаточное изображение
// копится), а каждое из них — расход ресурса e-ink. Поэтому:
//  - частичное — не чаще раза в 5 минут и только если содержимое слотов
//    действительно изменилось (данные и так приходят раз в 5 минут; минутный
//    таймер в шапке сам по себе перерисовки не стоит);
//  - полное — раз в час по времени, а не по счётчику: оно снимает блеклость и
//    не зависит от того, сколько частичных набежало.
constexpr uint32_t PARTIAL_MIN_INTERVAL_MS = 5UL * 60 * 1000;
constexpr uint32_t FULL_REFRESH_INTERVAL_MS = 60UL * 60 * 1000;
uint32_t g_last_redraw = 0;
uint32_t g_last_full_refresh = 0;
uint32_t g_last_content_hash = 0;

// Кнопка только что переключила дашборд — перерисовать немедленно, минуя
// пятиминутный интервал частичного обновления (buttons.h, короткое
// нажатие): владелец нажал кнопку и ждёт смены экрана сейчас, а не через
// пять минут.
bool g_force_redraw = false;

// Хэш содержимого слотов — что реально видно на экране. Время в него не
// входит намеренно: иначе каждая минута выглядела бы как «изменение».
// active_dashboard подмешан: смена дашборда меняет то, что нарисовано,
// даже когда сами слоты не изменились ни на бит — без этого поля переход
// на другой экран ждал бы случайного изменения данных, чтобы попасть в
// content_changed (g_force_redraw закрывает это для НЕМЕДЛЕННОЙ перерисовки,
// но хэш должен отражать смену и для последующих обычных сравнений).
uint32_t content_hash(const slots::Store& store, uint8_t active_dashboard) {
    uint32_t h = 2166136261u;  // FNV-1a
    h ^= active_dashboard;
    h *= 16777619u;
    for (const auto& kv : store.all()) {
        const slots::Slot& sl = kv.second;
        for (const char* p = kv.first.c_str(); *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
        for (const char* p = sl.text.c_str(); *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
        h ^= sl.ok ? 1u : 0u; h *= 16777619u;
        h ^= static_cast<uint32_t>(sl.history_len); h *= 16777619u;
    }
    return h;
}

// Последний известный режим сети — нужен только чтобы поймать момент, когда
// netman только что поднял точку доступа (см. loop() ниже).
netman::Mode g_last_mode = netman::Mode::kConnecting;

// Настоящее время появляется после синхронизации с NTP; до неё — секунды с
// включения. Порог отсекает 1970 год: часы ESP32 стартуют с эпохи, и без
// синхронизации на кадре стояло «ЧТ · 1 ЯНВАРЯ», а таймер обновления шёл от
// нуля. Дата в кадре считается из UTC со сдвигом из настроек, поэтому здесь
// отдаём UTC, а не локальное время.
bool g_time_synced = false;

uint32_t now_seconds() {
    const time_t t = time(nullptr);
    if (t > 1'600'000'000) {  // сентябрь 2020: любое реальное время больше
        g_time_synced = true;
        return static_cast<uint32_t>(t);
    }
    return millis() / 1000;
}

// Что сейчас нарисовано на кадре с учётными данными точки доступа — чтобы не
// перерисовывать неизменившееся.
String g_shown_ap_ssid;
String g_shown_ap_password;

void redraw() {
    // Полное обновление — раз в час, по времени. Оно не рисует данные, а
    // прогоняет панель через полный цикл, чтобы снять накопившуюся блеклость;
    // следом идёт обычная отрисовка кадра.
    if (millis() - g_last_full_refresh >= FULL_REFRESH_INTERVAL_MS) {
        display::refresh_full();
        g_last_full_refresh = millis();
    }
    if (netman::mode() == netman::Mode::kAccessPoint) {
        // В режиме точки доступа опрос источников не идёт (см. loop() ниже),
        // и показывать нечего, кроме того, куда и с каким паролем
        // подключиться (docs/decisions.md, п.8).
        //
        // Кадр статичный, а режим точки доступа — по определению из п.8 не
        // «минуты настройки», а вся поездка до первой удачной сети. Рисовать
        // одно и то же каждые полминуты — 2880 обновлений панели в сутки
        // впустую, а её ресурс конечен.
        const String ssid = netman::ap_ssid();
        const String password = netman::ap_password();
        if (ssid == g_shown_ap_ssid && password == g_shown_ap_password) {
            return;
        }
        g_shown_ap_ssid = ssid;
        g_shown_ap_password = password;
        display::show_ap_credentials(ssid, password);
        return;
    }

    // Ушли из режима точки доступа — забываем показанное, иначе возврат в
    // него с теми же учётными данными не перерисует экран.
    g_shown_ap_ssid = String();
    g_shown_ap_password = String();

    layout::DeviceInfo device;
    device.wifi_rssi = netman::rssi();
    device.battery_pct = battery::percent();
    // now_seconds() — время работы устройства (millis()/1000), не настоящее
    // unix-время: синхронизации часов (NTP) в проекте пока нет — вне рамок
    // фазы 3, см. Status Log в .claude/plans/inkroam.md. Свежесть слотов
    // (Slot::fresh/stale) от этого не страдает — там сравниваются между собой
    // значения одних и тех же часов, — а вот дата и часы в шапке кадра будут
    // отсчитываться от 1 января 1970 года, а не от реальной даты, пока NTP не
    // появится.
    device.now = now_seconds();
    device.next_update_at = device.now + PARTIAL_MIN_INTERVAL_MS / 1000;
    const config::Settings& settings = netman::settings();
    device.timezone_minutes = settings.timezone_minutes;

    // dashboards всегда ровно три, active_dashboard всегда 0..2 — инвариант,
    // который держат config::defaults()/from_json (config.cpp). Проверка
    // границ здесь — не «а вдруг», а защита от рисования по мусорному
    // индексу, если это когда-нибудь перестанет быть истиной.
    static const config::Dashboard kEmptyDashboard;
    const config::Dashboard& dashboard = settings.active_dashboard < settings.dashboards.size()
                                              ? settings.dashboards[settings.active_dashboard]
                                              : kEmptyDashboard;
    display::show_frame(g_slots, device, dashboard);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);  // USB CDC поднимается не мгновенно; иначе первые строки пропадают
    Serial.println("inkroam: старт");

    display::begin();
    display::show_boot_screen();
    battery::begin();
    buttons::begin();

    netman::begin(config::load());
    portal::begin();

    Serial.printf("inkroam: сеть — %s\n", netman::status_text().c_str());
}

void loop() {
    netman::loop();
    portal::loop();

    // Часы проверяем здесь, а не внутри redraw(): иначе g_time_synced
    // обновлялся бы только отрисовкой, и цикл рисовал бы кадр за кадром,
    // пока ждёт синхронизацию, — непрерывный износ панели без NTP.
    now_seconds();

    // Кнопки платы — до чтения режима сети ниже: долгое BTN1 меняет режим
    // прямо здесь (netman::force_access_point() не асинхронна, в отличие от
    // netman::reload()), и остаток этого тика должен увидеть уже новый режим.
    const buttons::Event btn = buttons::poll(millis());
    if (btn.kind == buttons::Kind::kShort) {
        // Короткое 1/2/3 — переключить дашборд. config::save() тем же путём,
        // что и сохранение формы на портале (portal.cpp, handle_post_config):
        // NVS остаётся единственным источником истины.
        config::Settings settings = netman::settings();
        settings.active_dashboard = btn.index;
        if (config::save(settings)) {
            // reload_now(), не reload(): мы в главном цикле, и кадр ниже в этом
            // же тике должен взять уже новый active_dashboard из netman::settings().
            netman::reload_now();
            g_force_redraw = true;
            Serial.printf("buttons: короткое BTN%u — дашборд %u\n",
                          static_cast<unsigned>(btn.index + 1), static_cast<unsigned>(btn.index));
        } else {
            // save() отказал (потолок NVS-blob) — переключать нечего: reload_now
            // прочитал бы прежний дашборд, а кнопка выглядела бы сломанной молча.
            Serial.println("buttons: настройки не сохранились — дашборд не переключён");
        }
    } else if (btn.kind == buttons::Kind::kLong && btn.index == 0) {
        // Долгое BTN1 — точка доступа по требованию, даже если сейчас есть
        // рабочая сеть (docs/widgets.md): без сохранённых сетей рядом это
        // единственный способ снова достучаться до страницы настройки.
        Serial.println("buttons: долгое BTN1 — принудительная точка доступа");
        netman::force_access_point();
    }

    // Точка доступа только что поднялась (при старте или после неудачной
    // попытки вернуться в сохранённую сеть) — пароль на панели рисуем сразу,
    // а не ждём плановую перерисовку: до неё владельцу пришлось бы стоять
    // рядом с устройством до плановой перерисовки, не зная, куда подключаться.
    netman::Mode mode = netman::mode();
    if (mode == netman::Mode::kAccessPoint && mode != g_last_mode) {
        redraw();
        g_last_redraw = millis();
    }
    if (mode != g_last_mode) {
        // Кадр точки доступа не должен висеть до пяти минут после выхода в сеть
        // (и наоборот): режим — часть содержимого, хэш слотов его не видит.
        g_last_redraw = 0;
    }
    g_last_mode = mode;

    // Опрос источников имеет смысл только при подключении к сети: в режиме
    // точки доступа устройство настраивают, а не показывают.
    //
    // Настройки читаем из netman, а не держим свою копию: раньше main.cpp
    // загружал их один раз в setup(), и после сохранения формы на портале
    // (netman::reload()) опрашивался ещё старый список коннекторов — до
    // перезагрузки устройства. netman — единственный владелец настроек в
    // рантайме (docs/decisions.md, п.6), здесь и во всех остальных
    // потребителях читаем через него.
    static netman::Mode last_net_mode = netman::Mode::kConnecting;
    static uint32_t station_since_ms = 0;
    if (netman::mode() == netman::Mode::kStation && last_net_mode != netman::Mode::kStation) {
        // Только что вышли в сеть — запускаем часы. configTime неблокирующий,
        // SNTP доберёт время в фоне; до этого now_seconds() честно отдаёт
        // время с включения, а не выдуманную дату.
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        station_since_ms = millis();
    }
    last_net_mode = netman::mode();

    // Часы только что синхронизировались — всё, что успело опроситься до
    // этого, датировано «секундами с включения»: после скачка времени такие
    // слоты выглядят устаревшими на полвека («≈» у курсов, «сброс через
    // 20000 дней» у лимитов — фото владельца, 2026-09-22). Расписание опроса
    // само перезапросит их на этом же тике (last_polled_ тоже маленький), а
    // кадр перерисовываем принудительно, не дожидаясь смены содержимого.
    static bool was_synced = false;
    if (g_time_synced && !was_synced) {
        was_synced = true;
        g_force_redraw = true;
    }

    // До синхронизации часов опрос бессмыслен: «сброс через 472222:00» и
    // письма «21.09» вместо «09:33» — и всё это застыло бы на кадре на пять
    // минут. Ждём NTP, но не дольше 30 с ПОСЛЕ ВЫХОДА В СЕТЬ — без интернета
    // к часам всё равно не дотянуться, а кадр нужен. Отсчёт от подключения,
    // не от включения: подключение к Wi-Fi само занимало 16 с, и прежний
    // порог «20 с с включения» открывал опрос за 4 с до прихода NTP.
    const bool station_now = netman::mode() == netman::Mode::kStation;
    const bool clock_ready =
        g_time_synced || (station_now && station_since_ms != 0 && millis() - station_since_ms > 30'000);
    if (netman::mode() == netman::Mode::kStation && clock_ready) {
        // По всем трём дашбордам, не только активному — переключение кнопкой
        // должно показать данные сразу, а не ждать первого опроса после
        // смены экрана (widgets::compute_demand, docs/widgets.md).
        const widgets::Demand demand = widgets::compute_demand(netman::settings());
        connectors::poll_due(netman::settings(), g_slots, now_seconds(), demand);

        // Снимок состояния для /api/status — из главного цикла, где хранилище
        // и живёт. Диагностика без кабеля: «почему пусто на экране» отвечает
        // сама страница, а не разбор с проводом в руках.
        static uint32_t last_status_at = 0;
        if (millis() - last_status_at > 5000) {
            last_status_at = millis();
            const uint32_t now = now_seconds();
            // ArduinoJson, а не склейка строк: тема письма с кавычкой ломала
            // документ целиком. Текст слотов почты наружу не отдаём — этот
            // адрес открыт всей локальной сети, а в поездке это чужой Wi-Fi;
            // для диагностики «почему пусто» хватает ok/age/error.
            JsonDocument doc;
            doc["time_synced"] = g_time_synced;
            JsonObject slots_obj = doc["slots"].to<JsonObject>();
            for (const auto& kv : g_slots.all()) {
                const slots::Slot& sl = kv.second;
                JsonObject o = slots_obj[kv.first].to<JsonObject>();
                // Значения слотов (text) наружу не отдаём вовсе: /api/status
                // открыт всей сети, а /api/config пишется из любой сети — иначе
                // чужой мог бы привязать к слоту произвольную сущность HA или
                // внутренний адрес и прочитать ответ здесь (ревью 2026-09-21).
                // Для «почему пусто на экране» хватает ok/error/age.
                o["ok"] = sl.ok;
                o["error"] = sl.error;
                o["age"] = sl.at ? (now - sl.at) : 0;
                o["ttl"] = sl.ttl;
                o["history_len"] = sl.history_len;
            }
            String json;
            serializeJson(doc, json);
            portal::set_status_json(json);
        }
    }

    const uint32_t now = millis();
    // До готовности часов станционный кадр не рисуем: на нём были бы пустые
    // слоты (опрос ещё не шёл) или мусорные даты — и любой из них открыл бы
    // пятиминутный интервал. Экран включения висит не дольше 20 с.
    if (mode == netman::Mode::kStation && !clock_ready) {
        delay(50);
        return;
    }

    // Частичное обновление — только когда есть что показать нового и не чаще
    // раза в 5 минут; полное — раз в час независимо от содержимого (внутри
    // redraw). В режиме точки доступа кадр статичный, там свой отсев.
    // g_force_redraw (кнопка сменила дашборд) обходит пятиминутный интервал —
    // владелец ждёт смену экрана сейчас, а не по расписанию.
    const uint8_t active_dashboard = netman::settings().active_dashboard;
    const bool content_changed = content_hash(g_slots, active_dashboard) != g_last_content_hash;
    const bool interval_passed = now - g_last_redraw >= PARTIAL_MIN_INTERVAL_MS;
    const bool full_due = now - g_last_full_refresh >= FULL_REFRESH_INTERVAL_MS;
    const bool first_frame = g_last_redraw == 0;
    if (first_frame || full_due || g_force_redraw || (content_changed && interval_passed)) {
        g_last_content_hash = content_hash(g_slots, active_dashboard);
        g_force_redraw = false;
        redraw();
        g_last_redraw = now;
    }

    delay(50);  // сторожевому таймеру нужен воздух, а спешить тут некуда
}
