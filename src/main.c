/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * io-edge-hub 主入口 (Zephyr 版 main.c + settings/init.c + 各 SYS_INIT
 * 级初始化的合并对应物):
 *   - 初始化顺序 = 设计文档 §6.2 (配置先于使用者, 存储先于历史, 网络
 *     最后): HAL/时钟 -> OS/日志/RTC -> 存储链 (NOR -> config ->
 *     littlefs -> 历史) -> IO 采样 -> CAN (波特率/ID 启动快照) -> 网络
 *     (MAC=UID 派生, IP=holding_reg) -> Modbus/UDP 任务 -> IWDG -> 心跳
 *     任务 -> 调度器
 *   - MAC: STM32 96-bit UID 折叠 + Wiznet OUI 00:08:DC (Zephyr 版
 *     derive_mac_from_uid 逐式移植)
 *   - 失败降级不阻断启动: NOR 不在位 -> 出厂默认 + 历史停写; littlefs
 *     挂载失败 -> 历史静默丢弃; W5500 失败 -> 无链路空转本地功能
 *   - 心跳任务 = Zephyr 版 main 主循环: 喂狗 + 状态 LED 300/2700ms +
 *     延迟重启轮询 (sync -> 1s -> cold reboot)
 */

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"         /* HAL / huart1 / Error_Handler / UID_BASE */
#include "board.h"        /* board_init / STATUS_LED_* */
#include "fw_version.h"   /* FW_VERSION_* / FW_GIT_VERSION (构建期注入) */

#include "log.h"          /* log_init / LOG_* */
#include "io_hooks.h"     /* os_init / get_reboot_status / history_sync / io_reboot_cold */
#include "io_time.h"      /* io_time_init / io_now_epoch */
#include "io_watchdog.h"  /* watchdog_init / watchdog_feed */

#include "init.h"         /* holding 寄存器模型 / ip_addr_valid / holding_reg_load */
#include "config_store.h" /* config_store_init */
#include "w25qxx.h"       /* w25qxx_init / w25qxx_flash */
#include "lfs_port.h"     /* lfs_port_mount / lfs_t */
#include "history.h"      /* history_init */
#include "history_file.h" /* hist_file_set_clock */
#include "io.h"           /* dio_start / adc_start */
#include "io_can.h"       /* can_start / mod_can_send */
#include "w5500.h"        /* w5500_net_init / w5500_net_ready */
#include "w5500_macraw.h" /* w5500_macraw_init (LwIP MACRAW netif) */
#include "udp_cfg.h"      /* udp_cfg_start */
#include "wizchip_conf.h" /* getPHYCFGR / PHYCFGR_LNK_ON (boot 链路轮询) */

#include "lwip/tcpip.h"   /* tcpip_init */

/* CCM zeroing: LwIP memp pools are placed in CCM via LWIP_DECLARE_MEMORY_ALIGNED
 * section attribute. CCM has no startup initialization, so we zero it here.
 * Defined in the linker script: _sccmram / _eccmram */
extern uint32_t _sccmram, _eccmram;

/* Modbus 传输层启动入口 (src/modbus/tcp.c / rtu.c; target-only,
 * 仅 main 接线使用, 不单设头文件) */
extern void mb_tcp_start(void);
extern void mb_rtu_start(void);
/* Web 服务启动入口 (src/web/httpd.c, 同上) */
extern void web_httpd_start(void);

UART_HandleTypeDef huart1;

static StackType_t idle0_stack[configMINIMAL_STACK_SIZE];
static StaticTask_t idle0_tcb;
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &idle0_tcb;
    *ppxIdleTaskStackBuffer = idle0_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

static StackType_t tmr_stack[configTIMER_TASK_STACK_DEPTH];
static StaticTask_t tmr_tcb;
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &tmr_tcb;
    *ppxTimerTaskStackBuffer = tmr_stack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    NVIC_SystemReset(); /* 对齐 Zephyr k_sys_fatal_error_handler: 栈溢出 -> 热重启 */
}

/* 心跳 = Zephyr 版 main 主循环的对应物: 喂狗 + 状态 LED + 延迟重启轮询 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    GPIO_InitTypeDef io = {0};
    io.Pin = STATUS_LED_PIN; io.Mode = GPIO_MODE_OUTPUT_PP; io.Pull = GPIO_NOPULL; io.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STATUS_LED_PORT, &io);
    for (;;) {
        watchdog_feed(); /* 3s 周期 << 30s IWDG 窗口 */

        HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(300));
        HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_RESET);
        vTaskDelay(pdMS_TO_TICKS(2700));

        if (get_reboot_status()) { /* 对齐 Zephyr 主循环: sync 后延时重启 */
            LOG_INF("delayed reboot");
            history_sync();
            io_reboot_cold(); /* 内含 100ms 延时 */
        }
    }
}

