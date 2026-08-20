/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 模拟输入: 4 路 ADC (ADC1 IN10-IN13, PC0-PC3)
 *   - AI0/AI1 (IN10/11): 电流 4-20mA, value = 7.414 * voltage / 10  (单位 0.01mA)
 *   - AI2/AI3 (IN12/13): 电压 0-10V,  value = 3.7037 * voltage / 10 (单位 0.01V)
 *
 * Zephyr 版 src/io/adc.c 的 FreeRTOS 移植:
 *   - 通道号/系数改硬编码自 overlay 引脚表 (io-channels 10-13 +
 *     ai-coeffs {7414,7414,3704,3704}, boards/io_edge_f407vet6.overlay)
 *   - 采样 = HAL_ADC 软件启动 + 轮询单次转换 (scan 关闭, 每通道独立,
 *     对齐 Zephyr 每通道独立 adc_read_dt 单次序列)
 *   - ADCCLK = PCLK2/4 = 21MHz (168MHz/2/4)。偏差记录: Zephyr overlay
 *     st,adc-prescaler=<2> 即 42MHz, 超 F407 数据手册 ADC 时钟上限
 *     (21/42MHz 档边界, 12bit 分辨率下规格值为 21MHz), 取 /4 更安全
 *   - 采样时间 144 周期 (对齐设计文档; 转换时间 (144+12)/21MHz ≈ 7.4us)
 *   - ai_convert 纯函数对齐 Zephyr 逐位语义, host 测试锁定
 *     (tests/test_adc_math.c); HAL/任务部分 #ifndef HOST_TEST 分离
 */

#include <stdint.h>

#include "init.h"
#include "io.h"

/* 工程量转换系数 (放大 1e4 倍做整数运算): Zephyr 版从 /zephyr,user 的
 * ai-coeffs 读取, 此处按 overlay 硬编码 (顺序 = 通道号 AI0-3) */
static const uint32_t ai_coeff[AI_NUM] = {7414, 7414, 3704, 3704};

/* 12-bit raw -> 工程量 (0.01mA / 0.01V); 两步整除语义与 Zephyr 版
 * adc.c:44 逐位一致 (voltage 先整除截断, 系数乘积 64 位再整除) */
uint16_t ai_convert(uint8_t ch, int32_t raw)
{
	int32_t voltage_mv = raw * 3300 / 4096; /* VREF = VDDA = 3.3V */
	uint32_t val = (uint64_t)ai_coeff[ch] * (uint32_t)voltage_mv / 10000U;

	return (uint16_t)val;
}

#ifndef HOST_TEST /* ================= 以下 target-only (固件构建) ================= */

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"

#include "history.h"
#include "io_hooks.h" /* io_now_epoch (Zephyr 版 (uint32_t)time(NULL)) */
#include "log.h"

/* 采样间隔上限 5s: 业务合理性约束 (远程调大时钳制, 防止采样响应过慢) */
#define SAMPLE_INTERVAL_MAX 5000U

#define ADC_TASK_STACK 256u /* 字 */
#define ADC_TASK_PRIO  6u

/* 轮询超时: 转换本身 (144+12)/21MHz ≈ 7.4us, 10ms 纯为异常兜底 */
#define ADC_POLL_TIMEOUT_MS 10u

ADC_HandleTypeDef hadc1;

/* 通道表: IN10-13 = PC0-PC3 = AI0-3 (顺序即通道号, 来自 overlay) */
static const uint32_t adc_chans[AI_NUM] = {
	ADC_CHANNEL_10, ADC_CHANNEL_11, ADC_CHANNEL_12, ADC_CHANNEL_13,
};

static StackType_t adc_stack[ADC_TASK_STACK];
static StaticTask_t adc_tcb;

/* ==================== 采样线程 ==================== */

static void adc_task(void *arg)
{
	(void)arg;

	while (1) {
		uint32_t si = get_holding_reg(HOLDING_AI_SAMPLE_MS_IDX);
		uint16_t en = get_holding_reg(HOLDING_AI_ENABLE_IDX);

		if (si < 10) {
			si = 10;
		} else if (si > SAMPLE_INTERVAL_MAX) {
			si = SAMPLE_INTERVAL_MAX;
		}

		for (int i = 0; i < AI_NUM; i++) {
			if (!(en & (1u << i))) {
				continue;
			}

			/* 单通道单次转换: scan 关闭, 每次转换前选通道
			 * (rank 1), 软件启动 + 轮询, 对齐 Zephyr 每通道
			 * 独立 adc_read_dt; 失败 (超时) 不更新该通道
			 * input_reg (保留上次值, Zephyr adc_read_dt != 0
			 * 同语义) */
			ADC_ChannelConfTypeDef cfg = {0};

			cfg.Channel = adc_chans[i];
			cfg.Rank = 1;
			cfg.SamplingTime = ADC_SAMPLETIME_144CYCLES;
			if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) {
				continue;
			}
			if (HAL_ADC_Start(&hadc1) != HAL_OK) {
				continue;
			}
			if (HAL_ADC_PollForConversion(&hadc1, ADC_POLL_TIMEOUT_MS) == HAL_OK) {
				update_input_reg(INPUT_AI0_IDX + i,
						 ai_convert(i, (int32_t)HAL_ADC_GetValue(&hadc1)));
			}
			(void)HAL_ADC_Stop(&hadc1);
		}

		/* 历史记录 (仅当有通道使能) */
		if (en & 0x000Fu) {
			struct his_data d = {0};

			d.type = AI_TYPE;
			d.timestamps = io_now_epoch();
			d.ai.ai_en_status = en & 0x000Fu;
			for (int i = 0; i < AI_NUM; i++) {
				d.ai.ai_value[i] = get_input_reg(INPUT_AI0_IDX + i);
			}
			send_history_data(&d);
		}

		vTaskDelay(pdMS_TO_TICKS(si));
	}
}

/* ==================== 初始化 ==================== */

/* PC0-PC3 模拟模式 (ADC 专用引脚, 无上下拉) */
static void adc_gpio_init(void)
{
	GPIO_InitTypeDef io = {0};

	io.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
	io.Mode = GPIO_MODE_ANALOG;
	io.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOC, &io);
}

void adc_start(void)
{
	__HAL_RCC_ADC1_CLK_ENABLE();

	/* 单次软件触发转换, 12bit 右对齐; ADCCLK = PCLK2(84MHz)/4 = 21MHz
	 * (偏差记录见文件头) */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.ScanConvMode = DISABLE;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc1.Init.ContinuousConvMode = DISABLE;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	hadc1.Init.DMAContinuousRequests = DISABLE;
	if (HAL_ADC_Init(&hadc1) != HAL_OK) {
		Error_Handler();
	}

	adc_gpio_init();

	xTaskCreateStatic(adc_task, "adc", ADC_TASK_STACK, NULL,
			  ADC_TASK_PRIO, adc_stack, &adc_tcb);
	LOG_INF("adc ready: %d channels", AI_NUM);
}

#endif /* !HOST_TEST */
