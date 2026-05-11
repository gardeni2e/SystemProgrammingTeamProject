# 데모 시나리오 (과제 요구 순서)

1. `./bin/pos_server 9090` 로 POS 서버를 실행합니다.
2. `./bin/kitchen_client 127.0.0.1 9090` 로 Kitchen Client를 실행합니다.
3. `./bin/table_client 127.0.0.1 9090 1` 로 Table Client 1을 실행합니다.
4. `./bin/table_client 127.0.0.1 9090 2` 로 Table Client 2를 실행합니다.
5. Table 1에서 메뉴 목록을 확인합니다 (`MENU_REQUEST` 자동 전송 이후 응답 확인).
6. Table 1에서 장바구니를 구성하고 주문을 확정하여 서버로 전송합니다.
7. Kitchen 화면에서 신규 주문이 표시되는지 확인합니다.
8. Kitchen에서 해당 주문을 `COOKING` 상태로 변경합니다 (`c` 키).
9. Table 1 상태 화면에서 상태 변화가 반영되는지 확인합니다 (`o` 키 화면).
10. Kitchen에서 동일 주문을 `DONE` 상태로 변경합니다 (`d` 키).
11. POS에서 `F2` 결제 탭으로 이동 후 주문을 선택하고 Enter로 결제합니다.
12. `data/sales.log` 파일을 열어 매출 기록이 추가되었는지 확인합니다.
13. POS `F3` 메뉴 관리에서 특정 메뉴를 품절 처리합니다 (`s` 키 토글).
14. Table 2에서 품절 메뉴를 담으려 할 때 차단 메시지가 나타나는지 확인합니다.
15. POS에서 `q`를 누르거나 서버 터미널에서 `Ctrl+C`로 종료하면서 `data/tables.conf`, 로그 파일과 소켓 자원이 안전하게 정리되는지 확인합니다.

### 추가 팁

- `make run-server`, `make run-kitchen`, `make run-table TABLE=n` 타깃으로 빠르게 실행할 수 있습니다.
- `data/img_<메뉴번호>.png`와 `chafa` 패키지가 있으면 Table 클라이언트 `i` 키로 미리보기가 가능합니다.