/* 512 字 = 2048B: 心跳现承担喂狗 + 延迟重启 (history_sync -> littlefs
 * 落盘 + LOG 160B 格式化缓冲), 256 字对 FPU 上下文(132B)+vfprintf 深度
 * 余量不足 */
static StackType_t hb_stack[512];
static StaticTask_t hb_tcb;

/* boot 任务: 1024 字 = 4096B。全部子系统初始化在调度器启动后的本任务
 * 里执行 -- CM4F 端口的 uxCriticalNesting 在调度器启动前是 0xAAAAAAAA
 * 魔数 (port.c:161, 仅 xPortStartScheduler 清零 port.c:442), 预调度器
 * 阶段任何进出临界区的 FreeRTOS API (互斥锁创建/队列/日志锁) 都会让
 * vPortExitCritical 的 nesting==0 判断永假, BASEPRI 停在 0x50 --
 * TIM7 tick (优先级 15) 等中断被永久屏蔽, HAL_Delay 死循环。初始化
 * 挪到调度器后即恢复正常的临界区语义 (对齐 Zephyr 版 main-as-thread) */
static StackType_t boot_stack[1024];
static StaticTask_t boot_tcb;

/* littlefs 实例: boot 任务静态持有 (挂载后须永久有效), history_init 保存指针 */
static lfs_t lfs;

static time_t hist_clock_fn(void)
{
    return (time_t)io_now_epoch();
}

/* ================================================================
 * MAC 派生: STM32 96-bit UID 折叠为唯一 MAC (前 3B = Wiznet OUI)
 * (Zephyr main.c derive_mac_from_uid 逐式移植)
 * ================================================================ */
static void derive_mac_from_uid(uint8_t *mac)
{
    static const uint8_t oui[3] = {0x00, 0x08, 0xDC};
    const uint8_t *uid = (const uint8_t *)UID_BASE; /* 12 字节, 内存序 */

    mac[0] = oui[0];
    mac[1] = oui[1];
    mac[2] = oui[2];
    mac[3] = uid[0] ^ uid[3] ^ uid[6] ^ uid[9];
    mac[4] = uid[1] ^ uid[4] ^ uid[7] ^ uid[10];
    mac[5] = uid[2] ^ uid[5] ^ uid[8] ^ uid[11];
    /* Zephyr 版 hwinfo 读取失败回退 01:02:03; UID_BASE 为恒存在的唯一
     * ID 存储器映射, 无读取失败路径, 回退分支不再需要 */
}

/* ================================================================
 * 网络初始化 (Zephyr net_init 对应物): UID MAC + 静态 IP + 链路等待
 * ================================================================ */
static void net_setup(void)
{
    static const uint8_t mask[4] = {255, 255, 255, 0}; /* 掩码固定 /24 */
    static const uint8_t def_ip[4] = {192, 168, 12, 101};
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t gw[4];
    int i;

    /* 静态 IP: holding_reg 组装 (config_store 已加载); 非法值兜底默认 */
    ip[0] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET1_IDX);
    ip[1] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET2_IDX);
    ip[2] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET3_IDX);
    ip[3] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET4_IDX);
    if (!ip_addr_valid(ip[0], ip[1], ip[2], ip[3])) {
        LOG_WRN("invalid IP in holding regs, using default");
        for (i = 0; i < 4; i++) {
            ip[i] = def_ip[i];
        }
    }
    /* 网关 = IP 前三段 + 末段改 1 (Zephyr 语义) */
    gw[0] = ip[0];
    gw[1] = ip[1];
    gw[2] = ip[2];
    gw[3] = 1;

    derive_mac_from_uid(mac);
    if (w5500_net_init(mac, ip, mask, gw) != 0) {
        LOG_ERR("W5500 init failed, network unavailable");
        return; /* 降级: mbtcp/udpcfg 任务照常启动, 无链路空转 */
    }
    LOG_INF("MAC (UID): %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    LOG_INF("IP: %u.%u.%u.%u/24", ip[0], ip[1], ip[2], ip[3]);

    /* 链路等待 <=5s (Zephyr: k_sem_take(net_link_sem, 5s) 超时继续)。
     * 轮询 PHYCFGR.LNK, 100ms x 50 = 5s 上限, 超时 WRN 继续。
     * boot_task 上下文中调度器已运行, HAL_Delay 由 tick 驱动正常。 */
    for (i = 0; i < 50 && (getPHYCFGR() & PHYCFGR_LNK_ON) == 0; i++) {
        HAL_Delay(100);
    }
    if ((getPHYCFGR() & PHYCFGR_LNK_ON) == 0) {
        LOG_WRN("net link up timeout, continue anyway");
    }
}

