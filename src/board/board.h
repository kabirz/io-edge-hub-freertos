#ifndef APP_BOARD_H
#define APP_BOARD_H
#include "main.h"

#define STATUS_LED_PORT  GPIOE
#define STATUS_LED_PIN   GPIO_PIN_7

void board_init(void);
void heartbeat_start(void);
#endif
