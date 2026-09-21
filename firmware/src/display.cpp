#include "display.h"

#include <GxEPD2_BW.h>
#include <SPI.h>

#include "board.h"
#include "canvas_gxepd2.h"

namespace {

// Панель кита промаркирована как GD7965 (он же EK79655/UC8179) — в терминах
// GxEPD2 это модель GDEW075T7, класс GxEPD2_750_T7. Не путать с классом
// GxEPD2_750 (без суффикса): та — другая, более старая панель 640x384 без
// частичного обновления, совпадение цифр в имени вводит в заблуждение.
using Panel = GxEPD2_750_T7;

// page_height = HEIGHT: буфер сразу на весь экран, без постраничной отрисовки.
// 800×480/8 = 48 КБ — ровно то число, под которое явно включена раскладка
// flash/PSRAM в platformio.ini; постраничный режим здесь не нужен.
//
// BUSY передан как -1 (не подключен для драйвера) осознанно: GxEPD2_750_T7
// жёстко зашивает busy_level = LOW внутри своего конструктора (см.
// GxEPD2_750_T7.cpp) и не даёт способа переопределить полярность снаружи. На
// этом ките BUSY инвертирован — panel занята при HIGH (см. board.h), то есть
// с реальным пином опрос драйвера ловил бы «свободна» в момент, когда панель
// ещё занята. Библиотека предусматривает для этого штатный запасной путь:
// если busy < 0, вместо опроса пина используются её собственные консервативные
// паузы (power_on_time/power_off_time/full_refresh_time/partial_refresh_time).
// Плата за это — обновление всегда ждёт полное время из этих констант, а не
// завершается раньше по факту готовности; для рисования кадра это не проблема.
GxEPD2_BW<Panel, Panel::HEIGHT> epd(Panel(board::EPD_CS, board::EPD_DC, board::EPD_RESET, -1));

}  // namespace

namespace display {

void begin() {
    // SPI на ESP32-S3 в этом ките разведён не на дефолтные пины VSPI — без
    // явного SPI.begin() с нужными CLK/MOSI GxEPD2 попытается говорить с
    // панелью по чужим ногам. MISO не подключен: панель только принимает
    // данные, читать с неё (OTP, температура) не требуется.
    SPI.begin(board::SPI_CLK, -1, board::SPI_MOSI, board::EPD_CS);

    epd.init(0, true, 10, false, SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    epd.setRotation(0);
}

void show_boot_screen() {
    // Полное обновление: это первый кадр после включения, панель могла хранить
    // остаточное изображение прошлого сеанса — полный цикл его снимает.
    epd.setFullWindow();
    canvas::CanvasGxEPD2<Panel> canvas(epd);
    epd.firstPage();
    do {
        layout::draw_boot(canvas, "включение · ищем сохранённую сеть…");
    } while (epd.nextPage());
}

void refresh_full() {
    // Явный полный refresh поверх того, что уже лежит в буфере: используется
    // после серии частичных обновлений, чтобы убрать остаточное изображение.
    // setFullWindow() гарантирует режим "полностью", а не "быстро частично".
    epd.setFullWindow();
    epd.display(false);
}


void show_frame(const slots::Store& store, const layout::DeviceInfo& device,
                const config::Dashboard& dashboard) {
    // Частичное обновление: полное моргает всем экраном около секунды, а этот
    // кадр перерисовывается часто.
    epd.setPartialWindow(0, 0, board::SCREEN_WIDTH, board::SCREEN_HEIGHT);
    canvas::CanvasGxEPD2<Panel> canvas(epd);
    epd.firstPage();
    do {
        layout::draw_frame(canvas, store, device, dashboard);
    } while (epd.nextPage());
}

void show_ap_credentials(const String& ssid, const String& password) {
    // Частичное обновление — тот же приём, что и в show_frame: полное
    // моргает секунду, а этот кадр висит на экране, пока не найдётся
    // сохранённая сеть, то есть потенциально всю поездку до первой настройки.
    //
    // Через canvas::CanvasGxEPD2 и layout::draw_ap_credentials — та же пара,
    // что рисует основной кадр (show_frame): растровый шрифт Terminus вместо
    // латиницы Adafruit_GFX и QR через wifi_qr, а не отдельная реализация
    // здесь. Это же делает кадр снимаемым хостовым инструментом
    // (tools/render_frame) для проверки без устройства.
    epd.setPartialWindow(0, 0, board::SCREEN_WIDTH, board::SCREEN_HEIGHT);
    canvas::CanvasGxEPD2<Panel> canvas(epd);
    epd.firstPage();
    do {
        layout::draw_ap_credentials(canvas, ssid, password);
    } while (epd.nextPage());
}

}  // namespace display
