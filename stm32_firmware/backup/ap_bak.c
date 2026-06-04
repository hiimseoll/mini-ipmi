#include "ap.h"
#include "esp8266.h"
#include "wifi.h"
#include "my_uart.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

// ========================================================
// [핵심] KVM 작동 모드 스위치 (확장성을 위한 아키텍처)
// 1 = UART 시리얼 콘솔 모드 (현재 라즈베리 파이 완벽 제어용)
// 2 = USB HID 키보드 모드 (향후 일반 데스크탑/PC BIOS 제어용)
// ========================================================
#define KVM_MODE_UART      1
#define KVM_MODE_USB_HID   2

#define CURRENT_KVM_MODE   KVM_MODE_UART   // <--- 여기서 모드를 변경하세요!
// ========================================================

#if CURRENT_KVM_MODE == KVM_MODE_USB_HID
#include "usbd_hid.h"
extern USBD_HandleTypeDef hUsbDeviceFS;
#endif

#define MY_CLIENT_ID "STM02"
#define CLI_RX_BUF_SIZE 512

// 라파 전원 감지용 GPIO
#define RPI_PWR_PORT  GPIOB
#define RPI_PWR_PIN   GPIO_PIN_9

// 릴레이 모듈 제어용 GPIO
#define RELAY_PWR_PORT     GPIOB
#define RELAY_PWR_PIN      GPIO_PIN_13
#define RELAY_RST_PORT     GPIOB
#define RELAY_RST_PIN      GPIO_PIN_14
#define RELAY_REC_PORT     GPIOB
#define RELAY_REC_PIN      GPIO_PIN_15

// 하드웨어 매핑
extern UART_HandleTypeDef huart6;
extern UART_HandleTypeDef huart1;

// RTOS 및 통신 컨텍스트
extern wifi_t wifi_ctx;
extern esp8266_t esp8266_ctx;

osMessageQueueId_t cliMsgQueue;
osMutexId_t wifiTxMutex;
uint8_t cli_rx_dma_buf[CLI_RX_BUF_SIZE];


// ========================================================
// 2. 공통 유틸리티 함수
// ========================================================
static int relay_match_cmd(const char *payload, const char *cmd3) {
    if (strncmp(payload, cmd3, 3) == 0) return 1;
    if (payload[0] >= '0' && payload[0] <= '9' && strncmp(payload + 1, cmd3, 3) == 0) return 1;
    return 0;
}

// 스레드 안전 송신 + 와이파이 과부하 방지 (최적화 버전)
esp8266_result_t safe_sock_send(uint8_t type, const uint8_t *payload, uint16_t len) {
    if (wifi_ctx.state != WIFI_STATE_READY || len > 512) return ESP8266_ERROR;

    PacketHeader h = {0xAA55, type, len};
    uint16_t total_len = sizeof(PacketHeader) + len;
    uint8_t send_buf[sizeof(PacketHeader) + 512];

    memcpy(send_buf, &h, sizeof(PacketHeader));
    memcpy(send_buf + sizeof(PacketHeader), payload, len);

    esp8266_result_t res = ESP8266_ERROR;

    if (osMutexAcquire(wifiTxMutex, osWaitForever) == osOK) {
        res = esp8266_SendPayload(wifi_ctx.esp, send_buf, total_len);
        osMutexRelease(wifiTxMutex);

        // [핵심 수정] 터미널을 망가뜨리는 로그 출력 삭제, 딜레이 대폭 감소 (50ms -> 5ms)
        if (res != ESP8266_OK) {
            osDelay(5); // 와이파이 모듈이 뻗지 않고 숨만 쉴 수 있도록 아주 짧게 대기
        }
    }
    return res;
}
// ========================================================
// [선택적 컴파일] USB HID 관련 함수 및 구조체
// ========================================================
#if CURRENT_KVM_MODE == KVM_MODE_USB_HID
static uint8_t char_to_hid_scancode(char c) {
    if (c >= 'a' && c <= 'z') return (c - 'a' + 0x04);
    if (c >= 'A' && c <= 'Z') return (c - 'A' + 0x04);
    if (c >= '1' && c <= '9') return (c - '1' + 0x1E);
    if (c == '0') return 0x27;
    if (c == ' ') return 0x2C;
    if (c == '\n' || c == '\r') return 0x28;
    return 0;
}

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} KeyboardReport;
#endif


