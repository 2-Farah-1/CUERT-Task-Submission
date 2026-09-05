#ifndef USB_UART_BRIDGE_H
#define USB_UART_BRIDGE_H

#include <stdint.h>

/* TEMPORARY: set to 0 to restore the transparent USB <-> UART bridge. */
#define USB_BRIDGE_SELF_TEST 0

void Bridge_Init(void);
void Bridge_Poll(void);
void Bridge_UsbInit(void);
void Bridge_UsbDeInit(void);
void Bridge_UsbReceive(const uint8_t *data, uint32_t length);

/* Inspect in a debugger; no diagnostic bytes are injected into the stream. */
extern volatile uint32_t bridge_uart_rx_dropped;
extern volatile uint32_t bridge_uart_errors;

#endif
