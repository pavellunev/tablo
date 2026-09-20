#!/usr/bin/env bash
# scripts/verify.sh — DoD для inkroam. Тонкий wrapper над общим
# ~/.claude/templates/verify-core.sh: отдельного профиля PlatformIO в core
# нет, поэтому сборка/тесты PlatformIO подставлены как PY_BUILD/PY_TEST —
# тот же приём, что в trmnl-ink/scripts/verify.sh для esphome.
#
# Флаги: --fast (только build), --quiet.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

CORE="$HOME/.claude/templates/verify-core.sh"
if [ ! -f "$CORE" ]; then
    echo "❌ не найден $CORE" >&2
    exit 2
fi

# pio ставится через pip --user и не всегда в системном PATH.
export PATH="$HOME/Library/Python/3.13/bin:$PATH"

if ! command -v pio >/dev/null 2>&1; then
    echo "❌ pio (PlatformIO) не найден в PATH" >&2
    exit 2
fi

export PROJECT_TYPE=python
export PY_BUILD="pio run -e xiao-esp32s3"
# Тесты логики (слоты, раскладка, разбор конфигурации) гоняются на хосте,
# env:native — см. platformio.ini. Пока под firmware/test ничего нет, честно
# пропускаем шаг вместо того, чтобы показывать по нему ложный PASS.
if find firmware/test -mindepth 1 -maxdepth 2 -type d 2>/dev/null | grep -q .; then
    export RUN_TESTS=1
    export PY_TEST="pio test -e native"
else
    export RUN_TESTS=0
    echo "ℹ️  тестов пока нет (firmware/test пуст) — шаг Tests пропущен"
fi
# Тесты здесь — часть DoD, а не справочная информация: падение обязано
# останавливать гейт, иначе сломанный тест-билд проходит гейт как «PASS».
export TESTS_BLOCKING=1
export RUN_LINT=0

exec "$CORE" "$@"
