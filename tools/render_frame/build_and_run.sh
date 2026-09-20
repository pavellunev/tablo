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

g++ -std=gnu++17 -O1 -DNATIVE_BUILD \
    -Ifirmware/src \
    -Ifirmware/test/test_slots \
    tools/render_frame/main.cpp \
    firmware/src/layout.cpp \
    firmware/src/canvas.cpp \
    firmware/src/canvas_mem.cpp \
    firmware/src/font.cpp \
    firmware/src/slots.cpp \
    -o "$BIN"

"./$BIN" "$OUT_DIR"
