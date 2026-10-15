#!/usr/bin/env bash
# PIO-USB 基线及本地补丁，移植自 pico-hid-mapper。重复运行不会覆盖本地修改。
# 用法：./scripts/apply-patches.sh [--status]
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIO_DIR="$PROJECT_ROOT/lib/pico_pio_usb"
PIO_REF=9510f795853b468d6a1fdf12dd582b39eddee436
MODE="${1:-apply}"
case "$MODE" in
    apply|--status) ;;
    *) echo "用法：$0 [--status]" >&2; exit 2 ;;
esac

if [ ! -f "$PIO_DIR/.git" ] && [ ! -d "$PIO_DIR/.git" ]; then
    if [ "$MODE" = "--status" ]; then
        echo "PIO-USB 子模块未初始化" >&2
        exit 1
    fi
    git -C "$PROJECT_ROOT" submodule update --init lib/pico_pio_usb
fi

if [ "$(git -C "$PIO_DIR" rev-parse HEAD)" != "$PIO_REF" ]; then
    if [ "$MODE" = "--status" ]; then
        echo "PIO-USB 基线不符，需要 $PIO_REF；运行 ./scripts/apply-patches.sh" >&2
        exit 1
    fi
    if [ -n "$(git -C "$PIO_DIR" status --porcelain)" ]; then
        echo "PIO-USB 有本地修改，无法切换基线；请先保存修改。" >&2
        exit 1
    fi
    if ! git -C "$PIO_DIR" cat-file -e "$PIO_REF^{commit}" 2>/dev/null; then
        git -C "$PIO_DIR" fetch origin "$PIO_REF"
    fi
    git -C "$PIO_DIR" checkout --detach "$PIO_REF"
fi

failed=0
for patch in "$PROJECT_ROOT"/patches/pio_usb/*.patch; do
    name="$(basename "$patch")"
    if git -C "$PIO_DIR" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "[已应用] $name"
    elif git -C "$PIO_DIR" apply --check "$patch" >/dev/null 2>&1; then
        if [ "$MODE" = "--status" ]; then
            echo "[未应用] $name"
            failed=1
        else
            git -C "$PIO_DIR" apply "$patch"
            echo "[已打上] $name"
        fi
    else
        echo "[冲突] ${name}（请检查 PIO-USB 本地修改）" >&2
        failed=1
    fi
done
exit "$failed"
