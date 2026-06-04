#include "ap.h"
#include "esp8266.h"
#include "wifi.h"
#include "my_uart.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

// ========================================================
// 1. KVM 모드 설정
// ========================================================
#define KVM_MODE_UART      1
#define KVM_MODE_USB_HID   2
#define CURRENT_KVM_MODE   KVM_MODE_UART // 모드 변경 시 수정

#include "usbd_hid.h"
extern USBD_HandleTypeDef hUsbDeviceFS;

#define MY_CLIENT_ID "STM02"
#define CLI_RX_BUF_SIZE 512

// GPIO 매핑
#define RPI_PWR_PORT       GPIOB
#define RPI_PWR_PIN        GPIO_PIN_9
#define RELAY_PWR_PORT     GPIOB
#define RELAY_PWR_PIN      GPIO_PIN_13 // PWR
#define RELAY_RST_PORT     GPIOB
#define RELAY_RST_PIN      GPIO_PIN_14 // RST
#define RELAY_REC_PORT     GPIOB
#define RELAY_REC_PIN      GPIO_PIN_15 // REC

extern UART_HandleTypeDef huart1;
extern wifi_t wifi_ctx;
extern esp8266_t esp8266_ctx;

osMessageQueueId_t cliMsgQueue;
osMutexId_t wifiTxMutex;
uint8_t cli_rx_dma_buf[CLI_RX_BUF_SIZE];

// ========================================================
// HID 키보드 데이터 구조체
// ========================================================
typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} KeyboardReport;

static uint8_t char_to_hid_scancode(char c) {
    if (c >= 'a' && c <= 'z') return (c - 'a' + 0x04);
    if (c >= 'A' && c <= 'Z') return (c - 'A' + 0x04);
    if (c >= '1' && c <= '9') return (c - '1' + 0x1E);
    if (c == '0') return 0x27;
    if (c == ' ') return 0x2C;
    if (c == '\n' || c == '\r') return 0x28;
    return 0;
}

// ========================================================
// 릴레이 제어 함수
// ========================================================
static int relay_match_cmd(const char *payload, const char *cmd3) {
    if (strncmp(payload, cmd3, 3) == 0) return 1;
    if (payload[0] >= '0' && payload[0] <= '9' && strncmp(payload + 1, cmd3, 3) == 0) return 1;
    return 0;
}

void rec_relay_write() {
    HAL_GPIO_WritePin(RELAY_REC_PORT, RELAY_REC_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_SET);
    osDelay(1000);
    HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_RESET);
    osDelay(10000);
    HAL_GPIO_WritePin(RELAY_REC_PORT, RELAY_REC_PIN, GPIO_PIN_RESET);
}

void pwr_relay_write() {
    HAL_GPIO_WritePin(RELAY_PWR_PORT, RELAY_PWR_PIN, GPIO_PIN_SET);
    osDelay(1000);
    HAL_GPIO_WritePin(RELAY_PWR_PORT, RELAY_PWR_PIN, GPIO_PIN_RESET);
}

void rst_relay_write() {
    HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_SET);
    osDelay(1000);
    HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_RESET);
}

// ========================================================
// 통신 유틸리티
// ========================================================
esp8266_result_t safe_sock_send(uint8_t type, const uint8_t *payload, uint16_t len) {
    if (wifi_ctx.state != WIFI_STATE_READY || len > 512) return ESP8266_ERROR;
    PacketHeader h = {0xAA55, type, len};
    uint8_t send_buf[sizeof(PacketHeader) + 512];
    memcpy(send_buf, &h, sizeof(PacketHeader));
    memcpy(send_buf + sizeof(PacketHeader), payload, len);

    esp8266_result_t res = ESP8266_ERROR;
    if (osMutexAcquire(wifiTxMutex, osWaitForever) == osOK) {
        res = esp8266_SendPayload(wifi_ctx.esp, send_buf, sizeof(PacketHeader) + len);
        osMutexRelease(wifiTxMutex);
        if (res != ESP8266_OK) osDelay(5);
    }
    return res;
}

