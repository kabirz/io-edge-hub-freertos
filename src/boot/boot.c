/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot 引导主程序 (裸机, 无 RTOS): bootutil 验签 + SWAP_SCRATCH +
 * 跳转 slot0 应用。T6 起接入 CAN 紧急升级救援循环。
 *
 * 上电即喂 IWDG: IWDG 一旦启动跨软复位持续计数 (仅上电复位清零),
 * app 会话启动过的 IWDG 在 boot 的 SWAP 期间必须继续喂
 * (bootutil 经 MCUBOOT_WATCHDOG_FEED 逐块喂)。
 */

#include <stdint.h>

#include "main.h"

#include "board.h"
#include "boot_can.h"
#include "flash_layout.h"
#include "fw_version.h"
#include "io_watchdog.h"
#include "w25qxx.h"

#include "boot_uart.h"

#include "bootutil/bootutil.h"
#include "mbedtls/memory_buffer_alloc.h"

/* mbedTLS 静态内存池 (boot 域无 newlib malloc): RSA-2048 验签峰值
 * 约 2KB mpi 临时量, 8KB 留足裕量 */
static unsigned char mbed_arena[8192];

/* w25qxx.c 长擦除轮询会调用; boot 不启动 IWDG, 直接刷新计数器
 * (对未启动的 IWDG 写 KR 是无操作) */
void watchdog_feed(void)
{
    IWDG->KR = 0xAAAAu;
}

/* MCUboot assert 失败出口 (默认 newlib abort() 无声死循环): 报行号
 * (对应 bootutil 源文件, 单文件粒度足够定位) 后停机喂狗 */
void mcuboot_assert_fail(int line)
{
    boot_log("mcuboot ASSERT L%d", line);
    for (;;) {
        watchdog_feed();
    }
}

/* 跳转应用: 关中断 -> 停 tick -> VTOR/MSP -> Reset_Handler。
 * vec = 应用向量表地址 (镜像头之后), sp/pc 取前两项 */
static void boot_jump_vec(uint32_t vec)
{
    uint32_t sp = *(volatile uint32_t *)vec;
    uint32_t pc = *(volatile uint32_t *)(vec + 4u);

    __disable_irq();
    SysTick->CTRL = 0;
    HAL_NVIC_DisableIRQ(TIM7_IRQn);
    HAL_NVIC_DisableIRQ(USART1_IRQn);

    /* 清 pending, 防止 app 早期使能中断时误入 */
    NVIC->ICPR[0] = 0xFFFFFFFFu;

    SCB->VTOR = vec;
    __set_CONTROL(0); /* 特权 + MSP */
    __DSB();
    __ISB();
    __set_MSP(sp);
    ((void (*)(void))pc)();
    NVIC_SystemReset(); /* 不可达: 跳转后 MSP 已切换 */
}

int main(void)
{
    struct boot_rsp rsp;

    HAL_Init();   /* NVIC 分组 + TIM7 tick (HAL_Delay 就绪) */
    board_init(); /* 168MHz 时钟 + GPIO 端口时钟 */
    boot_uart_init();

    watchdog_feed();
    boot_log("io-edge-hub boot v%d.%d.%d_%s", FW_VERSION_MAJOR,
             FW_VERSION_MINOR, FW_VERSION_PATCH, FW_GIT_VERSION);
    mbedtls_memory_buffer_alloc_init(mbed_arena, sizeof(mbed_arena));

    /* 外部 NOR (slot1/scratch 所在); 失败仅失去升级能力, slot0 仍可启动 */
    if (w25qxx_init() != 0) {
        boot_log("boot: W25Q128 not present");
    } else {
        boot_log("boot: NOR ok");
    }

    /* CAN 紧急救援: 500ms 探测窗口, 上位机应答则进入 slot0 直写会话
     * (CONFIRM 后本会话 boot_go 直接验签启动, 无 swap 标记) */
    boot_can_wait(false);

    /* MCUboot: 验签 slot0 + 检查 slot1 升级请求 (SWAP_SCRATCH) */
    if (boot_go(&rsp) == 0) {
        uint32_t vec = rsp.br_image_off + rsp.br_hdr->ih_hdr_size;

        boot_log("boot: img@%08x v%d.%d.%d", (unsigned)vec,
                 rsp.br_hdr->ih_ver.iv_major, rsp.br_hdr->ih_ver.iv_minor,
                 rsp.br_hdr->ih_ver.iv_revision);
        boot_jump_vec(vec);
    }

    boot_log("boot: no valid image");

    /* 无有效镜像: CAN 救援循环 (持续探测, 会话 CONFIRM 后复位重启) */
    for (;;) {
        watchdog_feed();
        boot_can_wait(true);
        if (boot_rescue_done()) {
            boot_log("boot: rescue image installed, rebooting");
            HAL_Delay(200);
            NVIC_SystemReset();
        }
        boot_log("boot: still no valid image");
    }
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        watchdog_feed();
    }
}
