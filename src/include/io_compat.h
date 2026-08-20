#ifndef IO_COMPAT_H
#define IO_COMPAT_H

#include <time.h>

/* 主机单元测试跨编译器垫片: MSVC 无 POSIX gmtime_r, 映射到参数顺序相反的 gmtime_s。
 * 目标固件 (arm-zephyr-eabi newlib) 有原生 gmtime_r, 不经过此宏。 */
#if defined(_MSC_VER) && !defined(gmtime_r)
static inline struct tm *io_gmtime_r(const time_t *t, struct tm *tm_out)
{
	return gmtime_s(tm_out, t) == 0 ? tm_out : NULL;
}
#define gmtime_r(t, tm_out) io_gmtime_r((t), (tm_out))
#endif

/* 弱符号: MSVC 无对应机制, 直接普通定义 (host 测试只链接一份实现, 语义等价) */
#if defined(_MSC_VER)
#define IO_WEAK
#else
#define IO_WEAK __attribute__((weak))
#endif

#endif /* IO_COMPAT_H */
