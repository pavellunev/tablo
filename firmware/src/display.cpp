#include "display.h"

#include <GxEPD2_BW.h>
#include <SPI.h>

#include "board.h"

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
    epd.setFullWindow();
    epd.firstPage();
    do {
        epd.fillScreen(GxEPD_WHITE);

        // рамка по всему периметру экрана — проверяем, что драйвер не обрезает
        // крайние пиксели буфера при 800×480.
        epd.drawRect(0, 0, board::SCREEN_WIDTH, board::SCREEN_HEIGHT, GxEPD_BLACK);

        // встроенный растровый шрифт Adafruit_GFX кириллицу не содержит (в его
        // битмапах нет глифов за пределами ASCII) — полноценный Terminus с
        // кириллицей подключаем в фазе 3, здесь кегли проверяем на латинице.
        epd.setTextColor(GxEPD_BLACK);

        epd.setTextSize(1);
        epd.setCursor(20, 40);
        epd.print("inkroam - phase 0 boot screen");

        epd.setTextSize(2);
        epd.setCursor(20, 70);
        epd.print("GxEPD2_750_T7 800x480");

        epd.setTextSize(3);
        epd.setCursor(20, 110);
        epd.print("size 3x");

        epd.setTextSize(4);
        epd.setCursor(20, 150);
        epd.print("size 4x");

        // горизонтальные линии — проверка прямых без сглаживания на e-ink.
        for (int16_t y = 210; y <= 250; y += 10) {
            epd.drawFastHLine(20, y, board::SCREEN_WIDTH - 40, GxEPD_BLACK);
        }

        // залитый прямоугольник — проверка сплошной заливки без артефактов.
        epd.fillRect(20, 270, 200, 100, GxEPD_BLACK);

        epd.setTextSize(1);
        epd.setCursor(20, board::SCREEN_HEIGHT - 30);
        epd.print("full refresh, no partial yet");
    } while (epd.nextPage());
}

void refresh_full() {
    // Явный полный refresh поверх того, что уже лежит в буфере: используется
    // после серии частичных обновлений, чтобы убрать остаточное изображение.
    // setFullWindow() гарантирует режим "полностью", а не "быстро частично".
    epd.setFullWindow();
    epd.display(false);
}


void show_status(const String& network, const String& ip, const slots::Store& store,
                 uint32_t now) {
    // Частичное обновление: полное моргает всем экраном около секунды, а этот
    // кадр перерисовывается часто.
    epd.setPartialWindow(0, 0, board::SCREEN_WIDTH, board::SCREEN_HEIGHT);
    epd.firstPage();
    do {
        epd.fillScreen(GxEPD_WHITE);
        epd.setTextColor(GxEPD_BLACK);

        epd.setTextSize(2);
        epd.setCursor(20, 30);
        epd.print("inkroam");

        epd.setTextSize(1);
        epd.setCursor(20, 60);
        epd.print(network);
        if (ip.length() > 0) {
            epd.print("  ");
            epd.print(ip);
        }

        epd.drawLine(20, 75, board::SCREEN_WIDTH - 20, 75, GxEPD_BLACK);

        int16_t y = 100;
        if (store.size() == 0) {
            epd.setCursor(20, y);
            epd.print("no data yet - configure a connector");
        }
        for (const auto& entry : store.all()) {
            if (y > board::SCREEN_HEIGHT - 30) {
                break;  // ниже панели рисовать некуда
            }
            const slots::Slot& slot = entry.second;

            epd.setCursor(20, y);
            epd.print(entry.first);

            epd.setCursor(300, y);
            // Прочерк вместо значения: пустой слот и нулевой — разные вещи,
            // а ноль на месте «нет данных» читается как настоящая величина.
            if (slot.empty()) {
                epd.print("-");
            } else {
                epd.print(slot.text);
                // Возраст рядом со значением: кадр, молча показывающий
                // вчерашнее, выглядит исправным — это худший вид поломки.
                if (slot.stale(now)) {
                    epd.print("  (stale)");
                }
            }
            y += 20;
        }
    } while (epd.nextPage());
}

void show_ap_credentials(const String& ssid, const String& password) {
    // Частичное обновление — тот же приём, что и в show_status: полное
    // моргает секунду, а этот кадр висит на экране, пока не найдётся
    // сохранённая сеть, то есть потенциально всю поездку до первой настройки.
    epd.setPartialWindow(0, 0, board::SCREEN_WIDTH, board::SCREEN_HEIGHT);
    epd.firstPage();
    do {
        epd.fillScreen(GxEPD_WHITE);
        epd.setTextColor(GxEPD_BLACK);

        epd.setTextSize(2);
        epd.setCursor(20, 40);
        epd.print("connect to set up inkroam");

        epd.setTextSize(1);
        epd.setCursor(20, 90);
        epd.print("network:");
        // Кегль — целое число, кратное встроенному растровому шрифту (см.
        // docs/decisions.md, п.6: дробный масштаб рвёт штрихи).
        epd.setTextSize(3);
        epd.setCursor(20, 115);
        epd.print(ssid);

        epd.setTextSize(1);
        epd.setCursor(20, 190);
        epd.print("password:");
        epd.setTextSize(4);
        epd.setCursor(20, 220);
        epd.print(password);

        epd.setTextSize(1);
        epd.setCursor(20, board::SCREEN_HEIGHT - 30);
        epd.print("join this network on your phone, then open the setup page");
    } while (epd.nextPage());
}

}  // namespace display
