# 다중 터미널 POS 및 독립형 테이블 오더 시스템

## 팀 정보

- 이 저장소의 본문 또는 별도 제출 문서에 **팀명·학번·역할 분담**을 기입하세요.

## 프로젝트 개요

Ubuntu 24.04 로컬 네트워크 환경에서 외부 클라우드 없이 동작하는 테이블 오더·주방 디스플레이·POS 통합 시스템입니다. `pos_server` 단일 바이너리가 TCP 서버와 POS ncurses UI를 동시에 수행하고, `table_client`·`kitchen_client`가 각각 손님 단말과 주방 단말 역할을 합니다. 주문 상태는 `WAITING → COOKING → DONE → PAID`로 관리되며, 문자열 프로토콜과 파일 로그로 데이터를 지속화합니다.

## 주요 기능

1. **메뉴 CSV 관리 및 테이블 장바구니 주문**: `data/menu.csv`를 로드하고 품절 플래그를 검사한 뒤 `ORDER_CREATE`를 송신합니다.
2. **다중 클라이언트 소켓 서버**: `pthread` 기반 수락/세션 처리와 주문 이벤트 브로드캐스트로 실시간 동기화를 제공합니다.
3. **관리자 기능**: POS 화면에서 메뉴 CRUD·품절 토글·테이블 수 조정 후 설정 파일에 즉시 반영합니다.
4. **주방 상태 관리 & 직원 호출**: 주방 단말에서 상태 전이를 올리고, 테이블 단말은 `CALL_STAFF`로 POS/주방에 알림을 띄웁니다.
5. **결제 및 매출 로그**: `DONE` 주문만 결제 처리하여 `data/sales.log`에 매출 라인을 남기고 상태를 `PAID`로 전파합니다.
6. **SIGINT 안전 종료 & 선택적 chafa 미리보기**: Ctrl+C 시 소켓과 설정을 정리하고, `data/img_<id>.png`와 `chafa` 설치 시 이미지 미리보기를 제공합니다.

## 사용한 시스템콜 및 목적

| 계층 | 대표 syscall/API | 사용 목적 |
| --- | --- | --- |
| 네트워크 | `socket`, `bind`, `listen`, `accept`, `connect`, `shutdown`, `setsockopt` | IPv4 TCP 서버/클라이언트 채널 구축 및 종료 |
| I/O | `read`, `write`, `close`, `open`, `fsync` | 소켓 프레이밍·메뉴 CSV·설정·로그 파일 입출력 |
| 파일 메타 | `stat`, `lseek`, `rename` | 파일 존재/크기 확인, 매출 tail 조회, 원자적 저장 |
| 프로세스/동기화 | `fork`, `waitpid`, `pthread_*`, `sigaction` | 선택 이미지 렌더링 자식 프로세스, 서버 스레드 풀, SIGINT 처리 |

## 빌드 방법

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libncursesw6-dev
cd project
make
```

한글 UI는 **UTF-8 로케일 + `-lncursesw`(wide)** 조합으로 렌더링합니다(`ui.c`에서 `setlocale`, `<ncursesw/ncurses.h>` 사용). 터미널 인코딩이 UTF-8인지 확인하세요.

추가로 이미지 미리보기를 시험하려면 `sudo apt install -y chafa` 후 `data/img_<메뉴ID>.png` 파일을 배치합니다.

## 실행 방법

```bash
# 터미널 1
./bin/pos_server 9090

# 터미널 2
./bin/kitchen_client 127.0.0.1 9090

# 터미널 3
./bin/table_client 127.0.0.1 9090 1

# 터미널 4 (선택)
./bin/table_client 127.0.0.1 9090 2
```

Makefile 헬퍼:

```bash
make run-server PORT=9090
make run-kitchen HOST=127.0.0.1 PORT=9090
make run-table HOST=127.0.0.1 PORT=9090 TABLE=2
```

단축키 요약은 `docs/demo_scenario.md`를 참고합니다.

## 데모 시나리오

과제에서 요구한 15단계 시연은 `docs/demo_scenario.md`와 동일한 순서로 작성되어 있습니다. 요약하면 POS 기동 → 주방/테이블 클라이언트 접속 → 주문 전송 → 주방 상태 변경 → 테이블 동기화 확인 → POS 결제 및 `sales.log` 검증 → 품절 처리 및 차단 → Ctrl+C 종료 검증입니다.

## 테스트

```bash
make test
```

`tests/` 디렉터리의 소형 바이너리가 메뉴 입출력, 장바구니·주문 구조, 프로토콜 파서를 순차 검증합니다.

## 파일 구조

```
project/
├── Makefile
├── README.md
├── include/
│   ├── common.h
│   ├── protocol.h
│   ├── menu.h
│   ├── order.h
│   ├── storage.h
│   ├── server.h
│   └── ui.h
├── src/
│   ├── pos_server.c
│   ├── table_client.c
│   ├── kitchen_client.c
│   ├── protocol.c
│   ├── menu.c
│   ├── order.c
│   ├── storage.c
│   ├── server.c
│   ├── ui.c
│   └── common.c
├── data/
│   ├── menu.csv
│   ├── tables.conf
│   ├── orders.log
│   └── sales.log
├── docs/
│   └── demo_scenario.md
└── tests/
    ├── test_menu.c
    ├── test_order.c
    └── test_protocol.c
```

## 제한 사항 및 향후 개선점

- 인증/암호화 부재: 모든 단말을 신뢰 네트워크로 가정했으며 TLS나 토큰 검증이 없습니다.
- 단순 브로드캐스트 모델: 모든 세션에 동일 이벤트를 송신하고 클라이언트에서 필터링합니다. 대규모 매장에서는 구독 범위 최적화가 필요합니다.
- 영속 주문 큐 부재: 서버 비정상 종료 시 메모리 상 미결 주문은 재기동 후 재구성되지 않으며 로그 기반 복구는 향후 과제입니다.
- UI 국제화: 현재 문자열은 교육용 한글 메시지 위주이며 ICU 기반 포맷팅은 적용하지 않았습니다.
