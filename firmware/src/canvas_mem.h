// Канва в ОЗУ для хоста: тесты и инструмент снятия кадра (tools/render_frame).
//
// Раскладку нельзя проверить только на живом устройстве — оно физически может
// быть занято другим проектом либо вообще недоступно (см. это в задаче фазы и
// docs/decisions.md, п.6). CanvasMemory копит кадр в обычном массиве байт (по
// пикселю на байт — не 1 бит на пиксель, ради простоты кода конвертера, а не
// ради экономии памяти, которая на хосте не критична) и умеет сохранить его в
// PNG без внешних библиотек: PNG допускает несжатые ("stored") блоки deflate,
// а CRC32/Adler32 — десяток строк каждый, так что зависимость от zlib не
// нужна ради файла, который открывается один раз глазами.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas.h"

namespace canvas {

class CanvasMemory : public Canvas {
   public:
    CanvasMemory(int16_t w, int16_t h);

    int16_t width() const override { return width_; }
    int16_t height() const override { return height_; }
    void pixel(int16_t x, int16_t y, Color c) override;

    // 1 — чёрный пиксель, 0 — белый; вне границ считается белым.
    uint8_t at(int16_t x, int16_t y) const;

    // Несжатый PNG (grayscale, 8 бит на пиксель) — открывается любым
    // просмотрщиком, не требует библиотек на стороне записи.
    bool save_png(const std::string& path) const;

   private:
    int16_t width_;
    int16_t height_;
    std::vector<uint8_t> pixels_;  // width_*height_, 0/1
};

}  // namespace canvas
