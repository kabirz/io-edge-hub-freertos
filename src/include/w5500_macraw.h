/*
 * W5500 MACRAW netif driver for LwIP.
 * Socket 0 in MACRAW mode, EXTI interrupt + counting semaphore for RX.
 *
 * RX path: EXTI IRQ -> xSemaphoreGive -> rx_task -> low_level_input -> tcpip_input
 * TX path: LwIP tcpip thread -> linkoutput -> low_level_output -> SPI send
 */

#ifndef W5500_MACRAW_H
#define W5500_MACRAW_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize W5500 MACRAW netif and start RX task.
 * Must be called after w5500_net_init() (SPI + ioLibrary callbacks ready).
 * Returns true on success. */
bool w5500_macraw_init(const uint8_t mac[6]);

/* Check if MACRAW netif is up (link + initialized) */
bool w5500_macraw_link_up(void);

#endif /* W5500_MACRAW_H */