// ========================================================
// 3. 서버 수신 데이터 파싱 및 처리
// ========================================================
void process_server_command(uint8_t *rcv_data, uint16_t rcv_len) {
    PacketHeader header;
    char payload_str[64];
    uint16_t need_len;
    uint16_t offset = 0;

    while ((rcv_len - offset) >= sizeof(PacketHeader)) {
        memcpy(&header, rcv_data + offset, sizeof(PacketHeader));

        if (header.magic != 0xAA55) break;

        need_len = (uint16_t)(sizeof(PacketHeader) + header.length);
        if ((rcv_len - offset) < need_len) break;

        uint8_t *payload = rcv_data + offset + sizeof(PacketHeader);

        // --------------------------------------------------------
        // [Type 2] 릴레이 제어 (하드웨어 리셋/전원)
        // --------------------------------------------------------
        if (header.type == 2 && header.length >= 3) {
            memset(payload_str, 0, sizeof(payload_str));
            uint16_t copy_len = (header.length < sizeof(payload_str) - 1) ? header.length : (sizeof(payload_str) - 1);
            memcpy(payload_str, payload, copy_len);

            if (relay_match_cmd(payload_str, "PWR")) {
                HAL_GPIO_WritePin(RELAY_PWR_PORT, RELAY_PWR_PIN, GPIO_PIN_SET);
                uartPrintf(UART_CH_DEBUG, "[CMD] RELAY PWR ON\r\n");
            }
            else if (relay_match_cmd(payload_str, "RST")) {
                HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_SET);
                uartPrintf(UART_CH_DEBUG, "[CMD] RELAY RST ON\r\n");
            }
            else if (relay_match_cmd(payload_str, "REC")) {
                HAL_GPIO_WritePin(RELAY_REC_PORT, RELAY_REC_PIN, GPIO_PIN_SET);
                uartPrintf(UART_CH_DEBUG, "[CMD] RELAY REC ON\r\n");
            }
        }

        // --------------------------------------------------------
        // [Type 3] 원격 제어 키보드 입력 분기 (UART vs USB)
        // --------------------------------------------------------
        else if (header.type == 3 && header.length > 0) {
            memset(payload_str, 0, sizeof(payload_str));
            uint16_t copy_len = (header.length < sizeof(payload_str) - 1) ? header.length : (sizeof(payload_str) - 1);
            memcpy(payload_str, payload, copy_len);

#if CURRENT_KVM_MODE == KVM_MODE_UART
            // --- [모드 1] UART 시리얼 콘솔 모드 ---
            uint8_t tx_buf[64];
            uint16_t tx_len = 0;

            // 1. 단독 ENTER 전송 시 \n 딱 하나만 쏘도록 수정
            if (strncmp(payload_str, "ENTER", 5) == 0) {
                tx_buf[0] = '\n';
                tx_len = 1;
            }
            else if (strncmp(payload_str, "SPACE", 5) == 0) {
                tx_buf[0] = ' ';
                tx_len = 1;
            }
            else {
                memcpy(tx_buf, payload_str, copy_len);
                tx_len = copy_len;

                int has_enter = 0;
                for (int i = 0; i < tx_len; i++) {
                    if (tx_buf[i] == '\r' || tx_buf[i] == '\n') has_enter = 1;
                }

                // 2. 문자열 마지막에도 \n 하나만 붙여줌
                if (!has_enter && tx_len < sizeof(tx_buf) - 1) {
                    tx_buf[tx_len++] = '\n';
                }
            }
            HAL_UART_Transmit(&huart1, tx_buf, tx_len, 100);
            uartPrintf(UART_CH_DEBUG, "[SYS] UART String Sent: %s\r\n", payload_str);

#elif CURRENT_KVM_MODE == KVM_MODE_USB_HID
            // --- [모드 2] USB HID 키보드 모드 ---
            if (strncmp(payload_str, "ENTER", 5) == 0) {
                KeyboardReport report = {0};
                report.keys[0] = 0x28; // Enter
                USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                osDelay(40);
                memset(&report, 0, sizeof(KeyboardReport));
                USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                uartPrintf(UART_CH_DEBUG, "[SYS] HID Key Sent: ENTER\r\n");
            }
            else if (strncmp(payload_str, "SPACE", 5) == 0) {
                KeyboardReport report = {0};
                report.keys[0] = 0x2C; // Space
                USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                osDelay(40);
                memset(&report, 0, sizeof(KeyboardReport));
                USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                uartPrintf(UART_CH_DEBUG, "[SYS] HID Key Sent: SPACE\r\n");
            }
            else {
                int enter_needed = 1;
                for (int i = 0; i < copy_len; i++) {
                    char c = payload_str[i];
                    if (c == '\0') break;
                    if (c == '\n' || c == '\r') enter_needed = 0;

                    KeyboardReport report = {0};
                    report.keys[0] = char_to_hid_scancode(c);

                    if (report.keys[0] != 0) {
                        if (c >= 'A' && c <= 'Z') report.modifiers = 0x02;

                        USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                        osDelay(30);
                        memset(&report, 0, sizeof(KeyboardReport));
                        USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                        osDelay(30);
                    }
                }
                if (enter_needed) {
                    KeyboardReport report = {0};
                    report.keys[0] = 0x28; // Enter
                    USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                    osDelay(30);
                    memset(&report, 0, sizeof(KeyboardReport));
                    USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t*)&report, sizeof(KeyboardReport));
                    osDelay(30);
                }
                uartPrintf(UART_CH_DEBUG, "[SYS] HID String Sent: %s\r\n", payload_str);
            }
#endif
        }
        offset += need_len;
    }
}

static void ap_poll_server_commands(esp8266_t *esp) {
    uint8_t pkt[256];
    uint16_t pkt_len;
    int parse_ret;

    while ((parse_ret = esp8266_TryParseIPD(esp, pkt, sizeof(pkt), &pkt_len)) > 0) {
        process_server_command(pkt, pkt_len);
    }
}


