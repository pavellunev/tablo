#!/usr/bin/env bash
# scripts/verify.sh — DoD для tablo: сборка прошивки и хостовые тесты.
#
# Использование: scripts/verify.sh [--fast] [--quiet] [--help]
#   --fast   только сборка, без тестов
#   --quiet  без вывода PlatformIO, только итог по шагам
#
# Код выхода: 0 — всё прошло, 1 — шаг упал, 2 — нет pio или неверный аргумент.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

FAST=0
QUIET=0
for arg in "$@"; do
    case "$arg" in
        --fast) FAST=1 ;;
        --quiet) QUIET=1 ;;
        -h|--help)
            sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "неизвестный аргумент: $arg (см. --help)" >&2
            exit 2
            ;;
    esac
done

# pio ставится через `pip install --user` и не всегда в PATH: на macOS он
# в ~/Library/Python/3.x/bin, на Linux — в ~/.local/bin.
for d in "$HOME"/Library/Python/3.*/bin "$HOME/.local/bin"; do
    [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH

if ! command -v pio >/dev/null 2>&1; then
    echo "❌ pio (PlatformIO) не найден в PATH: pip install --user platformio" >&2
    exit 2
fi

FAILED=0

# run_step <название> <команда...>
run_step() {
    local name="$1"; shift
    local start=$SECONDS rc
    if [ "$QUIET" = 1 ]; then
        "$@" >/dev/null 2>&1
    else
        "$@" >&2
    fi
    rc=$?
    local dur=$((SECONDS - start))
    if [ "$rc" = 0 ]; then
        printf '  %-12s PASS (%ss)\n' "$name:" "$dur"
    else
        printf '  %-12s FAIL (%ss)\n' "$name:" "$dur"
        FAILED=1
    fi
    return "$rc"
}

echo "🏗  verify"
run_step Build pio run -e xiao-esp32s3
if [ "$FAST" = 0 ] && [ "$FAILED" = 0 ]; then
    run_step Tests pio test -e native
fi

if [ "$FAILED" = 0 ]; then
    echo "✅ verify PASS"
    exit 0
fi
echo "❌ verify FAIL"
exit 1
