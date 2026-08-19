#include "main.h"

TIM_HandleTypeDef htim7;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    __HAL_RCC_TIM7_CLK_ENABLE();
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    uint32_t apb1mul = (RCC->CFGR & RCC_CFGR_PPRE1) ? 2U : 1U; /* APB1>1 分频时定时器时钟 x2 */
    uint32_t timclk = pclk1 * apb1mul;                         /* 84MHz */
    htim7.Instance = TIM7;
    htim7.Init.Prescaler = (timclk / 1000000U) - 1U;            /* 1MHz */
    htim7.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim7.Init.Period = 1000U - 1U;                             /* 1ms */
    htim7.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    if (HAL_TIM_Base_Init(&htim7) != HAL_OK) { return HAL_ERROR; }
    if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) { return HAL_ERROR; }
    if (TickPriority < (1UL << __NVIC_PRIO_BITS)) {
        HAL_NVIC_SetPriority(TIM7_IRQn, TickPriority, 0U);
        HAL_NVIC_EnableIRQ(TIM7_IRQn);
    }
    return HAL_OK;
}

void HAL_SuspendTick(void) { __HAL_TIM_DISABLE_IT(&htim7, TIM_IT_UPDATE); }
void HAL_ResumeTick(void)  { __HAL_TIM_ENABLE_IT(&htim7, TIM_IT_UPDATE); }

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM7) { HAL_IncTick(); }
}

void TIM7_IRQHandler(void) { HAL_TIM_IRQHandler(&htim7); }
