// Тонкий интерфейс рисования: линия, прямоугольник, пиксель.
//
// Раскладка (layout.cpp) знает про координаты блоков, но не должна знать, кто
// именно кладёт пиксели на физический экран, — иначе типографику можно
// проверить только на живом устройстве, а оно как раз то, что физически
// занято или недоступно (docs/decisions.md, п.6: «инструмент снятия кадра
// нужен раньше первого решения о типографике»). У интерфейса две реализации:
// CanvasGxEPD2 (canvas_gxepd2.h, только на устройстве, пишет прямо в GxEPD2) и
// CanvasMemory (canvas_mem.h, хост, копит кадр в ОЗУ и умеет сохранить PNG/PBM).
#pragma once

#include <cstdint>

namespace canvas {

enum class Color : uint8_t { White = 0, Black = 1 };

class Canvas {
   public:
    virtual ~Canvas() = default;

    virtual int16_t width() const = 0;
    virtual int16_t height() const = 0;

    // Единственный обязательный примитив — всё остальное можно выразить через
    // него. Конкретные реализации переопределяют геометрию ниже там, где есть
    // более быстрый путь (у GxEPD2 — аппаратные fillRect/drawFastHLine).
    virtual void pixel(int16_t x, int16_t y, Color c) = 0;

    virtual void hline(int16_t x, int16_t y, int16_t w, Color c);
    virtual void vline(int16_t x, int16_t y, int16_t h, Color c);
    virtual void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c);
    virtual void rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);       // контур
    virtual void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);  // заливка
    virtual void fill(Color c);                                                  // весь холст
};

}  // namespace canvas