// ========================================================
// 4. RTOS Task 1: 와이파이 관리 및 하트비트
// ========================================================
void StartWifiTask(void *argument) {
    uartInit();

    esp8266_Init(&esp8266_ctx, UART_CH_ESP8266);
    wifiInit(&wifi_ctx, &esp8266_ctx);

    wifiTxMutex = osMutexNew(NULL);

    uint32_t last_check_time = HAL_GetTick();
    uint32_t last_heartbeat_time = HAL_GetTick();

    int is_authenticated = 0;

    for (;;) {
        wifiProcess(&wifi_ctx);

        if (wifiIsConnected(&wifi_ctx) == 1) {
            uint32_t now = HAL_GetTick();

            ap_poll_server_commands(wifi_ctx.esp);

            if ((now - last_check_time) >= 5000) {
                if (wifi_ctx.state != WIFI_STATE_READY) {
                    wifi_ctx.connected = 0;
                    wifi_ctx.state = WIFI_STATE_ERROR;
                    is_authenticated = 0;
                }
                last_check_time = HAL_GetTick();
            }

            if (wifi_ctx.state == WIFI_STATE_READY && is_authenticated == 0) {
                char auth_str[32];
                snprintf(auth_str, sizeof(auth_str), "%s:1234", MY_CLIENT_ID);

                safe_sock_send(0, (uint8_t*)auth_str, strlen(auth_str));
                uartPrintf(UART_CH_DEBUG, "[WIFI] Auth Packet Sent: %s\r\n", auth_str);

                is_authenticated = 1;
            }

            if ((now - last_heartbeat_time) >= 5000) {
                if (wifi_ctx.state == WIFI_STATE_READY) {
                    uint8_t pwr_state = (HAL_GPIO_ReadPin(RPI_PWR_PORT, RPI_PWR_PIN) == GPIO_PIN_SET) ? 1 : 0;
                    char tx_packet[16];

                    snprintf(tx_packet, sizeof(tx_packet), "%d\n", pwr_state);
                    safe_sock_send(4, (uint8_t*)tx_packet, (uint16_t)strlen(tx_packet));
                }
                last_heartbeat_time = HAL_GetTick();
            }
        }
        else {
            last_check_time = HAL_GetTick();
            last_heartbeat_time = HAL_GetTick();
        }
        osDelay(10);
    }
}


// ========================================================
// 5. RTOS Task 2: CLI 고속 수신 및 중계 (라파 -> 서버)
// ========================================================
void StartCliMonitorTask(void *argument) {
    cliMsgQueue = osMessageQueueNew(10, sizeof(uint16_t), NULL);

    osDelay(500);

    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, cli_rx_dma_buf, CLI_RX_BUF_SIZE) != HAL_OK) {
        uartPrintf(UART_CH_DEBUG, "[ERR] USART1 DMA Start Failed!\r\n");
    }

    uint16_t rcv_len = 0;
    uint16_t last_pos = 0;

    for (;;) {
        if (osMessageQueueGet(cliMsgQueue, &rcv_len, NULL, osWaitForever) == osOK) {
            uint16_t process_len = 0;

            if (rcv_len > last_pos) {
                process_len = rcv_len - last_pos;
                uartPrintf(UART_CH_DEBUG, "%.*s", process_len, &cli_rx_dma_buf[last_pos]);

                if (wifi_ctx.state == WIFI_STATE_READY) {
                    safe_sock_send(1, &cli_rx_dma_buf[last_pos], process_len);
                    osDelay(10);
                }
            }
            else if (rcv_len < last_pos) {
                process_len = CLI_RX_BUF_SIZE - last_pos;
                uartPrintf(UART_CH_DEBUG, "%.*s", process_len, &cli_rx_dma_buf[last_pos]);
                if (wifi_ctx.state == WIFI_STATE_READY) {
                    safe_sock_send(1, &cli_rx_dma_buf[last_pos], process_len);
                    osDelay(10);
                }

                if (rcv_len > 0) {
                    uartPrintf(UART_CH_DEBUG, "%.*s", rcv_len, &cli_rx_dma_buf[0]);
                    if (wifi_ctx.state == WIFI_STATE_READY) {
                        safe_sock_send(1, &cli_rx_dma_buf[0], rcv_len);
                        osDelay(10);
                    }
                }
            }
            last_pos = rcv_len;
        }
    }
}


// ========================================================
// 6. 인터럽트 콜백
// ========================================================
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == USART1) {
        osMessageQueuePut(cliMsgQueue, &Size, 0, 0);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        uartPrintf(UART_CH_DEBUG, "\r\n[ERR] USART1 Error (Code: %lu). Restarting DMA...\r\n", huart->ErrorCode);
        HAL_UART_AbortReceive(huart);
        HAL_UARTEx_ReceiveToIdle_DMA(&huart1, cli_rx_dma_buf, CLI_RX_BUF_SIZE);
    }
    else if (huart->Instance == USART6) {
        HAL_UART_AbortReceive(huart);
    }
}