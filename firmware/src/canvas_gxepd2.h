// Канва поверх GxEPD2 — реализация Canvas для реальной панели.
//
// Только на устройстве: конструктор принимает уже проинициализированный
// объект epd (см. display.cpp) по ссылке, а сам класс — только адаптер
// интерфейса, никакой инициализации SPI/панели здесь нет.
#pragma once

#include <GxEPD2_BW.h>

#include "board.h"
#include "canvas.h"

namespace canvas {

// Panel — тот же типаж, что в display.cpp (GxEPD2_750_T7). Шаблонный параметр
// вместо фиксированного типа, чтобы не тащить сюда весь список panel-классов
// GxEPD2 ради одной строки using.
template <typename Panel>
class CanvasGxEPD2 : public Canvas {
   public:
    explicit CanvasGxEPD2(GxEPD2_BW<Panel, Panel::HEIGHT>& epd) : epd_(epd) {}

    int16_t width() const override { return static_cast<int16_t>(board::SCREEN_WIDTH); }
    int16_t height() const override { return static_cast<int16_t>(board::SCREEN_HEIGHT); }

    void pixel(int16_t x, int16_t y, Color c) override {
        epd_.drawPixel(x, y, c == Color::Black ? GxEPD_BLACK : GxEPD_WHITE);
    }

    // Быстрые пути через собственные примитивы GxEPD2/Adafruit_GFX — без
    // этого рисование целого кадра шло бы попиксельно и заметно медленнее.
    void hline(int16_t x, int16_t y, int16_t w, Color c) override {
        epd_.drawFastHLine(x, y, w, c == Color::Black ? GxEPD_BLACK : GxEPD_WHITE);
    }

    void vline(int16_t x, int16_t y, int16_t h, Color c) override {
        epd_.drawFastVLine(x, y, h, c == Color::Black ? GxEPD_BLACK : GxEPD_WHITE);
    }

    void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) override {
        epd_.fillRect(x, y, w, h, c == Color::Black ? GxEPD_BLACK : GxEPD_WHITE);
    }

    void fill(Color c) override { epd_.fillScreen(c == Color::Black ? GxEPD_BLACK : GxEPD_WHITE); }

   private:
    GxEPD2_BW<Panel, Panel::HEIGHT>& epd_;
};

}  // namespace canvas
