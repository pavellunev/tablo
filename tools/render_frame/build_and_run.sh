#!/usr/bin/env bash
# Собирает и запускает render_frame — инструмент снятия кадра без устройства
# (см. комментарий в main.cpp и docs/decisions.md, п.6).
#
# Обычный g++, не PlatformIO: это хостовая программа, а не прошивка и не
# unity-тест. Arduino.h — тот же самый шим, что использует env:native для
# test_slots (String: c_str/length/сравнения, без конкатенации — см. его
# комментарий), переиспользуем, а не плодим второй такой же файл.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT_DIR="${1:-tools/render_frame/out}"
BIN="tools/render_frame/render_frame"

# QRCode (ricmoo) — тот же пакет, что и env:native/env:xiao-esp32s3
# (platformio.ini), путь фиксирован под libdeps, которые кладёт туда
# `pio pkg install -e native`; без него — понятная ошибка компиляции, а не
# вторая реализация кодирования QR ради обхода PlatformIO.
QRCODE_DIR=".pio/libdeps/native/QRCode/src"
if [ ! -f "$QRCODE_DIR/qrcode.c" ]; then
    echo "нет $QRCODE_DIR — сначала: pio pkg install -e native" >&2
    exit 1
fi

g++ -std=gnu++17 -O1 -DNATIVE_BUILD \
    -Ifirmware/src \
    -Ifirmware/test/test_slots \
    -I"$QRCODE_DIR" \
    tools/render_frame/main.cpp \
    firmware/src/layout.cpp \
    firmware/src/canvas.cpp \
    firmware/src/canvas_mem.cpp \
    firmware/src/font.cpp \
    firmware/src/slots.cpp \
    firmware/src/wifi_qr.cpp \
    "$QRCODE_DIR/qrcode.c" \
    -o "$BIN"

"./$BIN" "$OUT_DIR"
