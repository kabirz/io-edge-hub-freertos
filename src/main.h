#ifndef APP_MAIN_H
#define APP_MAIN_H
#include "stm32f4xx_hal.h"
extern UART_HandleTypeDef huart1;
void Error_Handler(void);
/* UID 折叠 MAC (00:08:DC OUI): main.c 定义, 网络/net_setup 与 shell
 * io info 共用 */
void derive_mac_from_uid(uint8_t *mac);
#endif
