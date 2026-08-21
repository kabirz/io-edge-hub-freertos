/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot 引导占位主程序 (裸机, 无 RTOS)。T1 阶段: 时钟 + 外部 NOR
 * 探测 + 跳转 slot0 应用; T2 起接入 bootutil 验签/SWAP, T6 接入 CAN
 * 紧急升级。
 *
 * 上电即喂 IWDG: IWDG 一旦启动跨软复位持续计数 (仅上电复位清零),
 * app 会话启动过的 IWDG 在 boot 的 SWAP 期间必须继续喂。
 */

#include <stdint.h>

#include "main.h"

#include "board.h"
#include "flash_layout.h"
#include "fw_version.h"
#include "io_watchdog.h"
#include "w25qxx.h"

#include "boot_uart.h"

/* w25qxx.c 长擦除轮询会调用; boot 不启动 IWDG, 直接刷新计数器
 * (对未启动的 IWDG 写 KR 是无操作) */
void watchdog_feed(void)
{
    IWDG->KR = 0xAAAAu;
}

/* 跳转 slot0 应用: 关中断 -> 停 tick -> VTOR/MSP -> Reset_Handler。
 * app 的 SystemInit 以 VECT_TAB_OFFSET 重设 VTOR, 此处先设一次保证
 * 跳转指令流期间的取向量窗口正确 */
static void boot_jump_app(uint32_t sp, uint32_t pc)
{
    __disable_irq();
    SysTick->CTRL = 0;
    HAL_NVIC_DisableIRQ(TIM7_IRQn);
    HAL_NVIC_DisableIRQ(USART1_IRQn);

    /* 清 pending, 防止 app 早期使能中断时误入 */
    NVIC->ICPR[0] = 0xFFFFFFFFu;

    SCB->VTOR = APP_ADDR;
    __set_CONTROL(0); /* 特权 + MSP */
    __DSB();
    __ISB();
    __set_MSP(sp);
    ((void (*)(void))pc)();
    NVIC_SystemReset(); /* 不可达: 跳转后 MSP 已切换 */
}

static void boot_try_boot(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_ADDR;
    uint32_t pc = *(volatile uint32_t *)(APP_ADDR + 4u);

    /* 栈顶落在主 SRAM 且入口在 slot0 地址范围内视为镜像存在 */
    if (sp >= 0x20000000u && sp <= 0x20020000u &&
        pc >= APP_ADDR && pc < APP_ADDR + SLOT0_SIZE && (pc & 1u) != 0u) {
        boot_log("boot: jump app @%08x", (unsigned)pc);
        boot_jump_app(sp, pc); /* noreturn */
    }
    boot_log("boot: no valid app in slot0");
}

int main(void)
{
    HAL_Init();   /* NVIC 分组 + TIM7 tick (HAL_Delay 就绪) */
    board_init(); /* 168MHz 时钟 + GPIO 端口时钟 */
    boot_uart_init();

    watchdog_feed();
    boot_log("io-edge-hub boot v%d.%d.%d_%s", FW_VERSION_MAJOR,
             FW_VERSION_MINOR, FW_VERSION_PATCH, FW_GIT_VERSION);

    if (w25qxx_init() != 0) {
        boot_log("boot: W25Q128 not present");
    } else {
        boot_log("boot: NOR ok");
    }

    boot_try_boot();

    /* 无有效 app: 常驻等待 (T6 起 = CAN 紧急升级救援循环) */
    for (;;) {
        watchdog_feed();
        HAL_Delay(200);
    }
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        watchdog_feed();
    }
}
