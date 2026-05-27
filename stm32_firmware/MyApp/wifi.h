#ifndef __WIFI_H__
#define __WIFI_H__
#include "main.h"
#include "esp8266.h"
// 네트워크 연결 FSM 상태
typedef enum {
    WIFI_STATE_INIT = 0,
    WIFI_STATE_AT_CHECK,        // 1. AT 명령어 응답 확인
    WIFI_STATE_SET_MODE,        // 2. Station 모드 변경
    WIFI_STATE_JOIN_AP,         // 3. 와이파이 공유기 접속
    WIFI_STATE_CHECK_PING,      // 4. 인터넷 핑 테스트
    WIFI_STATE_GET_IP,          // 5. 할당된 IP 주소 확인
    WIFI_STATE_SOCKET_CONNECT,  // 6. 서버 소켓 연결
    WIFI_STATE_AUTH_LOGIN,      // 7. 인증 처리
    WIFI_STATE_READY,           // 8. 연결 완료
    WIFI_STATE_ERROR,           // 예외 처리 단계 (CIPCLOSE 등 수행)
    WIFI_STATE_RETRY            // 재접속 전 쿨타임 대기
} wifi_state_t;
typedef struct {
    esp8266_t *esp;
    wifi_state_t state;
    uint8_t connected;          // 로그인 성공 플래그
} wifi_t;
#pragma pack(push, 1)
typedef struct { 
    uint16_t magic; // 매직 넘버 
    uint8_t type; // 패킷 타입
    uint16_t length; //문자열 길이.
} PacketHeader;
#pragma pack(pop)
extern wifi_t wifi_ctx;
extern esp8266_t esp8266_ctx;
void wifiInit(wifi_t *ctx, esp8266_t *esp);
void wifiProcess(wifi_t *ctx);
uint8_t wifiIsConnected(wifi_t *ctx);
int wifiPingCheck(wifi_t *ctx, const char* ip);
int wifiPrintIP(wifi_t *ctx);
#endif /* __WIFI_H__ */
