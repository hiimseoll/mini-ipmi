# STM32 Firmware (KVM / IPMI Controller)

본 디렉터리는 타겟 디바이스(라즈베리 파이)의 하드웨어 전원 및 시리얼 로그를 직접 통제하는 독립형 관리 컨트롤러 펌웨어(STM32F4xx) 소스코드입니다. OS 크래시 시에도 시스템 복구를 수행할 수 있는 원격 하드웨어 릴레이 및 가상 키보드(USB HID) 역할을 담당합니다.

## 핵심 기능 및 설계 주안점

1. **FreeRTOS 기반 Task Decoupling**
   - Wi-Fi 관리(`wifi.c`), KVM 명령 제어(`ap.c`), 시리얼 모니터링 등 다수의 로직을 독립된 Task로 분리했습니다.
   - 네트워크 Latency가 하드웨어 제어 블로킹(Blocking)을 유발하지 않도록, Message Queue와 Mutex를 활용하여 데이터 무결성과 비동기 병렬 처리를 구현했습니다.

2. **UART DMA 및 통신 병목 해결**
   - 타겟 디바이스에서 대용량 로그 송출 시 발생하는 1바이트 수신 인터럽트 오버헤드와 버퍼 오버런 문제를 해결하기 위해, **UART DMA + IDLE Interrupt** 방식을 적용하여 CPU 간섭 없는 대용량 데이터 버퍼링(8KB) 환경을 구축했습니다.

3. **안전 전원 제어 (Graceful Shutdown) 및 KVM 모드 스위치**
   - 무자비한 하드 리셋(Hard Reset)으로 인한 파일 시스템 손상을 막기 위해, 대상 OS 커널의 상태 핀을 GPIO로 모니터링하여 안전 종료 시퀀스(`ap.c`)를 거친 뒤 릴레이 전원을 차단하도록 설계했습니다.
   - 가상 키보드 모드와 펌웨어 설정 모드 간의 KVM 상태 머신을 구축했습니다.

## 모듈 구성 (`MyApp/`)
- `ap.c` / `ap.h`: KVM 스위칭 상태 머신, OS 안전 종료 트리거 및 릴레이 비동기 제어 로직.
- `wifi.c` / `wifi.h`: 재연결(Ping Test) 및 접속 상태 감지, 서버 연결을 담당하는 Wi-Fi 상태 머신(FSM).
- `esp8266.c` / `esp8266.h`: ESP8266 AT 커맨드 래퍼 및 송수신 버퍼 큐잉 드라이버.
- `my_uart.c` / `my_uart.h`: UART 인터페이스 초기화 및 Ring Buffer, DMA 수신 제어 로직.
