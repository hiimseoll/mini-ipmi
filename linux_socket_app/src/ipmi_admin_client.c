#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <stdint.h>
#define MAGIC_ID 0xAA55
typedef enum { PKT_AUTH=0, PKT_DEV_DATA=1, PKT_CMD=2, PKT_KEY=3, PKT_STATUS=4, PKT_ACCESS_REQ=5, PKT_ACCESS_RES=6 } PacketType;
typedef enum { STATE_STANDBY, STATE_MONITORING } AppState;
#pragma pack(push, 1)
typedef struct { uint16_t magic; uint8_t type; uint16_t length; } PacketHeader;
#pragma pack(pop)
// 패킷 전체 수신
int read_full(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t r = read(fd, (char *)buf + total_read, count - total_read);
        if (r <= 0) return -1; // 연결 끊김 감지
        total_read += r;
    }
    return total_read;
}
void send_packet(int sock, PacketType type, const char *payload) {
    PacketHeader h = { MAGIC_ID, type, (uint16_t)strlen(payload) };
    write(sock, &h, sizeof(h)); 
    write(sock, payload, h.length);
}
int main() {
    AppState current_state = STATE_STANDBY;
    char target_device[20] = {0};
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in adr = { .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("10.10.16.83"), .sin_port = htons(9000) };
    if (connect(sock, (struct sockaddr *)&adr, sizeof(adr)) == -1) {
        printf("[ERR] 서버 접속 실패\n");
        return 1;
    }
    send_packet(sock, PKT_AUTH, "ADMIN:1234");
    fd_set reads; FD_ZERO(&reads); FD_SET(0, &reads); FD_SET(sock, &reads);
    while(1) {
        fd_set temp = reads; 
        select(sock + 1, &temp, NULL, NULL, NULL);
        // 1. 키보드 입력 처리
        if (FD_ISSET(0, &temp)) {
            char buf[256], payload[512];
            if (!fgets(buf, sizeof(buf), stdin)) break;
            buf[strcspn(buf, "\n")] = 0;
            switch (current_state) {
                case STATE_STANDBY:
                    if (strncmp(buf, "ACCESS:", 7) == 0) { 
                        strcpy(target_device, buf + 7); 
                        send_packet(sock, PKT_ACCESS_REQ, target_device); 
                    }
                    break;
                case STATE_MONITORING:
                    if (strcmp(buf, "!QUIT!") == 0) { 
                        current_state = STATE_STANDBY; 
                        printf("\n>>> [종료]\n"); 
                        break; 
                    }
                    if (strlen(buf) >= 3 && buf[0] == '!' && buf[strlen(buf) - 1] == '!') {
                        char cmd[32] = {0}; 
                        strncpy(cmd, buf + 1, strlen(buf) - 2);
                        snprintf(payload, sizeof(payload), "%s:%s", target_device, cmd); 
                        send_packet(sock, PKT_CMD, payload);
                    } else {
                      // 빈 입력 시 ENTER로 처리
                        if (strlen(buf) == 0) {
                            strcpy(buf, "ENTER");
                        }
                        snprintf(payload, sizeof(payload), "%s:%s", target_device, buf);
                        send_packet(sock, PKT_KEY, payload);
                    }
               break;
            }
        }
        // 2. 서버 데이터 수신 처리
        if (FD_ISSET(sock, &temp)) {
            PacketHeader h;
            // 헤더 수신 실패 시 즉시 종료 (무한 침묵 방지)
            if (read_full(sock, &h, sizeof(h)) <= 0) {
                printf("\n[SYS] 서버와의 연결이 끊어졌습니다.\n");
                break;
            }
            // 매직 넘버 검증
            if (h.magic != MAGIC_ID) {
                printf("\n[ERR] 패킷 동기화 오류! 클라이언트를 재시작하세요.\n");
                break; 
            }
            char recv_buf[2048] = {0}; 
            uint16_t read_len = (h.length < sizeof(recv_buf) - 1) ? h.length : (sizeof(recv_buf) - 1);
            if (read_len > 0) {
                // 페이로드 수신 실패 처리
                if (read_full(sock, recv_buf, read_len) <= 0) {
                    printf("\n[SYS] 데이터 수신 중 끊어짐.\n");
                    break;
                }
                if (h.type == PKT_ACCESS_RES && strcmp(recv_buf, "SUCCESS") == 0) { 
                    current_state = STATE_MONITORING; 
                    printf(">>> [시작]\n"); 
                }
                else if (h.type == PKT_DEV_DATA) {
                    printf("%s", recv_buf);
                    fflush(stdout); 
                }
            }
        }
    }
    close(sock); 
    return 0;
}
