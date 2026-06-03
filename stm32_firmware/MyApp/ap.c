#include "ap.h"
#include "esp8266.h"
#include "wifi.h"
#include "my_uart.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>
// ========================================================
// KVM 작동 모드 설정
// ========================================================
#define KVM_MODE_UART      1
#define KVM_MODE_USB_HID   2
#define CURRENT_KVM_MODE   KVM_MODE_UART
#if CURRENT_KVM_MODE == KVM_MODE_USB_HID
#include "usbd_hid.h"
extern USBD_HandleTypeDef hUsbDeviceFS;
#endif
#define MY_CLIENT_ID "STM32"
// DMA 수신 버퍼 (8KB)
#define CLI_RX_BUF_SIZE 16384
// 하드웨어 핀 매핑
#define RPI_PWR_PORT       GPIOB
#define RPI_PWR_PIN        GPIO_PIN_9   // 물리 플러그(5V) 감지 핀
#define RPI_ON_PORT        GPIOB
#define RPI_ON_PIN         GPIO_PIN_10  // OS 구동 상태 감지 핀 (GPIO 22)
#define RELAY_PWR_PORT     GPIOB
#define RELAY_PWR_PIN      GPIO_PIN_13
#define RELAY_RST_PORT     GPIOB
#define RELAY_RST_PIN      GPIO_PIN_14
#define RELAY_REC_PORT     GPIOB
#define RELAY_REC_PIN      GPIO_PIN_15
extern UART_HandleTypeDef huart6;
extern UART_HandleTypeDef huart1;
extern wifi_t wifi_ctx;
extern esp8266_t esp8266_ctx;
osMessageQueueId_t cliMsgQueue;
osMutexId_t wifiTxMutex;
uint8_t cli_rx_dma_buf[CLI_RX_BUF_SIZE];
volatile int sys_init_done = 0;
// 릴레이 비동기 제어를 위한 전역 상태 변수
uint32_t pwr_off_time = 0;
uint32_t rst_off_time = 0;
uint32_t rec_step = 0;
uint32_t rec_next_time = 0;
// ========================================================
// 2. 공통 유틸리티 함수
// ========================================================
static int relay_match_cmd(const char *payload, const char *cmd3) {
    if (strncmp(payload, cmd3, 3) == 0) return 1;
    if (payload[0] >= '0' && payload[0] <= '9' && strncmp(payload + 1, cmd3, 3) == 0) return 1;
    return 0;
}
// 송신 함수 (Mutex 적용)
esp8266_result_t safe_sock_send(uint8_t type, const uint8_t *payload, uint16_t len) {
    if (wifi_ctx.state != WIFI_STATE_READY || len > 2048) return ESP8266_ERROR;
    PacketHeader h = {0xAA55, type, len};
    uint16_t total_len = sizeof(PacketHeader) + len;
    // 송신용 정적 버퍼
    static uint8_t send_buf[sizeof(PacketHeader) + 2048];
    esp8266_result_t res = ESP8266_ERROR;
    if (osMutexAcquire(wifiTxMutex, osWaitForever) == osOK) {
        memcpy(send_buf, &h, sizeof(PacketHeader));
        memcpy(send_buf + sizeof(PacketHeader), payload, len);
        res = esp8266_SendPayload(wifi_ctx.esp, send_buf, total_len);
        osMutexRelease(wifiTxMutex);
        if (res != ESP8266_OK) {
            osDelay(5);
        }
    }
    return res;
}
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
// 3. 비동기(Non-Blocking) 릴레이 제어 처리기
// ========================================================
void process_relays_nonblocking(void) {
    uint32_t now = HAL_GetTick();
    if (pwr_off_time > 0 && now >= pwr_off_time) {
        HAL_GPIO_WritePin(RELAY_PWR_PORT, RELAY_PWR_PIN, GPIO_PIN_RESET);
        pwr_off_time = 0;
    }
    if (rst_off_time > 0 && now >= rst_off_time) {
        HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_RESET);
        rst_off_time = 0;
    }
    if (rec_step == 1 && now >= rec_next_time) {
        HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_RESET);
        rec_next_time = now + 60000;
        rec_step = 2;
    }
    else if (rec_step == 2 && now >= rec_next_time) {
        HAL_GPIO_WritePin(RELAY_REC_PORT, RELAY_REC_PIN, GPIO_PIN_RESET);
        rec_step = 0;
    }
}
// ========================================================
// 4. 서버 수신 데이터 파싱 및 분기 처리
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
        // [Type 2] 릴레이 제어 패킷
        if (header.type == 2 && header.length >= 3) {
            memset(payload_str, 0, sizeof(payload_str));
            uint16_t copy_len = (header.length < sizeof(payload_str) - 1) ? header.length : (sizeof(payload_str) - 1);
            memcpy(payload_str, payload, copy_len);
            if (relay_match_cmd(payload_str, "PWR")) {
                HAL_GPIO_WritePin(RELAY_PWR_PORT, RELAY_PWR_PIN, GPIO_PIN_SET);
                pwr_off_time = HAL_GetTick() + 1000;
                uartPrintf(UART_CH_DEBUG, "[CMD] RELAY PWR ON\r\n");
            }
            else if (relay_match_cmd(payload_str, "RST")) {
                HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_SET);
                rst_off_time = HAL_GetTick() + 1000;
                uartPrintf(UART_CH_DEBUG, "[CMD] RELAY RST ON\r\n");
            }
            else if (relay_match_cmd(payload_str, "REC")) {
                HAL_GPIO_WritePin(RELAY_REC_PORT, RELAY_REC_PIN, GPIO_PIN_SET);
                HAL_GPIO_WritePin(RELAY_RST_PORT, RELAY_RST_PIN, GPIO_PIN_SET);
                rec_next_time = HAL_GetTick() + 1000;
                rec_step = 1;
                uartPrintf(UART_CH_DEBUG, "[CMD] RELAY REC ON\r\n");
            }
        }
        // [Type 3] KVM 키보드 입력 패킷
        else if (header.type == 3 && header.length > 0) {
            memset(payload_str, 0, sizeof(payload_str));
            uint16_t copy_len = (header.length < sizeof(payload_str) - 1) ? header.length : (sizeof(payload_str) - 1);
            memcpy(payload_str, payload, copy_len);
#if CURRENT_KVM_MODE == KVM_MODE_UART
            uint8_t tx_buf[64];
            uint16_t tx_len = 0;
            if (strncmp(payload_str, "ENTER", 5) == 0) {
                tx_buf[0] = '\r';
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
                    if (tx_buf[i] == '\n') tx_buf[i] = '\r';
                    if (tx_buf[i] == '\r') has_enter = 1;
                }
                if (!has_enter && tx_len < sizeof(tx_buf) - 1) {
                    tx_buf[tx_len++] = '\r';
                }
            }
            HAL_UART_Transmit(&huart1, tx_buf, tx_len, 100);
            uartPrintf(UART_CH_DEBUG, "[SYS] UART String Sent: %s\r\n", payload_str);