// ========================================================
// 명령 처리 로직
// ========================================================
void process_server_command(uint8_t *rcv_data, uint16_t rcv_len) {
    PacketHeader *h = (PacketHeader *)rcv_data;
    if (rcv_len < sizeof(PacketHeader) || h->magic != 0xAA55) return;
    char *payload = (char *)(rcv_data + sizeof(PacketHeader));

    if (h->type == 2) { // 릴레이 제어
        if (relay_match_cmd(payload, "PWR")) pwr_relay_write();
        else if (relay_match_cmd(payload, "RST")) rst_relay_write();
        else if (relay_match_cmd(payload, "REC")) rec_relay_write();
    }
    else if (h->type == 3) { // KVM 제어
#if CURRENT_KVM_MODE == KVM_MODE_UART
        uint8_t tx_buf[64] = {0};
        uint16_t tx_len = (h->length < 63) ? h->length : 63;
        memcpy(tx_buf, payload, tx_len);
        if (strncmp(payload, "ENTER", 5) == 0) { tx_buf[0] = '\n'; tx_len = 1; }
        else if (tx_buf[tx_len-1] != '\n') { tx_buf[tx_len++] = '\n'; }
        HAL_UART_Transmit(&huart1, tx_buf, tx_len, 100);
#else
        // USB HID 로직
        if (strncmp(payload, "ENTER", 5) == 0) {
            KeyboardReport r = {0}; r.keys[0] = 0x28;
            USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&r, sizeof(r));
            osDelay(30); memset(&r, 0, sizeof(r)); USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&r, sizeof(r));
        } else {
            for(int i=0; i<h->length; i++) {
                KeyboardReport r = {0}; r.keys[0] = char_to_hid_scancode(payload[i]);
                if(r.keys[0]) {
                    USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&r, sizeof(r));
                    osDelay(30); memset(&r, 0, sizeof(r)); USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&r, sizeof(r));
                    osDelay(30);
                }
            }
        }
#endif
    }
}

// ========================================================
// RTOS 태스크들
// ========================================================
void StartCommandTask(void *argument) {
    uint8_t pkt[256];
    uint16_t pkt_len;
    for(;;) {
        if (wifiIsConnected(&wifi_ctx)) {
            while (esp8266_TryParseIPD(&esp8266_ctx, pkt, sizeof(pkt), &pkt_len) > 0) {
                process_server_command(pkt, pkt_len);
            }
        }
        osDelay(10);
    }
}

void StartCliMonitorTask(void *argument) {
    cliMsgQueue = osMessageQueueNew(10, sizeof(uint16_t), NULL);
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, cli_rx_dma_buf, CLI_RX_BUF_SIZE);
    uint16_t rcv_len, last_pos = 0;
    for (;;) {
        if (osMessageQueueGet(cliMsgQueue, &rcv_len, NULL, osWaitForever) == osOK) {
            uint16_t process_len = (rcv_len >= last_pos) ? (rcv_len - last_pos) : (CLI_RX_BUF_SIZE - last_pos);
            if (process_len > 0) {
                if (wifi_ctx.state == WIFI_STATE_READY) safe_sock_send(1, &cli_rx_dma_buf[last_pos], process_len);
                last_pos = (rcv_len >= last_pos) ? rcv_len : 0;
            }
        }
    }
}

void StartWifiTask(void *argument) {
    HAL_GPIO_WritePin(RELAY_PWR_PORT, RELAY_PWR_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(RELAY_REC_PORT, RELAY_REC_PIN, GPIO_PIN_RESET);
    uartInit();
    esp8266_Init(&esp8266_ctx, UART_CH_ESP8266);
    wifiInit(&wifi_ctx, &esp8266_ctx);
    wifiTxMutex = osMutexNew(NULL);
    for (;;) {
        wifiProcess(&wifi_ctx);
        if (wifiIsConnected(&wifi_ctx) == 1 && wifi_ctx.state == WIFI_STATE_READY) {
            uint8_t pwr_state = (HAL_GPIO_ReadPin(RPI_PWR_PORT, RPI_PWR_PIN) == GPIO_PIN_SET) ? '1' : '0';
            safe_sock_send(4, &pwr_state, 1);
        }
        osDelay(5000);
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == USART1) osMessageQueuePut(cliMsgQueue, &Size, 0, 0);
}