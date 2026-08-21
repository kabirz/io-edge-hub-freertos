#!/usr/bin/env bash
# ============================================================
#  Linux 拉取 + 编译 + 签名 (一键脚本)
#  用法: ./build.sh [--pull-only|--no-pull]
#    默认: 拉子模块 -> 配置 -> 编译 -> 有私钥则签名
#  依赖: git / cmake / ninja-build / python3 (签名另需 pip install imgtool)
#  工具链: 自动探测 (Zephyr SDK -> PATH 上的交叉 gcc); 也可用环境变量
#          STM32_TOOLCHAIN_PATH / STM32_TARGET_TRIPLET 显式指定
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
# 候选须通过编译探针 (stm32-cmake 固定传 --sysroot=<root>/<triplet>,
# 新版 SDK 目录布局可能不兼容 -> nosys.specs 找不到, 探针能提前发现)
tc_probe() { # $1=root $2=triplet
    [ -x "$1/bin/$2-gcc" ] || return 1
    echo 'int main(void){return 0;}' \
        | "$1/bin/$2-gcc" --sysroot="$1/$2" -mthumb -mcpu=cortex-m4 \
              --specs=nosys.specs -x c - -o /dev/null 2>/dev/null
}

TOOLCHAIN_PATH=${STM32_TOOLCHAIN_PATH:-}
TRIPLET=${STM32_TARGET_TRIPLET:-}
if [ -n "$TOOLCHAIN_PATH" ]; then
    tc_probe "$TOOLCHAIN_PATH" "${TRIPLET:-arm-zephyr-eabi}" \
        || { echo "*** 探针失败: $TOOLCHAIN_PATH 布局不兼容" >&2; exit 1; }
fi
if [ -z "$TOOLCHAIN_PATH" ]; then
    # 本机 Zephyr SDK, 新版本优先, 探针通过才采用
    for cand in $(ls -d "$HOME"/zephyr-sdk-*/arm-zephyr-eabi 2>/dev/null |
                  sort -Vr); do
        if tc_probe "$cand" arm-zephyr-eabi; then
            TOOLCHAIN_PATH=$cand
            TRIPLET=arm-zephyr-eabi
            break
        fi
        echo "[info] SDK $cand 布局不兼容, 跳过"
    done
fi
if [ -z "$TOOLCHAIN_PATH" ]; then
    for t in arm-zephyr-eabi arm-none-eabi; do
        if command -v "$t-gcc" >/dev/null 2>&1; then
            GCC_BIN=$(readlink -f "$(command -v "$t-gcc")")
            ROOT=$(dirname "$(dirname "$GCC_BIN")")
            if tc_probe "$ROOT" "$t"; then
                TOOLCHAIN_PATH=$ROOT
                TRIPLET=$t
                break
            fi
            echo "[info] $t-gcc (sysroot $ROOT/$t) 探针失败, 跳过"
        fi
    done
fi
if [ -z "$TOOLCHAIN_PATH" ]; then
    echo "*** 未找到可用交叉工具链 (探针均失败):" >&2
    echo "    安装 Zephyr SDK 0.16/0.17, 或设置 STM32_TOOLCHAIN_PATH /" >&2
    echo "    STM32_TARGET_TRIPLET 指向兼容布局的 arm-none-eabi GCC" >&2
    exit 1
fi

# Linux 与 Windows 不共用 build 目录 (CMake 缓存的工具链路径互斥)
BUILD_DIR=${BUILD_DIR:-build-linux}
echo "=== [2/3] configure + build ($BUILD_DIR, TOOLCHAIN=$TOOLCHAIN_PATH TRIPLET=$TRIPLET) ==="
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
cmake -S . -B "$BUILD_DIR" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake \
    -DSTM32_TOOLCHAIN_PATH="$TOOLCHAIN_PATH" \
    -DSTM32_TARGET_TRIPLET="$TRIPLET" \
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
