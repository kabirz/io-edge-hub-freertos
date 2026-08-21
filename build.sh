#!/usr/bin/env bash
# ============================================================
#  Linux 拉取 + 编译 + 签名 (一键脚本)
#  用法: ./build.sh [--pull-only|--no-pull]
#    默认: 拉子模块 -> 配置 -> 编译 -> 有私钥则签名
#  依赖: git / cmake / ninja-build / python3 (签名另需 pip install imgtool)
#  工具链 (按优先级):
#    1. 系统 arm-none-eabi-gcc (默认; stm32-cmake 内置默认 /usr +
#       arm-none-eabi, 无需任何 -D 参数)
#       Ubuntu: sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi
#    2. 环境变量 STM32_TOOLCHAIN_PATH / STM32_TARGET_TRIPLET 显式指定
#       (如 xpack / Zephyr SDK 的非常规布局)
#    3. 本机 Zephyr SDK (探测兜底)
#    候选均须通过编译探针 (stm32-cmake 固定传 --sysroot=<root>/<triplet>,
#    新版 SDK 目录布局可能不兼容 -> nosys.specs 找不到, 探针能提前发现)
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

PULL=1
[ "${1:-}" = "--pull-only" ] && PULL=only
[ "${1:-}" = "--no-pull" ] && PULL=0

if [ "$PULL" != 0 ]; then
    echo "=== [1/3] submodules (selective, no --recursive) ==="
    git submodule update --init
    # 嵌套子模块须从其父仓库内寻址
    git -C deps/STM32CubeF4 submodule update --init \
        Drivers/STM32F4xx_HAL_Driver Drivers/CMSIS/Device/ST/STM32F4xx
    git -C deps/mcuboot submodule update --init ext/mbedtls
    [ "$PULL" = only ] && { echo OK; exit 0; }
fi

# ---- 工具链探测 ----
tc_probe() { # $1=root $2=triplet
    [ -x "$1/bin/$2-gcc" ] || return 1
    echo 'int main(void){return 0;}' \
        | "$1/bin/$2-gcc" --sysroot="$1/$2" -mthumb -mcpu=cortex-m4 \
              --specs=nosys.specs -x c - -o /dev/null 2>/dev/null
}

TC_FLAGS=()   # 传给 cmake 的 -D; 默认路径为空 = 用 stm32-cmake 内置默认
TC_CHOSEN=    # ""=未找到 / default=系统 arm-none-eabi / env / sdk

if [ -n "${STM32_TOOLCHAIN_PATH:-}" ]; then
    # 显式指定: 探针校验后透传给 cmake
    TRIPLET=${STM32_TARGET_TRIPLET:-arm-zephyr-eabi}
    tc_probe "$STM32_TOOLCHAIN_PATH" "$TRIPLET" \
        || { echo "*** 探针失败: $STM32_TOOLCHAIN_PATH 布局不兼容" >&2; exit 1; }
    TC_FLAGS=(-DSTM32_TOOLCHAIN_PATH="$STM32_TOOLCHAIN_PATH"
              -DSTM32_TARGET_TRIPLET="$TRIPLET")
    TC_CHOSEN=env
    echo "[toolchain] env override: $STM32_TOOLCHAIN_PATH ($TRIPLET)"
elif command -v arm-none-eabi-gcc >/dev/null 2>&1 \
        && tc_probe /usr arm-none-eabi; then
    # 系统 arm-none-eabi, stm32-cmake 默认即此, 不传任何 -D
    TC_CHOSEN=default
    echo "[toolchain] system arm-none-eabi (stm32-cmake defaults)"
else
    # Zephyr SDK 兜底 (新版本优先, 探针通过才采用)
    for cand in $(ls -d "$HOME"/zephyr-sdk-*/arm-zephyr-eabi 2>/dev/null |
                  sort -Vr); do
        if tc_probe "$cand" arm-zephyr-eabi; then
            TC_FLAGS=(-DSTM32_TOOLCHAIN_PATH="$cand"
                      -DSTM32_TARGET_TRIPLET=arm-zephyr-eabi)
            TC_CHOSEN=sdk
            echo "[toolchain] Zephyr SDK: $cand"
            break
        fi
        echo "[info] SDK $cand 布局不兼容, 跳过"
    done
fi
if [ -z "$TC_CHOSEN" ]; then
    echo "*** 未找到可用交叉工具链 (探针均失败):" >&2
    echo "    sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi" >&2
    echo "    或设置 STM32_TOOLCHAIN_PATH / STM32_TARGET_TRIPLET" >&2
    exit 1
fi

# Linux 与 Windows 不共用 build 目录 (CMake 缓存的工具链路径互斥)
BUILD_DIR=${BUILD_DIR:-build-linux}
echo "=== [2/3] configure + build ($BUILD_DIR) ==="
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
cmake -S . -B "$BUILD_DIR" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake \
    "${TC_FLAGS[@]}" \
    -DSTM32_CUBE_F4_PATH=deps/STM32CubeF4 \
    -DFREERTOS_PATH=deps/FreeRTOS-Kernel
cmake --build "$BUILD_DIR"

echo "=== [3/3] sign ==="
if [ ! -f tools/keys/root-rsa2048.pem ]; then
    echo "[skip] tools/keys/root-rsa2048.pem 不存在, 跳过签名"
    echo "       (私钥不入库; 从原开发机拷贝 tools/keys/ 目录后再签,"
    echo "        注意: 新生成的密钥与已部署设备的 keyhash 不匹配)"
else
    if ! BUILD_DIR="$BUILD_DIR" python3 tools/sign_fw.py; then
        echo "[warn] 签名失败 (build 产物完好; 通常是缺 imgtool: pip install imgtool)"
    else
        echo "产物: $BUILD_DIR/boot.hex $BUILD_DIR/fw.hex $BUILD_DIR/full.hex (全片烧录)"
    fi
fi
echo OK
