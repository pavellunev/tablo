#include "wifi_qr.h"

#include <qrcode.h>

#include <string>
#include <vector>

namespace wifi_qr {

namespace {

// Экранирование по правилам WIFI: формата — обратный слэш ставится перед
// каждым из четырёх спецсимволов, иначе они читаются как разделители полей
// вместо содержимого ssid/password.
void append_escaped(std::string& out, const String& value) {
    for (const char* p = value.c_str(); *p; ++p) {
        char c = *p;
        if (c == '\\' || c == ';' || c == ',' || c == '"') out += '\\';
        out += c;
    }
}

}  // namespace

String payload(const String& ssid, const String& password) {
    // Строки собираются через std::string и оборачиваются в String один раз
    // на границе — у Arduino String в тестовом шиме нет конкатенации (см.
    // комментарий в connectors.cpp про тот же приём).
    std::string out = "WIFI:T:WPA;S:";
    append_escaped(out, ssid);
    out += ";P:";
    append_escaped(out, password);
    out += ";;";
    return String(out.c_str());
}

int16_t draw(canvas::Canvas& canvas, int16_t x, int16_t y, const String& text, uint8_t module_px,
             uint8_t quiet_zone_modules) {
    QRCode qr;
    // std::vector, а не буфер на стеке фиксированного размера: размер зависит
    // от версии через рантайм-функцию библиотеки, а VLA — не стандартный C++.
    std::vector<uint8_t> buffer(qrcode_getBufferSize(kVersion));
    if (qrcode_initText(&qr, buffer.data(), kVersion, ECC_MEDIUM, text.c_str()) != 0) {
        return 0;  // payload не влез в kVersion — не должно случаться, но не роняем кадр
    }

    for (uint8_t my = 0; my < qr.size; ++my) {
        for (uint8_t mx = 0; mx < qr.size; ++mx) {
            if (!qrcode_getModule(&qr, mx, my)) continue;
            int16_t px = static_cast<int16_t>(x + (quiet_zone_modules + mx) * module_px);
            int16_t py = static_cast<int16_t>(y + (quiet_zone_modules + my) * module_px);
            canvas.fill_rect(px, py, module_px, module_px, canvas::Color::Black);
        }
    }

    return side_for(module_px, quiet_zone_modules);
}

}  // namespace wifi_qr
