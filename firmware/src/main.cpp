// Цикл устройства: подняться, найти сеть, опрашивать источники, рисовать.
//
// Раскладки пока нет (фаза 3) — на панель уходит служебный кадр с состоянием
// сети и списком полученных слотов. Он не временная заглушка ради галочки, а
// рабочий инструмент: по нему видно, отвечает ли коннектор и что именно он
// прислал, без подключения кабеля и чтения логов.

#include <Arduino.h>

#include "board.h"
#include "config.h"
#include "connectors.h"
#include "display.h"
#include "netman.h"
#include "portal.h"
#include "slots.h"

namespace {

slots::Store g_slots;

// Перерисовываем не чаще, чем раз в полминуты: у e-ink каждое обновление —
// расход ресурса панели, а данные всё равно приходят по своим интервалам.
constexpr uint32_t REDRAW_INTERVAL_MS = 30'000;
uint32_t g_last_redraw = 0;

// Счётчик частичных обновлений. Копить их бесконечно нельзя — накапливается
// остаточное изображение, поэтому периодически вызываем полное.
constexpr uint16_t FULL_REFRESH_EVERY = 60;
uint16_t g_partial_count = 0;

// Последний известный режим сети — нужен только чтобы поймать момент, когда
// netman только что поднял точку доступа (см. loop() ниже).
netman::Mode g_last_mode = netman::Mode::kConnecting;

uint32_t now_seconds() { return millis() / 1000; }

// Что сейчас нарисовано на кадре с учётными данными точки доступа — чтобы не
// перерисовывать неизменившееся.
String g_shown_ap_ssid;
String g_shown_ap_password;

void redraw() {
    if (++g_partial_count >= FULL_REFRESH_EVERY) {
        display::refresh_full();
        g_partial_count = 0;
        return;
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
    display::show_status(netman::status_text(), netman::ip().toString(), g_slots,
                         now_seconds());
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);  // USB CDC поднимается не мгновенно; иначе первые строки пропадают
    Serial.println("inkroam: старт");

    display::begin();
    display::show_boot_screen();

    netman::begin(config::load());
    portal::begin();

    Serial.printf("inkroam: сеть — %s\n", netman::status_text().c_str());
}

void loop() {
    netman::loop();
    portal::loop();

    // Точка доступа только что поднялась (при старте или после неудачной
    // попытки вернуться в сохранённую сеть) — пароль на панели рисуем сразу,
    // а не ждём плановую перерисовку: до неё владельцу пришлось бы стоять
    // рядом с устройством до REDRAW_INTERVAL_MS, не зная, куда подключаться.
    netman::Mode mode = netman::mode();
    if (mode == netman::Mode::kAccessPoint && mode != g_last_mode) {
        redraw();
        g_last_redraw = millis();
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
    if (netman::mode() == netman::Mode::kStation) {
        connectors::poll_due(netman::settings().connectors, g_slots, now_seconds());
    }

    const uint32_t now = millis();
    if (now - g_last_redraw >= REDRAW_INTERVAL_MS) {
        g_last_redraw = now;
        redraw();
    }

    delay(50);  // сторожевому таймеру нужен воздух, а спешить тут некуда
}
