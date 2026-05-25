# Raspberry Pi 4B (Target Device) config.txt 설정

IPMI 원격 제어 시스템에서 관리 대상 장치인 라즈베리 파이 4B의 부팅 과정과 하드웨어 제어를 STM32와 연동하기 위해 수정한 커널 파라미터 및 디바이스 트리 오버레이(Device Tree Overlay) 설정 내역입니다.

## 1. 조건부 부팅 분기 (Recovery 모드 제어)
```ini
gpio=16=ip,pu
[gpio16=1]
cmdline=cmdline.txt
[gpio16=0]
cmdline=cmdline_recovery.txt
```
* **동작 방식**: 부팅 단계에서 GPIO 16번 핀의 상태를 감지하여 커널 파라미터를 분기합니다.
* **시스템 연동**: 원격 접속이 불가능한 장애 상황 시, STM32를 통해 라즈베리 파이의 GPIO 16번을 Low(GND)로 강제 풀다운시킨 후 부팅합니다. 이를 통해 최소 기능만 작동하는 복구(Recovery) OS 환경으로 진입시켜 하드웨어 기반 시스템 복구를 수행합니다.

### `cmdline_recovery.txt` 파라미터 구성 (`init=/bin/sh`)
`cmdline_recovery.txt`에는 다음과 같은 커널 파라미터가 주입되어 있습니다.
```text
console=tty1 console=ttyAMA0,115200 root=PARTUUID=d7b33b01-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=KR init=/bin/sh
```
* **핵심 기능**: `init=/bin/sh`를 통해 정상적인 `systemd` 초기화 과정을 생략하고, 커널 부팅 직후 시리얼 콘솔(`ttyAMA0`) 환경에서 Root 권한 쉘(Shell)을 실행합니다. 관리자는 STM32를 거쳐 해당 쉘에 접근하여 파일 시스템 복구(`fsck`) 등을 수행합니다.

## 2. 시리얼 콘솔 할당
```ini
dtoverlay=disable-bt
enable_uart=1
```
* **동작 방식**: 하드웨어 UART를 블루투스 대신 GPIO 핀(TX/RX)에 매핑하여 활성화합니다.
* **시스템 연동**: 일반적인 네트워크가 단절된 상황에서도, STM32가 해당 시리얼 콘솔을 중계하여 원격에서 터미널(CLI)에 접근할 수 있도록 독립형 관리 통로를 확보합니다.

## 3. 전원 상태 동기화 및 안전 종료
```ini
dtoverlay=gpio-poweroff,gpiopin=22,active_low=0
dtoverlay=gpio-shutdown,gpio_pin=3,active_low=1,gpio_pull=up
```
* **동작 방식**: 
  1. **gpio-poweroff (GPIO 22)**: 리눅스 OS가 완전히 종료(Halt)되었을 때 HIGH 신호를 출력합니다.
  2. **gpio-shutdown (GPIO 3)**: 해당 핀에 LOW 신호가 인가되면 리눅스 커널 수준에서 안전 종료(Graceful Shutdown) 프로세스를 시작합니다.
* **시스템 연동**: 파일 시스템 손상(Hard Reset)을 방지하기 위해 STM32가 GPIO 3번에 제어 신호를 인가하여 OS 안전 종료를 트리거합니다. 이후 GPIO 22번 출력을 모니터링하여 OS 셧다운 완료를 확인한 뒤 릴레이를 통해 물리적 전원을 최종 차단합니다.
