/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 轻量格式化: 仅支持 %s %c %d %u %x 与 0 填充宽度 (如 %08x),
 * 避免引入 newlib vfprintf/dtoa/malloc (~20KB)。boot 64KB 分区
 * 需给 bootutil + RSA 验签留空间。
 */

#include <stdarg.h>
#include <stddef.h>

#include "main.h"

#include "boot_uart.h"

#define BOOT_LOG_LINE_MAX 160u
#define BOOT_LOG_TMO_MS   100u

UART_HandleTypeDef huart1; /* app 侧 main.h 的 extern 在 boot 由本单元定义 */

static void put_str(char **p, char *end, const char *s)
{
    while (*s != '\0' && *p < end) {
        *(*p)++ = *s++;
    }
}

static void put_u32(char **p, char *end, uint32_t v, uint8_t base,
                    uint8_t pad, char pad_ch)
{
    char tmp[12];
    const char *hex = "0123456789abcdef";
    uint8_t n = 0;

    do {
        tmp[n++] = hex[v % base];
        v /= base;
    } while (v != 0);
    while (n < pad && *p < end) {
        *(*p)++ = pad_ch;
        pad--;
    }
    while (n > 0 && *p < end) {
        *(*p)++ = tmp[--n];
    }
}

void boot_uart_init(void)
{
    GPIO_InitTypeDef io = {0};

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    io.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    io.Mode = GPIO_MODE_AF_PP;
    io.Pull = GPIO_PULLUP;
    io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    io.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &io);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200u;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    (void)HAL_UART_Init(&huart1);
}

void boot_log(const char *fmt, ...)
{
    char buf[BOOT_LOG_LINE_MAX];
    char *p = buf;
    char *end = buf + sizeof(buf) - 3;
    va_list ap;

    va_start(ap, fmt);
    while (*fmt != '\0' && p < end) {
        uint8_t pad = 0;
        char pad_ch = ' ';

        if (*fmt != '%') {
            *p++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            pad_ch = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            pad = (uint8_t)(pad * 10u + (uint8_t)(*fmt++ - '0'));
        }
        switch (*fmt) {
        case 's':
            put_str(&p, end, va_arg(ap, const char *));
            break;
        case 'c':
            if (p < end) {
                *p++ = (char)va_arg(ap, int);
            }
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0 && p < end) {
                *p++ = '-';
                v = -v;
            }
            put_u32(&p, end, (uint32_t)v, 10, pad, pad_ch);
            break;
        }
        case 'u':
            put_u32(&p, end, va_arg(ap, uint32_t), 10, pad, pad_ch);
            break;
        case 'x':
            put_u32(&p, end, va_arg(ap, uint32_t), 16, pad, pad_ch);
            break;
        case '%':
            if (p < end) {
                *p++ = '%';
            }
            break;
        default:
            break;
        }
        fmt++;
    }
    va_end(ap);

    *p++ = '\r';
    *p++ = '\n';
    (void)HAL_UART_Transmit(&huart1, (const uint8_t *)buf,
                            (uint16_t)(p - buf), BOOT_LOG_TMO_MS);
}
