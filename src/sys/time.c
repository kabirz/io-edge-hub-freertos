/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTC 时间管理 (STM32 内部 RTC, LSE 驱动) — Zephyr 版 src/sys/time.c 的
 * FreeRTOS 移植:
 *   - 启动时从 RTC 读取时间装入 epoch 缓存 (Zephyr 版为 clock_settime)
 *   - set_timestamp(): Modbus 0x0E/0x0F 或 UDP SET_TIME 写 RTC (BIN) +
 *     缓存; Zephyr 版为 void, 本版返回 bool (UDP 路径需判 ok)
 *   - 运行期缓存由 1Hz FreeRTOS 软件定时器递增 (对齐 Zephyr 系统时钟
 *     自走; RTC 本体只在设时间时写, 读路径无锁 -- u32 读原子)
 *
 * host 测试 (HOST_TEST) 只编译纯函数 ts_in_range。
 */

#include <stdbool.h>
#include <time.h>

#include "io_time.h"

/* 合法时间戳范围: [2000-01-01 00:00:00, 2100-01-01 00:00:00] 闭区间
 * (Zephyr 版为半开 t < TS_MAX, 任务书边界锁定上端点含入 -- 差异仅
 * 2100-01-01 00:00:00 整这一秒)。超出范围的值通常是主站写入错误
 * (如只写低 16 位、高字为 0 -> 1970 年), gmtime 对部分非法输入会
 * 返回 NULL, 解引用将触发 HardFault。这里直接拒绝越界值。 */
#define TS_MIN 946684800U  /* 2000-01-01 00:00:00 UTC */
#define TS_MAX 4102444800U /* 2100-01-01 00:00:00 UTC */

bool ts_in_range(time_t t)
{
	return t >= (time_t)TS_MIN && t <= (time_t)TS_MAX;
}

#ifndef HOST_TEST
/* ==================== target-only: RTC + epoch 缓存 ==================== */

#include <stdint.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "main.h"
#include "log.h"

/* 备份域标志 "RTC 已写入过默认时间": 无标志 = 备份域首次上电 (或 VBAT
 * 掉电), RTC 日期不可信, 写默认值; 有标志 = RTC 由 VBAT 维持, 保留时间。
 * (CubeMX 惯例是 BKUP DR0 写 0x32F2, 此处用同寄存器自定义魔数) */
#define RTC_BKP_MAGIC 0x54494D45u /* "TIME" */

static RTC_HandleTypeDef hrtc;

/* epoch 缓存 (u32): 定时器服务任务写, 多任务读 */
static volatile uint32_t epoch_cache;

static StaticTimer_t time_tick_tcb;
static TimerHandle_t time_tick_timer;

static void time_tick_cb(TimerHandle_t timer)
{
	(void)timer;
	epoch_cache++;
}

uint32_t io_now_epoch(void)
{
	return epoch_cache; /* u32 对齐读原子 */
}

bool set_timestamp(time_t t)
{
	struct tm tm;
	RTC_TimeTypeDef rtc_t = {0};
	RTC_DateTypeDef rtc_d = {0};

	if (!ts_in_range(t)) {
		LOG_WRN("invalid timestamp %lld, ignored", (long long)t);
		return false;
	}
	if (gmtime_r(&t, &tm) == NULL) {
		LOG_WRN("gmtime failed for %lld", (long long)t);
		return false;
	}

	rtc_t.Hours = (uint8_t)tm.tm_hour; /* HourFormat24, 无 AM/PM */
	rtc_t.Minutes = (uint8_t)tm.tm_min;
	rtc_t.Seconds = (uint8_t)tm.tm_sec;
	rtc_d.WeekDay = (uint8_t)(tm.tm_wday == 0 ? 7 : tm.tm_wday); /* 1-7, 周一=1 */
	rtc_d.Month = (uint8_t)(tm.tm_mon + 1);
	rtc_d.Date = (uint8_t)tm.tm_mday;
	rtc_d.Year = (uint8_t)(tm.tm_year - 100); /* RTC 年 = 2000+yy */

	if (HAL_RTC_SetTime(&hrtc, &rtc_t, RTC_FORMAT_BIN) != HAL_OK ||
	    HAL_RTC_SetDate(&hrtc, &rtc_d, RTC_FORMAT_BIN) != HAL_OK) {
		LOG_WRN("rtc set failed");
		return false;
	}

	/* Zephyr 版: rtc_set_time 后 clock_settime; 本版直接更新缓存
	 * (1Hz 定时器在新基准上继续递增) */
	epoch_cache = (uint32_t)t;
	LOG_INF("time set: %lld", (long long)t);
	return true;
}

/* RTC -> epoch。HAL 约束: GetTime 后必须紧跟 GetDate 才能解锁影子
 * 寄存器, 否则下次 GetTime 挂在 RSF 上。失败 (未初始化/mktime 越界/
 * 范围门外) 返回 false。 */
