#ifndef __MAP_HW__MY_UART_H__
#define __MAP_HW__MY_UART_H__

#include <stdint.h>
#include <stdbool.h>

#define UART_CH_DEBUG   0
#define UART_CH_ESP8266 1
#define UART_CH_RPI4    2
#define UART_CH_MAX     3

// function
bool uartInit(void);
bool uartOpen(uint8_t ch, uint32_t baudrate);
bool uartClose(uint8_t ch);

uint32_t uartAvailable(uint8_t ch);

uint8_t uartRead(uint8_t ch);
bool uartReadBlock(uint8_t ch, uint8_t *p_data, uint32_t timeout);

uint32_t uartWrite(uint8_t ch, const uint8_t *p_data, uint32_t len);
bool uartWriteBlock(uint8_t ch, const uint8_t *p_data, uint32_t len);

uint32_t uartPrintf(uint8_t ch, const char *fmt, ...);

#endif //__MAP_HW__MY_UART_H__