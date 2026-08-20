/*
 * ai_convert 整数换算语义 (Task 14): 必须与 Zephyr 版 adc.c 逐位一致 ——
 *   voltage_mv = raw * 3300 / 4096            (32 位整除, 先截断)
 *   val        = coeff * voltage_mv / 10000   (64 位中间量, 再整除)
 * 两步整除不可交换/不可合并 (合并成一次乘除会得到不同结果)。
 *
 * 期望值手算 (coeff = {7414, 7414, 3704, 3704}):
 *   raw=4095: 4095*3300 = 13,513,500; /4096 -> 3299 (3299*4096=13,512,704)
 *     ch0: 7414*3299 = 24,458,786; /10000 -> 2445
 *     ch2: 3704*3299 = 12,219,496; /10000 -> 1221
 *   raw=2048: 2048*3300 = 6,758,400; /4096 -> 1650 (整除无余)
 *     ch0: 7414*1650 = 12,233,100; /10000 -> 1223
 *     ch2: 3704*1650 =  6,111,600; /10000 ->  611
 * 注: 任务书行内估算值 (2449/2446/≈1222) 混入了浮点近似, 以此处按
 * C 整数语义逐位手算为准 (见 task-14-report.md)。
 */

#include "test_util.h"
#include "io.h"

int main(void)
{
	/* 满量程 4095: 电流通道 (x1.0 coeff 7414, 单位 0.01mA) */
	TEST_EQ_INT(ai_convert(0, 4095), 2445);
	TEST_EQ_INT(ai_convert(1, 4095), 2445);
	/* 满量程 4095: 电压通道 (coeff 3704, 单位 0.01V) */
	TEST_EQ_INT(ai_convert(2, 4095), 1221);
	TEST_EQ_INT(ai_convert(3, 4095), 1221);

	/* 零点 */
	TEST_EQ_INT(ai_convert(0, 0), 0);
	TEST_EQ_INT(ai_convert(2, 0), 0);

	/* 中点 2048: 电压换算恰整除 (1650mV), 系数步进可见 */
	TEST_EQ_INT(ai_convert(0, 2048), 1223);
	TEST_EQ_INT(ai_convert(2, 2048), 611);

	/* 最低有效位以下: 1*3300/4096 = 0 -> 换算为 0 (整除截断) */
	TEST_EQ_INT(ai_convert(0, 1), 0);
	TEST_EQ_INT(ai_convert(3, 1), 0);

	TEST_MAIN_END();
}
