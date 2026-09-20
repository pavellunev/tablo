// Геометрия по умолчанию — через pixel(). Реализации переопределяют то, что
// умеют делать быстрее (см. canvas_gxepd2.cpp).
#include "canvas.h"

#include <cstdlib>

namespace canvas {

void Canvas::hline(int16_t x, int16_t y, int16_t w, Color c) {
    for (int16_t i = 0; i < w; ++i) pixel(x + i, y, c);
}

void Canvas::vline(int16_t x, int16_t y, int16_t h, Color c) {
    for (int16_t i = 0; i < h; ++i) pixel(x, y + i, c);
}

void Canvas::line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) {
    // Горизонталь/вертикаль — частый случай (рамки, разделители), быстрее и
    // ровнее через специализированные методы, чем через Брезенхэма.
    if (y0 == y1) {
        int16_t x = x0 < x1 ? x0 : x1;
        hline(x, y0, static_cast<int16_t>(std::abs(x1 - x0) + 1), c);
        return;
    }
    if (x0 == x1) {
        int16_t y = y0 < y1 ? y0 : y1;
        vline(x0, y, static_cast<int16_t>(std::abs(y1 - y0) + 1), c);
        return;
    }

    // Целочисленный Брезенхэм — на 1-битной панели сглаживание всё равно
    // невозможно, дробная точность не нужна (docs/decisions.md, п.1).
    int16_t dx = static_cast<int16_t>(std::abs(x1 - x0));
    int16_t dy = static_cast<int16_t>(-std::abs(y1 - y0));
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;

    while (true) {
        pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = static_cast<int16_t>(2 * err);
        if (e2 >= dy) {
            err = static_cast<int16_t>(err + dy);
            x0 = static_cast<int16_t>(x0 + sx);
        }
        if (e2 <= dx) {
            err = static_cast<int16_t>(err + dx);
            y0 = static_cast<int16_t>(y0 + sy);
        }
    }
}

void Canvas::rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
    if (w <= 0 || h <= 0) return;
    hline(x, y, w, c);
    hline(x, static_cast<int16_t>(y + h - 1), w, c);
    vline(x, y, h, c);
    vline(static_cast<int16_t>(x + w - 1), y, h, c);
}

void Canvas::fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
    for (int16_t row = 0; row < h; ++row) hline(x, static_cast<int16_t>(y + row), w, c);
}

void Canvas::fill(Color c) { fill_rect(0, 0, width(), height(), c); }

}  // namespace canvas