#elif CURRENT_KVM_MODE == KVM_MODE_USB_HID
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
// 5. RTOS Task 1: 와이파이 관리 및 통합 서버 명령 수신
// ========================================================
void StartWifiTask(void *argument) {
    uartInit();
    esp8266_Init(&esp8266_ctx, UART_CH_ESP8266);
    wifiInit(&wifi_ctx, &esp8266_ctx);
    wifiTxMutex = osMutexNew(NULL);
    sys_init_done = 1;
    uint32_t last_check_time = HAL_GetTick();
    uint32_t last_heartbeat_time = HAL_GetTick();
    int is_authenticated = 0;
    for (;;) {
        wifiProcess(&wifi_ctx);
        process_relays_nonblocking();
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
                uartPrintf(UART_CH_DEBUG, "[WIFI] Auth Packet Sent\r\n");
                is_authenticated = 1;
            }
            // 하트비트 전송 (상태 동기화)
            if ((now - last_heartbeat_time) >= 5000) {
                if (wifi_ctx.state == WIFI_STATE_READY) {
                    // 전원 연결 상태 확인
                    uint8_t pwr_state = (HAL_GPIO_ReadPin(RPI_PWR_PORT, RPI_PWR_PIN) == GPIO_PIN_SET) ? 1 : 0;
                    // OS 구동 상태 확인
                    uint8_t os_state = (HAL_GPIO_ReadPin(RPI_ON_PORT, RPI_ON_PIN) == GPIO_PIN_RESET) ? 1 : 0;
                    char tx_packet[16];
                    snprintf(tx_packet, sizeof(tx_packet), "%d,%d\n", pwr_state, os_state);
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
// 6. RTOS Task 2: CLI 고속 수신 및 중계 (라파 -> 서버)
// ========================================================
void StartCliMonitorTask(void *argument) {
    cliMsgQueue = osMessageQueueNew(10, sizeof(uint16_t), NULL);
    while (sys_init_done == 0) {
        osDelay(10);
    }
    osDelay(500);
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, cli_rx_dma_buf, CLI_RX_BUF_SIZE) != HAL_OK) {
        uartPrintf(UART_CH_DEBUG, "[ERR] USART1 DMA Start Failed!\r\n");
    }
    uint16_t rcv_len = 0;
    uint16_t last_pos = 0;
    for (;;) {
        if (osMessageQueueGet(cliMsgQueue, &rcv_len, NULL, osWaitForever) == osOK) {
            if (rcv_len > last_pos) {
                uint16_t process_len = rcv_len - last_pos;
                uartPrintf(UART_CH_DEBUG, "[SYS] Rcv %d bytes\r\n", process_len);
                if (wifi_ctx.state == WIFI_STATE_READY) {
                    for (uint16_t i = 0; i < process_len; i += 1024) {
                        uint16_t chunk = (process_len - i > 1024) ? 1024 : (process_len - i);
                        safe_sock_send(1, &cli_rx_dma_buf[last_pos + i], chunk);
                        osDelay(2);
                    }
                }
            }
            else if (rcv_len < last_pos) {
                uint16_t process_len = CLI_RX_BUF_SIZE - last_pos;
                uartPrintf(UART_CH_DEBUG, "[SYS] Rcv %d bytes (Wrap)\r\n", process_len + rcv_len);
                if (wifi_ctx.state == WIFI_STATE_READY) {
                    for (uint16_t i = 0; i < process_len; i += 1024) {
                        uint16_t chunk = (process_len - i > 1024) ? 1024 : (process_len - i);
                        safe_sock_send(1, &cli_rx_dma_buf[last_pos + i], chunk);
                        osDelay(2);
                    }
                }
                if (rcv_len > 0 && wifi_ctx.state == WIFI_STATE_READY) {
                    for (uint16_t i = 0; i < rcv_len; i += 1024) {
                        uint16_t chunk = (rcv_len - i > 1024) ? 1024 : (rcv_len - i);
                        safe_sock_send(1, &cli_rx_dma_buf[i], chunk);
                        osDelay(2);
                    }
                }
            }
            last_pos = rcv_len;
        }
    }
}
// ========================================================
// 7. 인터럽트 콜백
// ========================================================
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == USART1) {
        osMessageQueuePut(cliMsgQueue, &Size, 0, 0);
    }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        HAL_UART_AbortReceive(huart);
        HAL_UARTEx_ReceiveToIdle_DMA(&huart1, cli_rx_dma_buf, CLI_RX_BUF_SIZE);
    }
    else if (huart->Instance == USART6) {
        HAL_UART_AbortReceive(huart);
    }
}
