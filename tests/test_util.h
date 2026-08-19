#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
static int t_failures = 0;
#define TEST_ASSERT(cond) do { if (!(cond)) { t_failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define TEST_EQ_INT(a, b) TEST_ASSERT((long long)(a) == (long long)(b))
#define TEST_EQ_MEM(a, b, n) TEST_ASSERT(memcmp((a), (b), (n)) == 0)
#define TEST_MAIN_END() do { printf("%s: %s (%d failures)\n", __func__, \
    t_failures ? "FAIL" : "PASS", t_failures); return t_failures ? 1 : 0; } while (0)
#endif
