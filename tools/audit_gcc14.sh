#!/usr/bin/env bash
# GCC14 严格性审计: 把 GCC14 升级为硬错误的诊断类用 -Werror 提升,
# 并移除上游源的 -w 屏蔽, 全量编译收集所有会炸的点。
set -euo pipefail
cd /mnt/c/Users/jxwaz/code/io-edge-hub-freertos

rm -rf build-audit
cmake -S . -B build-audit -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake \
    -DSTM32_CUBE_F4_PATH=deps/STM32CubeF4 \
    -DFREERTOS_PATH=deps/FreeRTOS-Kernel >/dev/null

# 替换 -w 为 GCC14 硬错误等价的 -Werror 提升 (-w 位于 FLAGS 行尾,
# 需行尾锚点; 双模式保险)
PROMO='-Werror=implicit-function-declaration -Werror=implicit-int -Werror=int-conversion -Werror=incompatible-pointer-types'
sed -i "s/ -w\$/ $PROMO/" build-audit/build.ninja
sed -i "s/ -w / $PROMO /g" build-audit/build.ninja
echo "promoted sources: $(grep -c 'Werror=implicit-function-declaration' build-audit/build.ninja)"

set +e
cmake --build build-audit 2>&1 | grep -E 'error:' | sort -u | head -40
set -e
echo AUDIT-DONE
