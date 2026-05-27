#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cmsis_os2.h"
#include "esp8266.h"
#include "main.h"

#define WIFI_SSID         "KCCI601"
#define WIFI_PASSWORD     "@kcci601@"
#define TARGET_PING_IP    "10.10.16.254"
#define SERVER_IP         "10.10.16.83"
#define SERVER_PORT       "9000"

#define MY_CLIENT_ID      "STM32"
#define MY_CLIENT_PW      "1234"

wifi_t wifi_ctx;
esp8266_t esp8266_ctx;

void wifiInit(wifi_t *ctx, esp8266_t *esp) {
    ctx->esp = esp;
    ctx->state = WIFI_STATE_INIT;
    ctx->connected = 0;
}

void wifiProcess(wifi_t *ctx) {
    char cmd_buf[128];

    esp8266_Process(ctx->esp);

    if (ctx->state == WIFI_STATE_READY) {
        if (strstr(ctx->esp->rx_buf, "CLOSED") != NULL) {
            ctx->connected = 0;
            ctx->esp->tcp_link_up = 0;
            ctx->state = WIFI_STATE_ERROR;
            return;
        }
    }

    switch (ctx->state) {
        case WIFI_STATE_INIT:
            ctx->state = WIFI_STATE_AT_CHECK;
            break;

        case WIFI_STATE_AT_CHECK:
            if (esp8266_TestAT(ctx->esp) == ESP8266_OK) {
                ctx->state = WIFI_STATE_SET_MODE;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;

        case WIFI_STATE_SET_MODE:
            if (esp8266_SetMode(ctx->esp, ESP8266_WIFI_MODE_STA) == ESP8266_OK) {
                ctx->state = WIFI_STATE_JOIN_AP;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;

        case WIFI_STATE_JOIN_AP:
            if (esp8266_JoinAP(ctx->esp, WIFI_SSID, WIFI_PASSWORD) == ESP8266_OK) {
                ctx->state = WIFI_STATE_GET_IP;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;

        case WIFI_STATE_CHECK_PING:
            if (wifiPingCheck(ctx, TARGET_PING_IP) == 0) {
                ctx->state = WIFI_STATE_GET_IP;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;

        case WIFI_STATE_GET_IP:
            if (wifiPrintIP(ctx) == 0) {
                ctx->state = WIFI_STATE_SOCKET_CONNECT;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;

        case WIFI_STATE_SOCKET_CONNECT:
            (void)esp8266_ConfigTcp(ctx->esp);

            snprintf(cmd_buf, sizeof(cmd_buf),
                     "AT+CIPSTART=\"TCP\",\"%s\",%s", SERVER_IP, SERVER_PORT);

            if (esp8266_SendCmd(ctx->esp, cmd_buf, "OK", 5000) == ESP8266_OK) {
                ctx->esp->tcp_link_up = 1;
                ctx->state = WIFI_STATE_AUTH_LOGIN;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;

        case WIFI_STATE_AUTH_LOGIN: {
            char payload[64];
            uint16_t payload_len;
            uint16_t total_len;
            PacketHeader h;
            uint8_t send_buf[sizeof(PacketHeader) + 64];

            snprintf(payload, sizeof(payload), "%s:%s", MY_CLIENT_ID, MY_CLIENT_PW);
            payload_len = (uint16_t)strlen(payload);
            total_len = sizeof(PacketHeader) + payload_len;

            h.magic = 0xAA55;
            h.type = 0;
            h.length = payload_len;

            memcpy(send_buf, &h, sizeof(PacketHeader));
            memcpy(send_buf + sizeof(PacketHeader), payload, payload_len);

            if (esp8266_SendPayload(ctx->esp, send_buf, total_len) == ESP8266_OK) {
                memset(ctx->esp->rx_buf, 0, sizeof(ctx->esp->rx_buf));
                ctx->esp->rx_len = 0;
                ctx->connected = 1;
                ctx->state = WIFI_STATE_READY;
            } else {
                ctx->state = WIFI_STATE_ERROR;
            }
            break;
        }

        case WIFI_STATE_READY:
            break;

        case WIFI_STATE_ERROR:
            ctx->connected = 0;
            esp8266_Close(ctx->esp);
            ctx->state = WIFI_STATE_RETRY;
            break;

        case WIFI_STATE_RETRY:
            osDelay(3000);
            ctx->state = WIFI_STATE_INIT;
            break;

        default:
            ctx->state = WIFI_STATE_INIT;
            break;
    }
}

uint8_t wifiIsConnected(wifi_t *ctx) {
    if (ctx == NULL) return 0;
    return ctx->connected;
}

int wifiPingCheck(wifi_t *ctx, const char* ip) {
    char cmd_buf[64];

    if (ctx == NULL || ctx->esp == NULL || ip == NULL) return -1;

    snprintf(cmd_buf, sizeof(cmd_buf), "AT+PING=\"%s\"", ip);

    if (esp8266_SendCmd(ctx->esp, cmd_buf, "OK", 5000) != ESP8266_OK) {
        return -1;
    }

    return (strstr(ctx->esp->rx_buf, "+PING") != NULL) ? 0 : -1;
}

int wifiPrintIP(wifi_t *ctx) {
    if (ctx == NULL || ctx->esp == NULL) return -1;

    if (esp8266_SendCmd(ctx->esp, "AT+CIFSR", "OK", 2000) != ESP8266_OK) {
        return -1;
    }

    return (strstr(ctx->esp->rx_buf, "STAIP") != NULL) ? 0 : -1;
}
