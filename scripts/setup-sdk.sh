#!/usr/bin/env bash
#
# 克隆项目后初始化构建环境（复刻 DS5Dongle CI 的准备步骤）：
#   1. 初始化 SDK / hidkit 子模块
#   2. 将 pico-sdk 内嵌的 tinyusb 切到 DS5Dongle CI 指定的 commit
#      （该版本才有 UAC1/AUDIO10 支持；pico-sdk 自带的 0.18 只收 UAC2）
#   3. 固定 PIO-USB 热插拔基线并应用自愈等补丁
#
# 用法: ./scripts/setup-sdk.sh
set -euo pipefail

cd "$(dirname "$0")/.."

# --- 1. submodule ---
# PIO-USB 由 apply-patches.sh 单独管理，避免重复 setup 尝试覆盖补丁工作区。
git submodule update --init --recursive lib/pico-sdk lib/hidkit lib/hidkit-tusb-xinput lib/hidkit-tusb-switch-pro

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

./scripts/apply-patches.sh

echo "环境就绪: cmake -B build -S . && make -C build -j8"
