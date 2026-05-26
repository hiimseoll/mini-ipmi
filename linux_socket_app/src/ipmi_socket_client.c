#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
                       // 
#define SERVER_IP "10.10.16.83" // 서버 IP
#define SERVER_PORT 9000
#define MY_ID "RPI_01"        // 이 디바이스의 고유 ID

// GPIO 제어 함수
void gpio_write(int pin, int value) {
    char path[50];
    sprintf(path, "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    write(fd, value ? "1" : "0", 1);
    close(fd);
}

int main() {
    int sock;
    struct sockaddr_in serv_adr;
    char msg[1024];

    sock = socket(PF_INET, SOCK_STREAM, 0);
    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family = AF_INET;
    serv_adr.sin_addr.s_addr = inet_addr(SERVER_IP);
    serv_adr.sin_port = htons(SERVER_PORT);

    if(connect(sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1) {
        perror("Connect failed");
        return 1;
    }

    // 1. 서버에 ID 등록
    sprintf(msg, "REGISTER:%s", MY_ID);
    write(sock, msg, strlen(msg));

    // 2. 명령 수신 루프
    while(1) {
        int len = read(sock, msg, sizeof(msg)-1);
        if(len <= 0) break;
        msg[len] = 0;

        printf("Received Command: %s\n", msg);

        // 명령 파싱 및 GPIO 제어
        if(strcmp(msg, "RESET") == 0) {
            gpio_write(17, 1); usleep(500000); gpio_write(17, 0);
        } else if(strcmp(msg, "GPIO16_ON") == 0) {
            gpio_write(16, 1);
        } else if(strcmp(msg, "GPIO16_OFF") == 0) {
            gpio_write(16, 0);
        }
    }
    close(sock);
    return 0;
}
