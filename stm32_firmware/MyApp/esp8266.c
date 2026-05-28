#include "esp8266.h"
#include "my_uart.h"
#include <stdio.h>
#include <string.h>
#include "cmsis_os2.h"
#include "main.h"
#include "stm32f4xx_hal.h"
/*
 * ESP8266 AT 명령 드라이버
 *
 * 구조:
 * esp8266.c  → ESP-01 AT 명령 처리
 * my_uart.c  → 실제 UART 송수신
 *
 * 주의:
 * 이 코드는 1차 구현용 blocking 방식입니다.
 * esp8266_SendCmd() 내부에서 timeout 동안 응답을 기다립니다.
 */
#define ESP8266_CMD_BUF_SIZE 128
#define ESP8266_SEND_BUF_SIZE 64
#define ESP8266_DEFAULT_TIMEOUT 1000
#define ESP8266_JOIN_TIMEOUT 10000
#define ESP8266_TCP_TIMEOUT 5000
#define ESP8266_SEND_TIMEOUT 5000
// inner
static void esp8266_ClearRxBuffer(esp8266_t *ctx);
static void esp8266_FlushUartRx(esp8266_t *ctx);
static uint8_t esp8266_Contains(esp8266_t *ctx, const char *str);
static esp8266_result_t esp8266_WaitResponse(
    esp8266_t *ctx,
    const char *expect,
    uint32_t timeout_ms);
static esp8266_result_t esp8266_SendRaw(
    esp8266_t *ctx,
    const char *data);
static esp8266_result_t esp8266_SendRawLen(
    esp8266_t *ctx,
    const uint8_t *data,
    uint32_t len);
static esp8266_result_t esp8266_SendCmdNoFlushRx(
    esp8266_t *ctx,
    const char *cmd,
    const char *expect,
    uint32_t timeout_ms);
