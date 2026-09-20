#include "canvas_mem.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace canvas {

namespace {

// ── CRC32 и Adler32: оба нужны формату PNG (CRC — на каждый chunk, Adler —
// на несжатый поток внутри zlib-обёртки IDAT). Никакой сторонней библиотеки
// ради десятка строк подключать не стали.
uint32_t crc32(const uint8_t* data, size_t len) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();

    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    constexpr uint32_t kMod = 65521;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void write_chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put_be32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> type_and_data(type, type + 4);
    type_and_data.insert(type_and_data.end(), data.begin(), data.end());
    out.insert(out.end(), type_and_data.begin(), type_and_data.end());
    put_be32(out, crc32(type_and_data.data(), type_and_data.size()));
}

// zlib-поток с несжатыми ("stored") блоками deflate: PNG требует, чтобы IDAT
// был валидным zlib-потоком, но deflate прямо предусматривает режим без
// сжатия (тип блока 00) — ровно для случаев вроде этого, когда сжимать
// нечем/незачем, а тащить настоящий deflate ради одного PNG в тестовом
// инструменте избыточно.
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.push_back(0x78);
    out.push_back(0x01);  // zlib header: metod=8 (deflate), самый быстрый уровень

    constexpr size_t kMaxBlock = 65535;
    size_t pos = 0;
    if (raw.empty()) {
        // Пустой поток всё равно должен закрыться финальным пустым блоком.
        out.push_back(0x01);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0xFF);
        out.push_back(0xFF);
    }
    while (pos < raw.size()) {
        size_t chunk = std::min(kMaxBlock, raw.size() - pos);
        bool last = (pos + chunk) >= raw.size();
        out.push_back(last ? 0x01 : 0x00);  // BFINAL | BTYPE=00 (stored), байт-выровнено
        uint16_t len = static_cast<uint16_t>(chunk);
        uint16_t nlen = static_cast<uint16_t>(~len);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(nlen & 0xFF));
        out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + pos, raw.begin() + pos + chunk);
        pos += chunk;
    }

    uint32_t adler = adler32(raw.data(), raw.size());
    put_be32(out, adler);
    return out;
}

}  // namespace

CanvasMemory::CanvasMemory(int16_t w, int16_t h)
    : width_(w), height_(h), pixels_(static_cast<size_t>(w) * h, 0) {}

void CanvasMemory::pixel(int16_t x, int16_t y, Color c) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;  // тихо режем — как настоящий экран
    pixels_[static_cast<size_t>(y) * width_ + x] = (c == Color::Black) ? 1 : 0;
}

uint8_t CanvasMemory::at(int16_t x, int16_t y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
    return pixels_[static_cast<size_t>(y) * width_ + x];
}

bool CanvasMemory::save_png(const std::string& path) const {
    // IHDR: 8-битный grayscale — проще, чем упаковывать 1 бит на пиксель, а
    // итоговый файл читает любой просмотрщик без сюрпризов с эндианностью бит.
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, static_cast<uint32_t>(width_));
    put_be32(ihdr, static_cast<uint32_t>(height_));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(0);  // color type: grayscale
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height_) * (width_ + 1));
    for (int16_t y = 0; y < height_; ++y) {
        raw.push_back(0);  // filter type "None" на каждую строку
        for (int16_t x = 0; x < width_; ++x) {
            raw.push_back(at(x, y) ? 0x00 : 0xFF);  // 1(чёрный)->0, 0(белый)->255
        }
    }

    std::vector<uint8_t> idat = zlib_stored(raw);

    std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    write_chunk(png, "IHDR", ihdr);
    write_chunk(png, "IDAT", idat);
    write_chunk(png, "IEND", {});

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return written == png.size();
}

}  // namespace canvas
