#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <stdint.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#define MAGIC_ID 0xAA55
#define TARGET_DEV "/dev/rfcomm0" // 블루투스 바인딩 포트
#define BT_BAUD    B9600
typedef enum { PKT_AUTH=0, PKT_DEV_DATA=1, PKT_CMD=2, PKT_KEY=3, PKT_STATUS=4, PKT_ACCESS_REQ=5, PKT_ACCESS_RES=6 } PacketType;
typedef enum { STATE_STANDBY, STATE_MONITORING } AppState;
#pragma pack(push, 1)
typedef struct { uint16_t magic; uint8_t type; uint16_t length; } PacketHeader;
#pragma pack(pop)
int bt_fd = -1;
// 블루투스 초기화 함수
void init_bluetooth() {
    bt_fd = open(TARGET_DEV, O_RDWR | O_NOCTTY | O_NDELAY);
    if (bt_fd == -1) {
        printf("[BT ERR] %s 열기 실패. rfcomm bind를 먼저 하세요.\n", TARGET_DEV);
        return;
    }
    struct termios options;
    tcgetattr(bt_fd, &options);
    cfsetispeed(&options, BT_BAUD);
    cfsetospeed(&options, BT_BAUD);
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~CRTSCTS; // 흐름제어 끄기
    options.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(bt_fd, TCSANOW, &options);
    printf("[BT OK] 블루투스 연결 완료\n");
}
int read_full(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t r = read(fd, (char *)buf + total_read, count - total_read);
        if (r <= 0) return -1;
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
    init_bluetooth();
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
                        if (strlen(buf) == 0) strcpy(buf, "ENTER");
                        snprintf(payload, sizeof(payload), "%s:%s", target_device, buf);
                        send_packet(sock, PKT_KEY, payload);
                    }
               break;
            }
        }
        // 2. 서버 데이터 수신 처리
        if (FD_ISSET(sock, &temp)) {
            PacketHeader h;
            if (read_full(sock, &h, sizeof(h)) <= 0) break;
            if (h.magic != MAGIC_ID) break;
            char recv_buf[2048] = {0}; 
            uint16_t read_len = (h.length < sizeof(recv_buf) - 1) ? h.length : (sizeof(recv_buf) - 1);
            if (read_len > 0) {
                if (read_full(sock, recv_buf, read_len) <= 0) break;
                if (h.type == PKT_ACCESS_RES && strcmp(recv_buf, "SUCCESS") == 0) { 
                    current_state = STATE_MONITORING; 
                    printf(">>> [시작]\n"); 
                }
                else if (h.type == PKT_DEV_DATA) {
                    printf("%s", recv_buf);
                    fflush(stdout); 
                }
                // 블루투스 수신 데이터 파싱
                if (h.type == PKT_DEV_DATA || h.type == PKT_STATUS) {
                    if (strstr(recv_buf, "1,1") != NULL) {
                        write(bt_fd, "1", 1);
                        tcdrain(bt_fd);
                    } else if (strstr(recv_buf, "0,0") != NULL) {
                        write(bt_fd, "0", 1);
                        tcdrain(bt_fd);
                    }
                }
            }
        }
    }
    if (bt_fd != -1) close(bt_fd);
    close(sock); 
    return 0;
}