static bool rtc_read_epoch(uint32_t *out)
{
	RTC_TimeTypeDef rtc_t;
	RTC_DateTypeDef rtc_d;
	struct tm tm = {0};
	time_t t;

	if (HAL_RTC_GetTime(&hrtc, &rtc_t, RTC_FORMAT_BIN) != HAL_OK ||
	    HAL_RTC_GetDate(&hrtc, &rtc_d, RTC_FORMAT_BIN) != HAL_OK) {
		return false;
	}

	tm.tm_hour = rtc_t.Hours;
	tm.tm_min = rtc_t.Minutes;
	tm.tm_sec = rtc_t.Seconds;
	tm.tm_mday = rtc_d.Date;
	tm.tm_mon = rtc_d.Month - 1;
	tm.tm_year = rtc_d.Year + 100; /* 2000+yy -> 1900+(yy+100) */
	tm.tm_isdst = 0;		     /* RTC 存储 UTC, 无夏令时 */

	t = mktime(&tm);
	if (t == (time_t)-1 || !ts_in_range(t)) {
		return false;
	}
	*out = (uint32_t)t;
	return true;
}

void io_time_init(void)
{
	RCC_OscInitTypeDef osc = {0};
	uint32_t epoch = 0;

	/* RTC/备份寄存器在备份域: PWR 时钟 + DBP 位放开写访问 */
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();

	/* RTC 时钟源不对 (如出厂 LSI/未配置) 才复位备份域: 清旧配置换 LSE;
	 * 已是 LSE 则绝不复位 -- 保留 VBAT 维持的时间和备份标志。
	 * (复位须在 LSE 起振前: 备份域复位会连带关 LSE) */
	if (__HAL_RCC_GET_RTC_SOURCE() != RCC_RTCCLKSOURCE_LSE) {
		__HAL_RCC_BACKUPRESET_FORCE();
		__HAL_RCC_BACKUPRESET_RELEASE();
	}

	/* LSE 起振 (外部 32.768kHz 晶振; 起振可慢至秒级, HAL 超时
	 * LSE_STARTUP_TIMEOUT=5s)。失败 (无晶振/损坏): 放弃 RTC, epoch
	 * 留 0 (对齐 Zephyr RTC 不可用时时间停在 0 的语义), 不阻断启动 */
	osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
	osc.LSEState = RCC_LSE_ON;
	osc.PLL.PLLState = RCC_PLL_NONE; /* 其余振荡器/PLL 保持 board_init 现状 */
	if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
		LOG_ERR("LSE startup failed, RTC unavailable");
		return;
	}
	__HAL_RCC_RTC_CONFIG(RCC_RTCCLKSOURCE_LSE);
	__HAL_RCC_RTC_ENABLE();

	/* 日历未初始化时配 24h 格式 + 1Hz 分频 (LSE 32768 = (127+1)*(255+1));
	 * 已初始化 (VBAT 维持) 则整体跳过 (HAL_RTC_Init 的 INITS 检查) */
	hrtc.Instance = RTC;
	hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
	hrtc.Init.AsynchPrediv = 127;
	hrtc.Init.SynchPrediv = 255;
	if (HAL_RTC_Init(&hrtc) != HAL_OK) {
		LOG_ERR("RTC init failed");
		return;
	}

	/* 备份域无标志 (首次上电): 写默认 2020-01-01 00:00:00 (周三),
	 * 读路径 (下方) 随即把它装入缓存; 之后 RTC 由 VBAT 维持 */
	if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_BKP_MAGIC) {
		RTC_TimeTypeDef rtc_t = {0};
		RTC_DateTypeDef rtc_d = {0};

		rtc_d.WeekDay = RTC_WEEKDAY_WEDNESDAY;
		rtc_d.Month = RTC_MONTH_JANUARY;
		rtc_d.Date = 1;
		rtc_d.Year = 20;
		if (HAL_RTC_SetTime(&hrtc, &rtc_t, RTC_FORMAT_BIN) == HAL_OK &&
		    HAL_RTC_SetDate(&hrtc, &rtc_d, RTC_FORMAT_BIN) == HAL_OK) {
			HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_BKP_MAGIC);
		}
	}

	/* RTC -> 缓存 (无效日期 -> epoch 0) */
	if (!rtc_read_epoch(&epoch)) {
		epoch = 0;
	}
	epoch_cache = epoch;

	/* 1Hz 软件定时器递增缓存: 本函数在 main 建任务前调用, xTimerStart
	 * 入队 (tmrNO_DELAY) 由调度器启动后的定时器服务任务执行 */
	time_tick_timer = xTimerCreateStatic("time1s", pdMS_TO_TICKS(1000),
					     pdTRUE, NULL, time_tick_cb,
					     &time_tick_tcb);
	if (time_tick_timer == NULL) {
		Error_Handler();
	}
	(void)xTimerStart(time_tick_timer, 0);

	LOG_INF("RTC epoch restored: %u", (unsigned)epoch);
}
#endif /* !HOST_TEST */
