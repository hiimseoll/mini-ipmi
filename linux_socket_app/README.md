# Linux Socket Application (Admin & Target Daemon)

네트워크 기반 원격 관리 시스템을 위한 TCP/IP 및 Bluetooth 소켓 클라이언트 프로그램 모음입니다. TLV(Type-Length-Value) 프로토콜을 차용하여 통신 패킷을 직렬화 및 역직렬화합니다.

## 핵심 기능 및 설계 주안점

1. **커스텀 패킷 프로토콜 (TLV)**
   - `[Magic Number (0xAA55)] + [Type] + [Length] + [Payload]` 구조를 통해 데이터 손실 및 패킷 찢어짐(Fragmentation)을 방지합니다.
   - `read_full` 형태의 누적 버퍼링 함수를 직접 구현하여 TCP 스트림 상의 프로토콜 파싱 오류를 원천 차단했습니다.

2. **I/O Multiplexing (Event-driven)**
   - `select()` 시스템 콜을 활용하여 표준 입력(키보드)과 소켓 수신 이벤트를 단일 스레드에서 비동기적으로 처리하여 자원 점유율을 최적화했습니다.

3. **다중 인터페이스 대역 외 관리 (OOB)**
   - 관리자는 TCP 통신(`ipmi_admin_client`)뿐만 아니라, 망이 완전히 단절된 로컬 환경에서도 Bluetooth 기반 시리얼 통신(`admin_linux`)을 통해 동일한 제어 및 모니터링을 수행할 수 있습니다.

## 컴포넌트 목록
- `src/admin_linux.c`: Bluetooth(`rfcomm0`)를 통한 로컬 관리자용 CLI 툴.
- `src/ipmi_admin_client.c`: TCP 소켓 기반 원격 관리자 클라이언트.
- `src/ipmi_socket_client.c`: 타겟 리눅스 기기용 데몬, `sysfs` 기반 제어 수행.
- `src/ipmi_socket_client_test.c`: 패킷 무결성 및 구조체 전송 단위 테스트용 더미 모듈.

## 빌드 방법
```bash
make
```
