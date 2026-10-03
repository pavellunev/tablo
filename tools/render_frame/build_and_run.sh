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

# QRCode (ricmoo) и ArduinoJson — те же пакеты, что и env:native/
# env:xiao-esp32s3 (platformio.ini), пути фиксированы под libdeps, которые
# кладёт туда `pio pkg install -e native`; без них — понятная ошибка
# компиляции, а не вторая реализация кодирования QR/JSON ради обхода
# PlatformIO. ArduinoJson нужен с появлением config.cpp в этой сборке
# (заводские дашборды — config::defaults()).
QRCODE_DIR=".pio/libdeps/native/QRCode/src"
ARDUINOJSON_DIR=".pio/libdeps/native/ArduinoJson/src"
if [ ! -f "$QRCODE_DIR/qrcode.c" ] || [ ! -f "$ARDUINOJSON_DIR/ArduinoJson.h" ]; then
    echo "нет $QRCODE_DIR или $ARDUINOJSON_DIR — сначала: pio pkg install -e native" >&2
    exit 1
fi

# Arduino.h-шим — test_config/, не test_slots/: config.cpp (заводские
# дашборды) зовёт String::isEmpty(), которого в шиме test_slots/test_layout
# нет (см. их комментарий про то, что каждому тесту достаточно своего
# набора). test_config — надмножество, безопасно для всех остальных
# исходников этой сборки.
g++ -std=gnu++17 -O1 -DNATIVE_BUILD \
    -Ifirmware/src \
    -Ifirmware/test/test_config \
    -I"$QRCODE_DIR" \
    -I"$ARDUINOJSON_DIR" \
    tools/render_frame/main.cpp \
    firmware/src/layout.cpp \
    firmware/src/canvas.cpp \
    firmware/src/canvas_mem.cpp \
    firmware/src/font.cpp \
    firmware/src/i18n.cpp \
    firmware/src/slots.cpp \
    firmware/src/wifi_qr.cpp \
    firmware/src/widgets/types.cpp \
    firmware/src/widgets/prims.cpp \
    firmware/src/widgets/w_markets.cpp \
    firmware/src/widgets/w_limits.cpp \
    firmware/src/widgets/w_air.cpp \
    firmware/src/widgets/w_limits_air.cpp \
    firmware/src/widgets/w_mail.cpp \
    firmware/src/widgets/w_today.cpp \
    firmware/src/widgets/w_metric.cpp \
    firmware/src/widgets/w_text.cpp \
    firmware/src/widgets/registry.cpp \
    "$QRCODE_DIR/qrcode.c" \
    -o "$BIN"

"./$BIN" "$OUT_DIR"
