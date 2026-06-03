#ifndef __AP_H__
#define __AP_H__

#include "main.h"
#include "esp8266.h"

// AP 및 리눅스 서버 연결용 FSM 상태 정의
typedef enum {
    AP_STATE_INIT = 0,
    AP_STATE_AT_CHECK,        // 모듈 응답 확인 (AT)
    AP_STATE_SET_MODE,        // Station 모드 변경 (AT+CWMODE=1)
    AP_STATE_JOIN_ROUTER,     // 와이파이 공유기 접속 (AT+CWJAP)
    AP_STATE_SOCKET_CONNECT,  // 리눅스 서버 소켓 연결 (AT+CIPSTART)
    AP_STATE_AUTH_LOGIN,      // [ID:PW] 송신 후 인증 확인
    AP_STATE_CONNECTED_READY, // 인증 완료, 정상 데이터 송수신 단계
    AP_STATE_ERROR_HANDLER,   // 각종 실패/오류 예외 처리 단계
    AP_STATE_RETRY_DELAY      // 재접속 전 쿨타임 대기
} ap_state_t;

typedef struct {
    esp8266_t *esp;           // 하위 esp8266 객체 포인터
    ap_state_t state;         // 현재 AP/소켓 FSM 상태
    uint8_t is_connected;     // 최종 연결 완료 플래그 (0: 안됨, 1: 연결됨)
} ap_ctx_t;

// 전역 컨텍스트 선언
extern ap_ctx_t g_ap_ctx;

/* 함수 프로토타입 */

void ap_Init(ap_ctx_t *ctx, esp8266_t *esp);
void ap_ProcessFSM(ap_ctx_t *ctx);
uint8_t ap_IsConnected(ap_ctx_t *ctx);
void process_server_command(uint8_t *rcv_data, uint16_t rcv_len);

#endif /* __AP_H__ */