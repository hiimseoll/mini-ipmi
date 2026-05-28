#ifndef __MAP_DRIVER__ESP8266_H__
#define __MAP_DRIVER__ESP8266_H__
#include "my_uart.h"
#define ESP8266_RX_BUF_SIZE 1024 // 수신 버퍼 크기
typedef enum {
    ESP8266_OK = 0,
    ESP8266_ERROR,
    ESP8266_TIMEOUT,
    ESP8266_BUSY
} esp8266_result_t;
typedef enum {
    ESP8266_WIFI_MODE_STA = 1,
    ESP8266_WIFI_MODE_AP = 2,
    ESP8266_WIFI_MODE_STA_AP = 3
} esp8266_wifi_mode_t;
typedef struct {
    uint8_t uart_ch;
    char rx_buf[ESP8266_RX_BUF_SIZE]; // 수신 버퍼
    uint32_t rx_len;
    uint8_t is_ready;
    uint8_t is_connected;
    uint8_t tcp_link_up;       /* AT+CIPSTART 성공 후 1 */
    uint8_t cipsend_pending;   /* '>' 대기 중 페이로드 미전송 */
} esp8266_t;
void esp8266_Init(esp8266_t *ctx, uint8_t uart_ch);
void esp8266_Process(esp8266_t *ctx);
/** AT 모드 유지: CIPMUX=0, CIPMODE=0 (투명 전송 방지) */
esp8266_result_t esp8266_ConfigTcp(esp8266_t *ctx);
esp8266_result_t esp8266_SendCmd(
    esp8266_t *ctx,
    const char *cmd,
    const char *expect,
    uint32_t timeout_ms);
esp8266_result_t esp8266_TestAT(esp8266_t *ctx);
esp8266_result_t esp8266_Restart(esp8266_t *ctx);
esp8266_result_t esp8266_SetMode(esp8266_t *ctx, esp8266_wifi_mode_t mode);
esp8266_result_t esp8266_JoinAP(esp8266_t *ctx, const char *ssid, const char *password);
esp8266_result_t esp8266_StartTCP(esp8266_t *ctx, const char *ip, uint16_t port);
/** AT+CIPSEND -> '>' -> raw bytes -> SEND OK (바이너리/문자열 공통) */
esp8266_result_t esp8266_SendPayload(
    esp8266_t *ctx,
    const uint8_t *data,
    uint32_t len);
/** NULL 종료 문자열 전송 (내부적으로 SendPayload 사용) */
esp8266_result_t esp8266_SendData(esp8266_t *ctx, const char *data);
esp8266_result_t esp8266_Close(esp8266_t *ctx);
/**
 * rx_buf에서 ESP8266 +IPD 수신 프레임을 찾아 TCP 페이로드만 추출합니다.
 * @return 1: 추출 성공, 0: +IPD 없음/미완료, -1: 오류
 */
int esp8266_TryParseIPD(
    esp8266_t *ctx,
    uint8_t *out,
    uint16_t out_max,
    uint16_t *out_len);
#endif // __MAP_DRIVER__ESP8266_H__
