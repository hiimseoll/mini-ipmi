#include "my_uart.h"

#include <stdio.h>
#include <stdarg.h>

#include "cmsis_os2.h"
#include "usart.h"

#define UART_TX_TIMEOUT 200
#define UART_RX_BUF_LENGTH 256

// STM32CubeMX에서 생성된 UART 핸들 외부 참조
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart6;

typedef struct {
    UART_HandleTypeDef *huart;
    USART_TypeDef *instance;

    osMessageQueueId_t rx_q;
    osMutexId_t tx_mutex;

    uint8_t rx_data;
} uart_tbl_t;

// inner
static uart_tbl_t uart_tbl[UART_CH_MAX] = {
    [UART_CH_DEBUG] = {
        .huart = &huart2,
        .instance = USART2,
        .rx_q = NULL,
        .tx_mutex = NULL,
        .rx_data = 0
    },

    [UART_CH_ESP8266] = {
        .huart = &huart6,
        .instance = USART6,
        .rx_q = NULL,
        .tx_mutex = NULL,
        .rx_data = 0
    },

    // [추가] RPI4 화면 수신용 채널 (USART1)
    [UART_CH_RPI4] = {
        .huart = &huart1,
        .instance = USART1,
        .rx_q = NULL,
        .tx_mutex = NULL,
        .rx_data = 0
    }
};

static bool uartIsValidCh(uint8_t ch) {
    if (ch >= UART_CH_MAX) return false;
    if (uart_tbl[ch].huart == NULL) return false;

    return true;
}

static void uartFlushRx(uint8_t ch) {
    uint8_t dummy;

    if (uartIsValidCh(ch) == false) return;
    if (uart_tbl[ch].rx_q == NULL) return;

    while (osMessageQueueGet(uart_tbl[ch].rx_q, &dummy, NULL, 0) == osOK) {
        ;
    }
}

// function
bool uartInit(void) {
    for (uint8_t i = 0; i < UART_CH_MAX; i++) {
        if (uart_tbl[i].rx_q == NULL) {
            uart_tbl[i].rx_q = osMessageQueueNew(UART_RX_BUF_LENGTH, sizeof(uint8_t), NULL);
        }

        if (uart_tbl[i].tx_mutex == NULL) uart_tbl[i].tx_mutex = osMutexNew(NULL);
        if (uart_tbl[i].rx_q == NULL || uart_tbl[i].tx_mutex == NULL) return false;
    }

    // 초기화 및 통신 속도 설정
    if (uartOpen(UART_CH_DEBUG, 9600) == false) return false;
    if (uartOpen(UART_CH_ESP8266, 115200) == false) return false;
    if (uartOpen(UART_CH_RPI4, 115200) == false) return false;

    return true;
}

bool uartOpen(uint8_t ch, uint32_t baudrate) {
    UART_HandleTypeDef *huart;

    if (uartIsValidCh(ch) == false) return false;

    huart = uart_tbl[ch].huart;

    HAL_UART_AbortReceive_IT(huart);

    if (huart->Init.BaudRate != baudrate) huart->Init.BaudRate = baudrate;

    if (HAL_UART_DeInit(huart) != HAL_OK) return false;
    if (HAL_UART_Init(huart) != HAL_OK) return false;

    uartFlushRx(ch);

    // [예외 처리] RPI4(USART1)는 ap.c에서 DMA IDLE로 별도 수신하므로 1바이트 IT를 켜지 않음
    if (ch != UART_CH_RPI4) {
        if (HAL_UART_Receive_IT(huart, &uart_tbl[ch].rx_data, 1) != HAL_OK) return false;
    }

    return true;
}

bool uartClose(uint8_t ch) {
    if (uartIsValidCh(ch) == false) return false;

    HAL_UART_AbortReceive_IT(uart_tbl[ch].huart);

    return true;
}
/*=================================================================*/
uint32_t uartAvailable(uint8_t ch) {
    if (uartIsValidCh(ch) == false) return 0;
    if (uart_tbl[ch].rx_q == NULL) return 0;

    return osMessageQueueGetCount(uart_tbl[ch].rx_q);
}
/*=================================================================*/
uint8_t uartRead(uint8_t ch) {
    uint8_t ret = 0;

    if (uartIsValidCh(ch) == false) return 0;
    if (uart_tbl[ch].rx_q == NULL) return 0;

    osMessageQueueGet(uart_tbl[ch].rx_q, &ret, NULL, 0);

    return ret;
}

bool uartReadBlock(uint8_t ch, uint8_t *p_data, uint32_t timeout) {
    if (uartIsValidCh(ch) == false || p_data == NULL) return false;
    if (uart_tbl[ch].rx_q == NULL) return false;
    if (osMessageQueueGet(uart_tbl[ch].rx_q, p_data, NULL, timeout) == osOK) return true;

    return false;
}
/*=================================================================*/
uint32_t uartWrite(uint8_t ch, const uint8_t *p_data, uint32_t len) {
    UART_HandleTypeDef *huart;

    if (uartIsValidCh(ch) == false || p_data == NULL || len == 0) return 0;
    if (uart_tbl[ch].tx_mutex == NULL) return 0;

    huart = uart_tbl[ch].huart;

    osMutexAcquire(uart_tbl[ch].tx_mutex, osWaitForever);

    if (HAL_UART_Transmit(huart, (uint8_t *)p_data, len, UART_TX_TIMEOUT) != HAL_OK) len = 0;

    osMutexRelease(uart_tbl[ch].tx_mutex);

    return len;
}

bool uartWriteBlock(uint8_t ch, const uint8_t *p_data, uint32_t len){
    return uartWrite(ch, p_data, len) == len;
}

uint32_t uartPrintf(uint8_t ch, const char *fmt, ...) {
    char buf[128];
    int ret;
    uint32_t len;
    va_list args;

    if (fmt == NULL) return 0;

    va_start(args, fmt);
    ret = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (ret < 0) return 0;

    if (ret >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    else                         len = (uint32_t)ret;

    return uartWrite(ch, (uint8_t *)buf, len);
}
/*=================================================================*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    for (uint8_t ch = 0; ch < UART_CH_MAX; ch++) {
        if (huart->Instance == uart_tbl[ch].instance) {

            // [예외 처리] RPI4 채널은 1바이트 IT 처리를 완전히 무시
            if (ch == UART_CH_RPI4) break;

            if (uart_tbl[ch].rx_q != NULL) {
                osMessageQueuePut(uart_tbl[ch].rx_q, &uart_tbl[ch].rx_data, 0, 0);
            }
            HAL_UART_Receive_IT(uart_tbl[ch].huart, &uart_tbl[ch].rx_data, 1);

            break;
        }
    }
}