/* boot 任务: 调度器启动后执行全部子系统初始化。
 * 原因: CM4F port 的 uxCriticalNesting 在调度器启动前是 0xAAAAAAAA
 * (port.c:161), 预调度器阶段任何进出临界区的 FreeRTOS API 都会
 * 让 vPortExitCritical 的 nesting==0 判断永假, BASEPRI 停在 0x50
 * 屏蔽 tick 中断, HAL_Delay 死循环。在调度器启动后执行初始化,
 * 临界区语义正常, BASEPRI 按预期在 nesting==0 时清零。 */
static void boot_task(void *arg)
{
    (void)arg;
    uint32_t now;

    os_init();       /* io_lock 互斥锁 */
    log_init();      /* USART1 日志最先就绪 */
    io_time_init();  /* LSE+RTC -> epoch 缓存 + 1Hz 定时器 */

    /* 开机 banner (对齐 Zephyr main: 构建时间/板名/容量/版本) */
    LOG_INF("build time: %s %s", __DATE__, __TIME__);
    LOG_INF("board: %s, clk: %dMHz", "io_edge_f407vet6",
            (int)(SystemCoreClock / 1000000u));
    LOG_INF("flash: %dKB, ram: %dKB", 512, 192);
    LOG_INF("version: v%d.%d.%d_%s", FW_VERSION_MAJOR, FW_VERSION_MINOR,
            FW_VERSION_PATCH, FW_GIT_VERSION);

    /* ---- 存储链: NOR -> config -> holding_reg -> littlefs -> 历史 ---- */
    if (w25qxx_init() != 0) {
        LOG_ERR("W25Qxx init failed");
    }

    if (config_store_init(w25qxx_flash()) != 0) {
        LOG_WRN("config store load failed, factory defaults");
    }

    holding_reg_load();


    /* 时间戳影子寄存器刷新 (Zephyr settings/init.c 的 load 后步骤) */
    now = io_now_epoch();
    update_holding_reg(HOLDING_TIMESTAMP_HI_IDX, (uint16_t)(now >> 16));
    update_holding_reg(HOLDING_TIMESTAMP_LO_IDX, (uint16_t)now);


    /* newlib time() 未接 RTC (恒 -1), 命名时钟注入 io_now_epoch */
    hist_file_set_clock(hist_clock_fn);

    if (lfs_port_mount(&lfs, w25qxx_flash()) == 0) {
        history_init(&lfs);
    } else {
        LOG_WRN("littlefs mount failed, history disabled");
        history_init(NULL);
    }

    /* settings 恢复后同步历史开关 */
    history_enable_write(get_holding_reg(HOLDING_HISTORY_ENABLE_IDX) != 0);

    /* ---- IO 采样 (holding_reg 已加载) ---- */
    dio_start();
    adc_start();
    can_start();


    /* ---- 网络 + 协议任务 ---- */
    net_setup();

    /* LwIP + MACRAW netif */
    /* Zero CCM region (LwIP memp pools reside here via section attribute).
     * CCM has no startup init; without this, pools contain garbage.
     * _sccmram/_eccmram are byte addresses from the linker script. */
    memset(&_sccmram, 0, (size_t)((uintptr_t)&_eccmram - (uintptr_t)&_sccmram));

    tcpip_init(NULL, NULL);
    {
        uint8_t mac[6];
        derive_mac_from_uid(mac);
        w5500_macraw_init(mac);
    }

    mb_tcp_start();
    mb_rtu_start();
    udp_cfg_start();
    web_httpd_start();


    LOG_INF("io-edge-hub ready");

    /* IWDG 30s: 放在存储链之后 -- 首次上电 littlefs 全片格式化可达
     * 分钟级, 不受 30s 窗口约束; 运行期历史写擦除才需要 lfs_port/
     * w25qxx 的事件型喂狗 */
    watchdog_init();


    vTaskDelete(NULL); /* boot 任务完成, 由 heartbeat 任务接管 */
}

int main(void)
{
    HAL_Init();      /* NVIC 优先级分组 + TIM7 tick (HAL_Delay 就绪) */
    board_init();    /* 时钟 168MHz + GPIO 端口时钟 */

    /* 所有 FreeRTOS API 调用挪到 boot_task, 在调度器启动后执行,
     * 避免 CM4F port 的 uxCriticalNesting 魔数导致 BASEPRI 永久屏蔽 */
    xTaskCreateStatic(boot_task, "boot", 1024, NULL, 6, boot_stack, &boot_tcb);
    xTaskCreateStatic(heartbeat_task, "hb", 512, NULL, 1, hb_stack, &hb_tcb);
    vTaskStartScheduler();
    for (;;) {}
}

void Error_Handler(void) { taskDISABLE_INTERRUPTS(); for (;;) {} }