static esp8266_result_t esp8266_WaitPrompt(esp8266_t *ctx, uint32_t timeout_ms);
static void esp8266_RecoverTcp(esp8266_t *ctx);
#if 1
static void esp8266_Yield(void){
    if (osKernelGetState() == osKernelRunning) {
        osDelay(1);
    }
}
#endif
// function
void esp8266_Init(esp8266_t *ctx, uint8_t uart_ch) {
    if (ctx == NULL) return;
    ctx->uart_ch = uart_ch;
    ctx->rx_len = 0;
    ctx->is_ready = 0;
    ctx->is_connected = 0;
    ctx->tcp_link_up = 0;
    ctx->cipsend_pending = 0;
    memset(ctx->rx_buf, 0, sizeof(ctx->rx_buf));
}
esp8266_result_t esp8266_ConfigTcp(esp8266_t *ctx) {
    esp8266_result_t ret;
    if (ctx == NULL) return ESP8266_ERROR;
    ret = esp8266_SendCmd(ctx, "AT+CIPMUX=0", "OK", ESP8266_DEFAULT_TIMEOUT);
    if (ret != ESP8266_OK) return ret;
    ret = esp8266_SendCmd(ctx, "AT+CIPMODE=0", "OK", ESP8266_DEFAULT_TIMEOUT);
    if (ret != ESP8266_OK) return ret;
    return ESP8266_OK;
}
void esp8266_Process(esp8266_t *ctx) {
    uint8_t data;
    if (ctx == NULL) return;
    /* 수신 버퍼 데이터 이동 */
    while (uartAvailable(ctx->uart_ch) > 0) {
        data = uartRead(ctx->uart_ch);
        if (ctx->rx_len < (sizeof(ctx->rx_buf) - 1)) {
            ctx->rx_buf[ctx->rx_len++] = (char)data;
            ctx->rx_buf[ctx->rx_len] = '\0';
        }
        else
            esp8266_ClearRxBuffer(ctx);
    }
}
esp8266_result_t esp8266_SendCmd(
    esp8266_t *ctx,
    const char *cmd,
    const char *expect,
    uint32_t timeout_ms) {
    esp8266_result_t ret;
    if (ctx == NULL || cmd == NULL || expect == NULL) return ESP8266_ERROR;
    esp8266_FlushUartRx(ctx);
    esp8266_ClearRxBuffer(ctx);
    ret = esp8266_SendRaw(ctx, cmd);
    if (ret != ESP8266_OK) return ret;
    ret = esp8266_SendRaw(ctx, "\r\n");
    if (ret != ESP8266_OK) return ret;
    return esp8266_WaitResponse(ctx, expect, timeout_ms);
}
esp8266_result_t esp8266_TestAT(esp8266_t *ctx) {
    esp8266_result_t ret;
    // 정상 응답: AT, OK
    ret = esp8266_SendCmd(ctx, "AT", "OK", ESP8266_DEFAULT_TIMEOUT);
    if (ret == ESP8266_OK) {
        ctx->is_ready = 1;
        // AT 명령 에코 비활성화
        esp8266_SendCmd(ctx, "ATE0", "OK", ESP8266_DEFAULT_TIMEOUT);
    }
    else {
        ctx->is_ready = 0;
    }
    return ret;
}
esp8266_result_t esp8266_Restart(esp8266_t *ctx) {
    esp8266_result_t ret;
    ret = esp8266_SendCmd(ctx, "AT+RST", "ready", 5000);
    ctx->is_connected = 0;
    if (ret == ESP8266_OK)
        ctx->is_ready = 1;
    else
        ctx->is_ready = 0;
    return ret;
}
esp8266_result_t esp8266_SetMode(esp8266_t *ctx, esp8266_wifi_mode_t mode) {
    char cmd[ESP8266_CMD_BUF_SIZE];
    int len;
    if (mode != ESP8266_WIFI_MODE_STA &&
        mode != ESP8266_WIFI_MODE_AP &&
        mode != ESP8266_WIFI_MODE_STA_AP) {
        return ESP8266_ERROR;
    }
    len = snprintf(cmd, sizeof(cmd), "AT+CWMODE=%d", mode);
    if (len < 0 || len >= (int)sizeof(cmd)) return ESP8266_ERROR;
    return esp8266_SendCmd(ctx, cmd, "OK", ESP8266_DEFAULT_TIMEOUT);
}
esp8266_result_t esp8266_JoinAP(
    esp8266_t *ctx,
    const char *ssid,
    const char *password) {
    char cmd[ESP8266_CMD_BUF_SIZE];
    int len;
    esp8266_result_t ret;
    if (ctx == NULL || ssid == NULL || password == NULL) return ESP8266_ERROR;
    len = snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    if (len < 0 || len >= (int)sizeof(cmd)) return ESP8266_ERROR;
    ret = esp8266_SendCmd(ctx, cmd, "OK", ESP8266_JOIN_TIMEOUT);
    if (ret == ESP8266_OK)
        ctx->is_connected = 1;
    else
        ctx->is_connected = 0;
    return ret;
}
esp8266_result_t esp8266_StartTCP(
    esp8266_t *ctx,
    const char *ip,
    uint16_t port) {
    char cmd[ESP8266_CMD_BUF_SIZE];
    int len;
    esp8266_result_t ret;
    if (ctx == NULL || ip == NULL) return ESP8266_ERROR;
    len = snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", ip, (unsigned int)port);
    if (len < 0 || len >= (int)sizeof(cmd)) return ESP8266_ERROR;
    ret = esp8266_SendCmd(ctx, cmd, "OK", ESP8266_TCP_TIMEOUT);
    if (ret != ESP8266_OK) {
        if (esp8266_Contains(ctx, "ALREADY CONNECTED") != 0) return ESP8266_OK;
    }
    return ret;
}
esp8266_result_t esp8266_SendPayload(
    esp8266_t *ctx,
    const uint8_t *data,
    uint32_t len) {
    char cmd[ESP8266_CMD_BUF_SIZE];
    esp8266_result_t ret;
    if (ctx == NULL || data == NULL || len == 0) return ESP8266_ERROR;
    if (ctx->tcp_link_up == 0) return ESP8266_ERROR;
    if (ctx->cipsend_pending != 0) {
        esp8266_RecoverTcp(ctx);
        return ESP8266_ERROR;
    }
    esp8266_Process(ctx);
    // 수신 버퍼 유지
    // esp8266_ClearRxBuffer(ctx);
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%lu", (unsigned long)len);
    ret = esp8266_SendCmdNoFlushRx(ctx, cmd, ">", ESP8266_SEND_TIMEOUT);
    if (ret != ESP8266_OK) {
        esp8266_RecoverTcp(ctx);
        return ret;
    }
    ctx->cipsend_pending = 1;
    ret = esp8266_SendRawLen(ctx, data, len);
    if (ret != ESP8266_OK) {
        ctx->cipsend_pending = 0;
        esp8266_RecoverTcp(ctx);
        return ret;
    }
    ret = esp8266_WaitResponse(ctx, "SEND OK", ESP8266_SEND_TIMEOUT);
    ctx->cipsend_pending = 0;
    if (ret != ESP8266_OK) {
        esp8266_RecoverTcp(ctx);
    }
    return ret;
}
int esp8266_TryParseIPD(
    esp8266_t *ctx,
    uint8_t *out,
    uint16_t out_max,
    uint16_t *out_len) {
    if (ctx == NULL || out == NULL || out_len == NULL) return 0;
    *out_len = 0;
    // "+IPD," 패킷 검색
    char *ipd = NULL;
    if (ctx->rx_len < 5) return 0;
    for (uint32_t i = 0; i <= ctx->rx_len - 5; i++) {
        if (memcmp(&ctx->rx_buf[i], "+IPD,", 5) == 0) {
            ipd = &ctx->rx_buf[i];
            break;
        }
    }
    if (ipd == NULL) return 0;
    // 페이로드 길이 파싱
    char *colon = NULL;
    uint32_t ipd_offset = (uint32_t)(ipd - ctx->rx_buf);
    for (uint32_t i = ipd_offset; i < ctx->rx_len; i++) {
        if (ctx->rx_buf[i] == ':') {
            colon = &ctx->rx_buf[i];
            break;
        }
    }
    if (colon == NULL) return 0;
    // IPD 정보 추출
    char header_str[32] = {0};
    uint32_t header_len = (uint32_t)(colon - ipd);
    if (header_len >= sizeof(header_str)) header_len = sizeof(header_str) - 1;
    memcpy(header_str, ipd, header_len);
    int payload_len = 0;
    if (sscanf(header_str, "+IPD,%*u,%d", &payload_len) != 1) {
        if (sscanf(header_str, "+IPD,%d", &payload_len) != 1) return 0;
    }
    if (payload_len <= 0) return 0;
    if (payload_len > (int)out_max) return -1; // 버퍼 초과
    colon++; // 데이터 시작점
    uint32_t remove_start = ipd_offset;
    uint32_t remove_end = (uint32_t)(colon - ctx->rx_buf) + (uint32_t)payload_len;
    // 데이터 수신 대기
    if (remove_end > ctx->rx_len) return 0;
    // 버퍼 갱신
    memcpy(out, colon, (uint32_t)payload_len);
    *out_len = (uint16_t)payload_len;
    uint32_t tail_len = ctx->rx_len - remove_end;
    if (tail_len > 0) {
        memmove(&ctx->rx_buf[remove_start], &ctx->rx_buf[remove_end], tail_len);
    }
    ctx->rx_len = remove_start + tail_len;
    ctx->rx_buf[ctx->rx_len] = '\0';
    return 1;
}
esp8266_result_t esp8266_SendData(esp8266_t *ctx, const char *data) {
    uint32_t len;
    if (ctx == NULL || data == NULL) return ESP8266_ERROR;
    len = (uint32_t)strlen(data);
    if (len == 0) return ESP8266_ERROR;
    return esp8266_SendPayload(ctx, (const uint8_t *)data, len);
}
esp8266_result_t esp8266_Close(esp8266_t *ctx) {
    esp8266_result_t ret;
    ret = esp8266_SendCmd(ctx, "AT+CIPCLOSE", "OK", ESP8266_DEFAULT_TIMEOUT);
    ctx->tcp_link_up = 0;
    ctx->cipsend_pending = 0;
    if (ret == ESP8266_OK) ctx->is_connected = 0;
    return ret;
}
// inner function
static void esp8266_ClearRxBuffer(esp8266_t *ctx) {
    if (ctx == NULL) return;
    memset(ctx->rx_buf, 0, sizeof(ctx->rx_buf));
    ctx->rx_len = 0;
}
static void esp8266_FlushUartRx(esp8266_t *ctx) {
    if (ctx == NULL) return;
    while (uartAvailable(ctx->uart_ch) > 0) {
        (void)uartRead(ctx->uart_ch);
    }
}
static uint8_t esp8266_Contains(esp8266_t *ctx, const char *str) {
    if (ctx == NULL || str == NULL) return 0;
    uint32_t str_len = strlen(str);
    if (str_len == 0 || ctx->rx_len < str_len) return 0;
    // 문자열 검색
    for (uint32_t i = 0; i <= ctx->rx_len - str_len; i++) {
        if (memcmp(&ctx->rx_buf[i], str, str_len) == 0) {
            return 1;
        }
    }
    return 0;
}
static esp8266_result_t esp8266_WaitResponse(
    esp8266_t *ctx,
    const char *expect,
    uint32_t timeout_ms) {
    uint32_t start_time;
    if (ctx == NULL || expect == NULL) return ESP8266_ERROR;
    start_time = HAL_GetTick();
    while ((HAL_GetTick() - start_time) < timeout_ms) {
        esp8266_Process(ctx);
        if (esp8266_Contains(ctx, expect) != 0) return ESP8266_OK;
        if (esp8266_Contains(ctx, "ERROR") != 0) return ESP8266_ERROR;
        if (esp8266_Contains(ctx, "FAIL") != 0) return ESP8266_ERROR;
        if (esp8266_Contains(ctx, "busy") != 0) return ESP8266_BUSY;
        esp8266_Yield();
    }
    return ESP8266_TIMEOUT;
}
static esp8266_result_t esp8266_SendRawLen(
    esp8266_t *ctx,
    const uint8_t *data,
    uint32_t len) {
    uint32_t written;
    if (ctx == NULL || data == NULL || len == 0) return ESP8266_ERROR;
    written = uartWrite(ctx->uart_ch, (uint8_t *)data, len);
    if (written != len) return ESP8266_ERROR;
    return ESP8266_OK;
}
static esp8266_result_t esp8266_SendRaw(
    esp8266_t *ctx,
    const char *data) {
    if (ctx == NULL || data == NULL) return ESP8266_ERROR;
    return esp8266_SendRawLen(ctx, (const uint8_t *)data, (uint32_t)strlen(data));
}
static esp8266_result_t esp8266_SendCmdNoFlushRx(
    esp8266_t *ctx,
    const char *cmd,
    const char *expect,
    uint32_t timeout_ms) {
    esp8266_result_t ret;
    if (ctx == NULL || cmd == NULL || expect == NULL) return ESP8266_ERROR;
    ret = esp8266_SendRaw(ctx, cmd);
    if (ret != ESP8266_OK) return ret;
    ret = esp8266_SendRaw(ctx, "\r\n");
    if (ret != ESP8266_OK) return ret;
    // 프롬프트 확인
    if (expect[0] == '>' && expect[1] == '\0') {
        ret = esp8266_WaitPrompt(ctx, timeout_ms);
        // 수신 대기
        if (ret == ESP8266_OK) {
            if (osKernelGetState() == osKernelRunning) {
                osDelay(10);
            } else {
                HAL_Delay(10);
            }
        }
        return ret;
    }
    return esp8266_WaitResponse(ctx, expect, timeout_ms);
}
static esp8266_result_t esp8266_WaitPrompt(esp8266_t *ctx, uint32_t timeout_ms) {
    uint32_t start_time = HAL_GetTick();
    if (ctx == NULL) return ESP8266_ERROR;
    while ((HAL_GetTick() - start_time) < timeout_ms) {
        esp8266_Process(ctx);
        // 배열 탐색
        for (uint32_t i = 0; i < ctx->rx_len; i++) {
            if (ctx->rx_buf[i] == '>') return ESP8266_OK;
        }
        if (esp8266_Contains(ctx, "ERROR") != 0) return ESP8266_ERROR;
        if (esp8266_Contains(ctx, "FAIL") != 0) return ESP8266_ERROR;
        if (esp8266_Contains(ctx, "busy") != 0) return ESP8266_BUSY;
        esp8266_Yield();
    }
    return ESP8266_TIMEOUT;
}
static void esp8266_RecoverTcp(esp8266_t *ctx) {
    if (ctx == NULL) return;
    ctx->cipsend_pending = 0;
    (void)esp8266_SendCmd(ctx, "AT+CIPCLOSE", "OK", 2000);
    ctx->tcp_link_up = 0;
    ctx->is_connected = 0;
}
