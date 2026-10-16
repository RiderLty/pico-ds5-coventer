#!/usr/bin/env bash
# 本地与 CI 共用的 Pico 2 固件构建入口。
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

usage() {
    cat <<'EOF'
用法：./build.sh [--bluetooth] [--skip-setup] [--build-dir DIR] [-- CMake 参数...]

默认初始化固定版本的依赖、应用 PIO-USB 补丁，并编译 Pico 2 Release 固件。
  --bluetooth      Pico 2 W 板载蓝牙输入，默认目录 build-bluetooth
  --skip-setup     跳过依赖准备（已有依赖、离线或增量构建时使用）
  --build-dir DIR  构建目录，默认 build
  -h, --help      显示帮助

环境变量：BUILD_TYPE（默认 Release）、JOBS（默认 CPU 核数）、
          TUSB_DEBUG_LEVEL（默认 0）、DEBUG_BAUDRATE（默认 4000000）。
示例：BUILD_TYPE=Debug ./build.sh --build-dir build-debug
EOF
}

BUILD_DIR=
BLUETOOTH=OFF
SKIP_SETUP=0
while (($#)); do
    case "$1" in
        --bluetooth)
            BLUETOOTH=ON
            shift
            ;;
        --skip-setup)
            SKIP_SETUP=1
            shift
            ;;
        --build-dir)
            if (($# < 2)) || [[ -z "$2" || "$2" == -* ]]; then
                echo "错误：--build-dir 需要目录参数" >&2
                exit 2
            fi
            BUILD_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "错误：未知参数 $1；使用 --help 查看用法" >&2
            exit 2
            ;;
    esac
done

BOARD=pico2
if [[ "$BLUETOOTH" == ON ]]; then
    BOARD=pico2_w
    BUILD_DIR="${BUILD_DIR:-build-bluetooth}"
else
    BUILD_DIR="${BUILD_DIR:-build}"
fi
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_board=$(sed -n 's/^PICO_BOARD:STRING=//p' "$BUILD_DIR/CMakeCache.txt")
    if [[ -n "$cached_board" && "$cached_board" != "$BOARD" ]]; then
        echo "错误：$BUILD_DIR 已用于 ${cached_board}；请使用独立 --build-dir" >&2
        exit 2
    fi
fi

BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
TUSB_DEBUG_LEVEL="${TUSB_DEBUG_LEVEL:-0}"
DEBUG_BAUDRATE="${DEBUG_BAUDRATE:-4000000}"
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "错误：JOBS 必须为正整数" >&2
    exit 2
fi
if [[ ! "$TUSB_DEBUG_LEVEL" =~ ^[0-3]$ ]]; then
    echo "错误：TUSB_DEBUG_LEVEL 必须为 0..3" >&2
    exit 2
fi

for tool in git cmake python3 arm-none-eabi-gcc arm-none-eabi-g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "错误：缺少构建工具 $tool" >&2
        exit 1
    fi
done

if [[ "$SKIP_SETUP" == 0 ]]; then
    ./scripts/setup-sdk.sh
fi

# 每次明确传入默认值，避免上次调试构建的 CMake 缓存影响发布固件。
cmake -S . -B "$BUILD_DIR" \
    -DPICO_BOARD="$BOARD" \
    -DENABLE_DS5_BT="$BLUETOOTH" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DTUSB_DEBUG_LEVEL="$TUSB_DEBUG_LEVEL" \
    -DDEBUG_BAUDRATE="$DEBUG_BAUDRATE" \
    "$@"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

python3 - "$BUILD_DIR/src/pico-ds5-coventer.uf2" <<'PY'
import hashlib
from pathlib import Path
import sys

firmware = Path(sys.argv[1])
print(f"固件：{firmware.resolve()}")
print(f"SHA-256：{hashlib.sha256(firmware.read_bytes()).hexdigest()}")
PY
