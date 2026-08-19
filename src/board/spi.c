/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI 外设初始化。SPI1 (W25Q128 NOR): APB2 84 MHz, 预分频 /2 = 42 MHz
 * (W25Q128 0x03 读命令上限 50 MHz); 软件 CS PA4 推挽、初始高。
 */

#include "spi.h"

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;

static void spi1_gpio_init(void)
{
    GPIO_InitTypeDef io = {0};

    /* 软件 CS: 推挽输出, 空闲高 (NOR 未选中) */
    io.Pin = GPIO_PIN_4;
    io.Mode = GPIO_MODE_OUTPUT_PP;
    io.Pull = GPIO_NOPULL;
    io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &io);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    /* PA5 SCK / PA6 MISO / PA7 MOSI, AF5 */
    io.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    io.Mode = GPIO_MODE_AF_PP;
    io.Pull = GPIO_NOPULL;
    io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    io.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &io);
}

void spi1_init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();    /* GPIOA 时钟 board_init 已开 */

    spi1_gpio_init();

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;      /* W25Q: 模式 0 */
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;   /* 84/2=42MHz */
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 10;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) { Error_Handler(); }
}

void spi2_init(void)
{
    /* 外设级配置 (APB1 42 MHz, /2 = 21 MHz); 引脚/使用方在任务 9 定义,
     * 届时补 GPIO AF 部分。当前无人调用。 */
    __HAL_RCC_SPI2_CLK_ENABLE();

    hspi2.Instance = SPI2;
    hspi2.Init.Mode = SPI_MODE_MASTER;
    hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi2.Init.NSS = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;   /* 42/2=21MHz */
    hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial = 10;
    if (HAL_SPI_Init(&hspi2) != HAL_OK) { Error_Handler(); }
}
