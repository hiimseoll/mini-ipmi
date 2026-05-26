#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdint.h>

#define SERVER_IP "10.10.16.83" // 서버 IP 입력
#define SERVER_PORT 9000
#define DEVICE_ID "RPI01"
#define DEVICE_PW "1234"

// TLV 프로토콜 헤더
#pragma pack(push, 1)
typedef struct { uint16_t magic; uint8_t type; uint16_t length; } PacketHeader;
#pragma pack(pop)

// 패킷 전송 함수
void send_packet(int sock, uint8_t type, const char *payload) {
    PacketHeader h = { 0xAA55, type, (uint16_t)strlen(payload) };
    write(sock, &h, sizeof(h));
    write(sock, payload, h.length);
}

int main() {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in adr = { .sin_family = AF_INET, .sin_addr.s_addr = inet_addr(SERVER_IP), .sin_port = htons(SERVER_PORT) };

    if (connect(sock, (struct sockaddr*)&adr, sizeof(adr)) == -1) {
        perror("Connect failed");
        return 1;
    }

    // 1. 인증 패킷 전송 (Type 0)
    char auth_payload[64];
    sprintf(auth_payload, "%s:%s", DEVICE_ID, DEVICE_PW);
    send_packet(sock, 0, auth_payload);
    printf("인증 시도: %s\n", auth_payload);

    // 2. 메시지 수신 루프
    PacketHeader h;
    char payload[1024];

    while (read(sock, &h, sizeof(h)) > 0) {
        if (h.magic != 0xAA55) continue;
        
        memset(payload, 0, sizeof(payload));
        if (h.length < sizeof(payload)) {
            read(sock, payload, h.length);
        } else {
            read(sock, payload, sizeof(payload)-1); // 버퍼 오버플로우 방지
        }

        // 수신 데이터 타입별 처리
        switch (h.type) {
            case 2: // 관리자가 보낸 제어 명령
                printf("[COMMAND Received] %s\n", payload);
                if (strcmp(payload, "RESET") == 0) {
                    printf("System Reset triggered!\n");
                    // 실제 RESET 로직 구현 공간
                }
                break;

            case 3: // 관리자가 보낸 키보드 입력
                printf("[KEY INPUT Received] %s\n", payload);
                // 여기에 키보드 입력에 대한 실제 동작 구현
                break;

            default:
                printf("Unknown Packet Type: %d\n", h.type);
                break;
        }
    }

    close(sock);
    return 0;
}
