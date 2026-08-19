#ifndef STM32F4xx_HAL_CONF_H
#define STM32F4xx_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define HAL_CAN_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED
#define HAL_RTC_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED

#define HSE_VALUE    13000000U  /* 板上 13MHz 晶振 */
#define HSE_STARTUP_TIMEOUT 100U
#define LSE_VALUE    32768U
#define LSI_VALUE    32000U
#define LSE_STARTUP_TIMEOUT 5000U
#define LSI_STARTUP_TIMEOUT 100U  /* 若该宏在 CubeF4 中叫法不同，以 deps/STM32CubeF4 模板为准 */
#define HSI_VALUE    16000000U
#define EXTERNAL_CLOCK_VALUE 12288000U

#define VDD_VALUE                    3300U
#define TICK_INT_PRIORITY            15U
#define USE_RTOS                     0U
#define PREFETCH_ENABLE              1U
#define INSTRUCTION_CACHE_ENABLE     1U
#define DATA_CACHE_ENABLE            1U

#define USE_SPI_CRC                  0U

#include "stm32f4xx_hal_dma.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_rcc.h"
#include "stm32f4xx_hal_gpio.h"
#include "stm32f4xx_hal_cortex.h"
#include "stm32f4xx_hal_uart.h"
#include "stm32f4xx_hal_spi.h"
#include "stm32f4xx_hal_adc.h"
#include "stm32f4xx_hal_can.h"
#include "stm32f4xx_hal_iwdg.h"
#include "stm32f4xx_hal_rtc.h"
#include "stm32f4xx_hal_tim.h"
#include "stm32f4xx_hal_pwr.h"

#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

#endif /* STM32F4xx_HAL_CONF_H */
