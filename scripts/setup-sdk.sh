#!/usr/bin/env bash
#
# 克隆项目后初始化构建环境（复刻 DS5Dongle CI 的准备步骤）：
#   1. 初始化 lib/ 下的 submodule（pico-sdk 2.3.0、Pico-PIO-USB）
#   2. 将 pico-sdk 内嵌的 tinyusb 切到 DS5Dongle CI 指定的 commit
#      （该版本才有 UAC1/AUDIO10 支持；pico-sdk 自带的 0.18 只收 UAC2）
#
# 用法: ./scripts/setup-sdk.sh
set -euo pipefail

cd "$(dirname "$0")/.."

# --- 1. submodule ---
git submodule update --init --recursive

# --- 2. tinyusb 切到 DS5Dongle CI 的 commit ---
TINYUSB_REF=2d56dc533e45e4e91b15e93fdab5e22e964f328d
TINYUSB_DIR=lib/pico-sdk/lib/tinyusb

if [ "$(git -C "$TINYUSB_DIR" rev-parse HEAD)" = "$TINYUSB_REF" ]; then
    echo "tinyusb 已在目标 commit，跳过"
else
    if git -C "$TINYUSB_DIR" rev-parse -q --verify "$TINYUSB_REF^{commit}" >/dev/null; then
        TINYUSB_TARGET="$TINYUSB_REF"
    else
        git -C "$TINYUSB_DIR" fetch --depth 1 origin "$TINYUSB_REF"
        TINYUSB_TARGET=FETCH_HEAD
    fi
    git -C "$TINYUSB_DIR" checkout --detach "$TINYUSB_TARGET"
fi

echo "环境就绪: cmake -B build -S . && make -C build -j8"
