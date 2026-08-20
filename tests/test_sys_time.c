/*
 * ts_in_range 纯函数边界 (Task 13): 合法范围 [946684800, 4102444800)
 * = [2000-01-01, 2100-01-01) UTC。set_timestamp 的范围门即本函数,
 * RTC/HAL 写路径为 target-only (固件构建验证), host 只测纯逻辑。
 */

#include <time.h>

#include "test_util.h"
#include "io_time.h"

int main(void)
{
	/* 端点常量自证: 恰为 2000/2100-01-01 00:00:00 UTC (防手滑改错) */
	struct tm tm;
	time_t t;

	t = 946684800;
	gmtime_r(&t, &tm);
	TEST_EQ_INT(tm.tm_year + 1900, 2000);
	TEST_EQ_INT(tm.tm_mon, 0);
	TEST_EQ_INT(tm.tm_mday, 1);
	t = 4102444800;
	gmtime_r(&t, &tm);
	TEST_EQ_INT(tm.tm_year + 1900, 2100);
	TEST_EQ_INT(tm.tm_mon, 0);
	TEST_EQ_INT(tm.tm_mday, 1);

	/* 下边界: [2000-01-01 起合法, 之前 (含 0/负值) 拒绝 */
	TEST_ASSERT(!ts_in_range(0));
	TEST_ASSERT(!ts_in_range(-1));
	TEST_ASSERT(!ts_in_range(946684799));
	TEST_ASSERT(ts_in_range(946684800));

	/* 上边界: 闭区间, 2100-01-01 00:00:00 本身合法, +1s 拒绝 */
	TEST_ASSERT(ts_in_range(4102444800));
	TEST_ASSERT(!ts_in_range(4102444801));

	/* 任务书 set_timestamp 用例的纯函数等价判定 */
	TEST_ASSERT(ts_in_range(1600000000));   /* 2020-09-13 */
	TEST_ASSERT(!ts_in_range(5000000000LL)); /* > 2100 年 */

	TEST_MAIN_END();
}
