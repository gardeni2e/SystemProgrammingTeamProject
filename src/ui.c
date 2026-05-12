#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>

#include <ncursesw/ncurses.h>

#include "menu.h"
#include "order.h"
#include "protocol.h"
#include "server.h"
#include "storage.h"
#include "ui.h"

#define MENU_PATH "data/menu.csv"
#define CONFIG_PATH "data/tables.conf"
#define SALES_LOG_PATH "data/sales.log"

static void ui_locale_utf8(void) {
    setlocale(LC_ALL, "");
}

static void ui_copy_banner(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) {
        return;
    }
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

static void ui_prefix_banner(char *dst, size_t dstsz, const char *prefix,
                             const char *src) {
    if (!dst || dstsz == 0) {
        return;
    }
    size_t pl = strnlen(prefix, dstsz - 1);
    memcpy(dst, prefix, pl);
    strncpy(dst + pl, src, dstsz - pl - 1);
    dst[dstsz - 1] = '\0';
}

typedef struct {
    int fd;
    unsigned char buf[8192];
    size_t len;
} UiSockReader;

typedef struct {
    int sock;
    int stop;
    pthread_mutex_t mu;
    char q[128][MAX_PROTO_LINE];
    int qn;
} UiNetQueue;

static int ui_write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int ui_sock_read_line(UiSockReader *sr, char *out, size_t outsz) {
    if (outsz == 0) {
        return -1;
    }
    for (;;) {
        for (size_t i = 0; i < sr->len; ++i) {
            if (sr->buf[i] == '\n') {
                size_t take = i + 1;
                size_t copy = take - 1;
                if (copy >= outsz) {
                    copy = outsz - 1;
                }
                memcpy(out, sr->buf, copy);
                out[copy] = '\0';
                while (copy > 0 &&
                       (out[copy - 1] == '\r' || out[copy - 1] == '\n')) {
                    out[--copy] = '\0';
                }
                memmove(sr->buf, sr->buf + take, sr->len - take);
                sr->len -= take;
                return 1;
            }
        }
        if (sr->len >= sizeof(sr->buf)) {
            return -1;
        }
        ssize_t rd =
            read(sr->fd, sr->buf + sr->len, sizeof(sr->buf) - sr->len);
        if (rd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rd == 0) {
            return 0;
        }
        sr->len += (size_t)rd;
    }
}

static void ui_net_enqueue(UiNetQueue *nq, const char *line) {
    pthread_mutex_lock(&nq->mu);
    if (nq->qn < 128) {
        strncpy(nq->q[nq->qn], line, MAX_PROTO_LINE - 1);
        nq->q[nq->qn][MAX_PROTO_LINE - 1] = '\0';
        nq->qn++;
    }
    pthread_mutex_unlock(&nq->mu);
}

static int ui_net_pop(UiNetQueue *nq, char *out, size_t outsz) {
    pthread_mutex_lock(&nq->mu);
    if (nq->qn <= 0) {
        pthread_mutex_unlock(&nq->mu);
        return 0;
    }
    strncpy(out, nq->q[0], outsz - 1);
    out[outsz - 1] = '\0';
    for (int i = 1; i < nq->qn; ++i) {
        memcpy(nq->q[i - 1], nq->q[i], MAX_PROTO_LINE);
    }
    nq->qn--;
    pthread_mutex_unlock(&nq->mu);
    return 1;
}

static void *ui_net_reader_main(void *arg) {
    UiNetQueue *nq = arg;
    UiSockReader sr;
    memset(&sr, 0, sizeof(sr));
    sr.fd = nq->sock;
    char line[MAX_PROTO_LINE];
    while (!nq->stop && ui_sock_read_line(&sr, line, sizeof(line)) > 0) {
        ui_net_enqueue(nq, line);
    }
    return NULL;
}

static int ui_tcp_connect(const char *host, uint16_t port, char *err,
                          size_t errsz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errsz, "socket %s", strerror(errno));
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        snprintf(err, errsz, "bad host");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        snprintf(err, errsz, "connect %s", strerror(errno));
        return -1;
    }
    return fd;
}

static void ui_apply_menu_response(MenuCatalog *cat, const char *line) {
    const char *p = strstr(line, "MENU_RESPONSE|");
    if (!p) {
        return;
    }
    p += strlen("MENU_RESPONSE|");
    menu_init_catalog(cat);
    char buf[MAX_PROTO_LINE];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    char *seg = strtok_r(buf, ";", &save);
    while (seg) {
        MenuItem mi;
        if (proto_parse_menu_csv_segment(seg, &mi) == 0 && mi.id > 0 &&
            cat->count < MAX_MENU_ITEMS) {
            cat->items[cat->count++] = mi;
        }
        seg = strtok_r(NULL, ";", &save);
    }
}

static void ui_merge_order_mirror(Order *orders, int *count, const Order *incoming) {
    int idx = orders_find_index(orders, *count, incoming->order_id);
    if (idx >= 0) {
        orders[idx] = *incoming;
        return;
    }
    if (*count < MAX_ORDERS) {
        orders[(*count)++] = *incoming;
    }
}

static void ui_optional_chafa(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0) {
        clear();
        mvprintw(0, 0, "이미지 파일을 찾을 수 없습니다.");
        mvprintw(1, 0, "경로: %s", path);
        mvprintw(3, 0, "아무 키나 누르면 돌아갑니다.");
        refresh();
        getch();
        return;
    }

    // ncurses 화면 종료 후 일반 터미널 모드로 복귀
    def_prog_mode();
    endwin();

    pid_t pid = fork();

    if (pid == 0) {
        execlp("chafa", "chafa", "--size", "40x18", path, (char *)NULL);
        perror("chafa 실행 실패");
        _exit(127);
    }

    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);

        printf("\n[이미지 출력 종료] Enter를 누르면 메뉴로 돌아갑니다...");
        fflush(stdout);
        getchar();
    }

    // ncurses 화면 복구
    reset_prog_mode();
    refresh();
    clear();
}

typedef enum {
    POS_TAB_HOME = 0,
    POS_TAB_ORDERS,
    POS_TAB_MENU,
    POS_TAB_SETTINGS,
    POS_TAB_SALES
} PosTab;

static void ui_draw_pos_home(ServerContext *ctx, int rows, int cols) {
    clear();
    mvprintw(0, 0, "POS 홈 | 테이블 상태 (%d석 설정)",
             ctx->cfg.max_tables);
    mvprintw(2, 0, "%-*s", cols - 1, "테이블별 활성 주문 요약");
    int line = 4;
    pthread_mutex_lock(&ctx->lock);
    for (int t = 1; t <= ctx->cfg.max_tables && line < rows - 4; ++t) {
        int cnt = 0;
        int unpaid = 0;
        for (int i = 0; i < ctx->order_count; ++i) {
            if (ctx->orders[i].table_id != t) {
                continue;
            }
            cnt++;
            if (ctx->orders[i].status != STATUS_PAID) {
                unpaid++;
            }
        }
        mvprintw(line++, 2, "Table %02d : 총 %d건 / 미결제 %d건", t, cnt,
                 unpaid);
    }
    if (ctx->last_staff_message[0] != '\0') {
        mvprintw(rows - 4, 0, "[직원호출] %s (%lld초 전)",
                 ctx->last_staff_message,
                 ctx->last_staff_time
                     ? (long long)(time(NULL) - ctx->last_staff_time)
                     : 0LL);
    }
    pthread_mutex_unlock(&ctx->lock);
    mvprintw(rows - 2, 0,
             "F1 홈 F2 주문·결제 F3 메뉴관리 F4 설정 F5 매출 q 종료");
}

static void ui_draw_pos_orders(ServerContext *ctx, int rows, int cols,
                               int sel) {
    (void)cols;
    clear();
    mvprintw(0, 0, "결제 대기(DONE) 주문 (↑↓ 선택 Enter 결제)");
    pthread_mutex_lock(&ctx->lock);
    int shown = 0;
    for (int i = 0; i < ctx->order_count && shown < rows - 6; ++i) {
        Order *o = &ctx->orders[i];
        if (o->status != STATUS_DONE) {
            continue;
        }
        attrset(shown == sel ? A_REVERSE : A_NORMAL);
        mvprintw(3 + shown, 0,
                 "#%04d T%02d %-8s ₩%-6d lines:%d", o->order_id, o->table_id,
                 status_to_string(o->status), o->total_price, o->item_count);
        attrset(A_NORMAL);
        shown++;
    }
    pthread_mutex_unlock(&ctx->lock);
    mvprintw(rows - 2, 0, "새로고침 r");
}

static int ui_pos_collect_done_ids(ServerContext *ctx, int *ids,
                                   int max_ids) {
    int n = 0;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->order_count && n < max_ids; ++i) {
        if (ctx->orders[i].status == STATUS_DONE) {
            ids[n++] = ctx->orders[i].order_id;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return n;
}

static void ui_draw_pos_menu_admin(ServerContext *ctx, int rows, int cols,
                                   int sel) {
    (void)cols;
    clear();
    mvprintw(0, 0, "메뉴 관리 (↑↓) a추가 e수정 d삭제 s품절토글 S저장");
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->menu.count && i + 3 < rows - 3; ++i) {
        MenuItem *m = &ctx->menu.items[i];
        attrset(i == sel ? A_REVERSE : A_NORMAL);
        mvprintw(3 + i, 0, "%3d %-28s ₩%-6d %s", m->id, m->name, m->price,
                 m->sold_out ? "[품절]" : "");
        attrset(A_NORMAL);
    }
    pthread_mutex_unlock(&ctx->lock);
}

static void ui_prompt_string(WINDOW *w, int row, const char *label,
                             char *buf, size_t bufsz) {
    echo();
    curs_set(1);
    mvprintw(row, 0, "%s", label);
    clrtoeol();
    wgetnstr(w, buf, (int)bufsz - 1);
    noecho();
    curs_set(0);
}

static void ui_prompt_int(WINDOW *w, int row, const char *label, int *out) {
    char buf[64];
    ui_prompt_string(w, row, label, buf, sizeof(buf));
    *out = atoi(buf);
}

static void ui_draw_pos_settings(ServerContext *ctx, int rows, int cols) {
    (void)cols;
    clear();
    mvprintw(0, 0, "매장 설정");
    mvprintw(2, 0, "현재 테이블 수: %d", ctx->cfg.max_tables);
    mvprintw(4, 0, "t: 테이블 수 변경 후 저장");
    mvprintw(rows - 2, 0, "현재 파일: %s", CONFIG_PATH);
}

static void ui_draw_pos_sales(int rows, int cols) {
    clear();
    mvprintw(0, 0, "매출 로그 tail (%s)", SALES_LOG_PATH);
    char chunk[16000];
    char err[128];
    ssize_t n =
        storage_read_tail(SALES_LOG_PATH, chunk, sizeof(chunk), err, sizeof(err));
    if (n < 0) {
        mvprintw(3, 0, "(로그 없음 또는 읽기 실패)");
        return;
    }
    int line = 2;
    char *save = NULL;
    char bufcopy[sizeof(chunk)];
    strncpy(bufcopy, chunk, sizeof(bufcopy) - 1);
    bufcopy[sizeof(bufcopy) - 1] = '\0';
    char *ln = strtok_r(bufcopy, "\n", &save);
    while (ln && line < rows - 2) {
        mvprintw(line++, 0, "%.*s", cols - 1, ln);
        ln = strtok_r(NULL, "\n", &save);
    }
}

void ui_run_pos(ServerContext *ctx) {
    ui_locale_utf8();
    initscr();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    timeout(200);
    PosTab tab = POS_TAB_HOME;
    int sel_order = 0;
    int sel_menu = 0;
    uint32_t last_rev = 0;

    while (!ctx->shutting_down) {
        int rows = 0, cols = 0;
        getmaxyx(stdscr, rows, cols);
        if ((uint32_t)ctx->orders_revision != last_rev) {
            last_rev = (uint32_t)ctx->orders_revision;
            sel_order = 0;
        }

        switch (tab) {
        case POS_TAB_HOME:
            ui_draw_pos_home(ctx, rows, cols);
            break;
        case POS_TAB_ORDERS:
            ui_draw_pos_orders(ctx, rows, cols, sel_order);
            break;
        case POS_TAB_MENU:
            ui_draw_pos_menu_admin(ctx, rows, cols, sel_menu);
            break;
        case POS_TAB_SETTINGS:
            ui_draw_pos_settings(ctx, rows, cols);
            break;
        case POS_TAB_SALES:
            ui_draw_pos_sales(rows, cols);
            break;
        }

        int ch = wgetch(stdscr);
        if (ch == 'q' || ch == 'Q') {
            ctx->shutting_down = 1;
            break;
        }
        if (ch == KEY_F(1)) {
            tab = POS_TAB_HOME;
        }
        if (ch == KEY_F(2)) {
            tab = POS_TAB_ORDERS;
        }
        if (ch == KEY_F(3)) {
            tab = POS_TAB_MENU;
        }
        if (ch == KEY_F(4)) {
            tab = POS_TAB_SETTINGS;
        }
        if (ch == KEY_F(5)) {
            tab = POS_TAB_SALES;
        }

        if (tab == POS_TAB_ORDERS) {
            int ids[MAX_ORDERS];
            int nids = ui_pos_collect_done_ids(ctx, ids, MAX_ORDERS);
            if (ch == KEY_UP) { 
                sel_order = (sel_order + nids - 1) % (nids ? nids : 1);
            }
            if (ch == KEY_DOWN) {
                sel_order = (sel_order + 1) % (nids ? nids : 1);
            }
            if (ch == '\n' || ch == KEY_ENTER) {
                if (nids > 0 && sel_order < nids) {
                    char err[160];
                    server_pay_order(ctx, ids[sel_order], err, sizeof(err));
                }
            }
        }

        if (tab == POS_TAB_MENU) {
            pthread_mutex_lock(&ctx->lock);
            int mc = ctx->menu.count;
            pthread_mutex_unlock(&ctx->lock);
            if (ch == KEY_UP) {
                sel_menu = (sel_menu + mc - 1) % (mc ? mc : 1);
            }
            if (ch == KEY_DOWN) {
                sel_menu = (sel_menu + 1) % (mc ? mc : 1);
            }
            if ((ch == 's' || ch == 'S') && mc > 0) {
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                int id = ctx->menu.items[sel_menu].id;
                int cur = ctx->menu.items[sel_menu].sold_out;
                menu_set_soldout(&ctx->menu, id, cur ? 0 : 1, err,
                                 sizeof(err));
                pthread_mutex_unlock(&ctx->lock);
                server_broadcast_line(ctx, "MENU_SYNC\n");
                server_save_config(ctx, err, sizeof(err));
                pthread_mutex_lock(&ctx->lock);
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);
            }
            if (ch == 'd' && mc > 0) {
                pthread_mutex_lock(&ctx->lock);
                int id = ctx->menu.items[sel_menu].id;
                pthread_mutex_unlock(&ctx->lock);
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                menu_delete_item(&ctx->menu, id, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);
                pthread_mutex_lock(&ctx->lock);
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);
                server_broadcast_line(ctx, "MENU_SYNC\n");
                sel_menu = 0;
            }
            if (ch == 'a') {
                MenuItem mi;
                memset(&mi, 0, sizeof(mi));
                char namebuf[MAX_NAME];
                namebuf[0] = '\0';
                ui_prompt_string(stdscr, rows - 6, "이름: ", namebuf,
                                 sizeof(namebuf));
                int price = 0;
                ui_prompt_int(stdscr, rows - 5, "가격: ", &price);
                strncpy(mi.name, namebuf, sizeof(mi.name) - 1);
                mi.price = price;
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                menu_add_item(&ctx->menu, &mi, err, sizeof(err));
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);
                server_broadcast_line(ctx, "MENU_SYNC\n");
            }
            if (ch == 'e' && mc > 0) {
                pthread_mutex_lock(&ctx->lock);
                int id = ctx->menu.items[sel_menu].id;
                pthread_mutex_unlock(&ctx->lock);
                MenuItem mi;
                pthread_mutex_lock(&ctx->lock);
                mi = ctx->menu.items[sel_menu];
                pthread_mutex_unlock(&ctx->lock);
                char nb[MAX_NAME];
                strncpy(nb, mi.name, sizeof(nb) - 1);
                nb[sizeof(nb) - 1] = '\0';
                ui_prompt_string(stdscr, rows - 6, "새 이름: ", nb,
                                 sizeof(nb));
                ui_prompt_int(stdscr, rows - 5, "새 가격: ", &mi.price);
                strncpy(mi.name, nb, sizeof(mi.name) - 1);
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                menu_update_item(&ctx->menu, id, &mi, err, sizeof(err));
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);
                server_broadcast_line(ctx, "MENU_SYNC\n");
            }
        }

        if (tab == POS_TAB_SETTINGS && ch == 't') {
            int nt = 0;
            ui_prompt_int(stdscr, rows - 4, "새 테이블 수: ", &nt);
            if (nt > 0 && nt <= MAX_TABLES) {
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                ctx->cfg.max_tables = nt;
                pthread_mutex_unlock(&ctx->lock);
                server_save_config(ctx, err, sizeof(err));
            }
        }
    }

    endwin();
}

typedef enum {
    TABLE_SCR_MENU = 0,
    TABLE_SCR_CART,
    TABLE_SCR_CONFIRM,
    TABLE_SCR_STATUS
} TableScreen;

static void ui_send_line(int sock, const char *s) {
    ui_write_all(sock, s, strlen(s));
}

static void ui_draw_table_menu(const MenuCatalog *cat, TableScreen scr,
                               const Cart *cart, int rows, int cols, int sel,
                               int table_id, const char *banner) {
    (void)cols;
    clear();
    mvprintw(0, 0, "Table %d | 화면:%s | 장바구니 줄 %d", table_id,
             scr == TABLE_SCR_MENU ? "메뉴"
             : scr == TABLE_SCR_CART ? "장바구니"
             : scr == TABLE_SCR_CONFIRM ? "확정"
                                      : "상태",
             cart->count);
    if (banner && banner[0]) {
        mvprintw(1, 0, "%s", banner);
    }
    int base = 3;
    if (scr == TABLE_SCR_MENU) {
        for (int i = 0; i < cat->count && base + i < rows - 4; ++i) {
            const MenuItem *m = &cat->items[i];
            attrset(i == sel ? A_REVERSE : A_NORMAL);
            mvprintw(base + i, 0, "%3d %-26s ₩%-6d %s", m->id, m->name,
                     m->price, m->sold_out ? "[품절]" : "");
            attrset(A_NORMAL);
        }
        mvprintw(rows - 4, 0,
                 "SPACE 수량+, c 장바구니 가기 o 주문상태 k 직원호출 i 이미지");
    } else if (scr == TABLE_SCR_CART) {
    int line = base;
    int footer_line = rows - 2;
    int max_line = footer_line - 1;

    if (cart->count == 0) {
        mvprintw(line, 0, "장바구니가 비어 있습니다.");
    } else {
        for (int i = 0; i < cart->count && line <= max_line; ++i) {
            attrset(i == sel ? A_REVERSE : A_NORMAL);
            mvprintw(line++, 0, "%2d. %-26s x%d  @%d  = ₩%d",
                     i + 1,
                     cart->items[i].name,
                     cart->items[i].qty,
                     cart->items[i].price,
                     cart->items[i].qty * cart->items[i].price);
            attrset(A_NORMAL);
        }
    }
        mvprintw(footer_line, 0,
             "+/- 수량 r삭제 z주문확정화면 m메뉴 | 합계 ₩%d",cart_total(cart));
    }
    else if (scr == TABLE_SCR_CONFIRM) {
        mvprintw(base, 0, "주문 합계: ₩%d", cart_total(cart));
        mvprintw(base + 2, 0, "Enter 확정 ESC 취소");
    } else if (scr == TABLE_SCR_STATUS) {
        mvprintw(base, 0, "%s", "네트워크 동기화된 주문 상태");
    }
}

static void ui_draw_table_status_lines(Order *orders, int count, int table_id,
                                       int rows, int cols, int base) {
    (void)cols;
    int line = base;
    for (int i = 0; i < count && line < rows - 3; ++i) {
        Order *o = &orders[i];
        if (o->table_id != table_id) {
            continue;
        }
        mvprintw(line++, 0, "#%04d %-8s ₩%d · ", o->order_id,
                 status_to_string(o->status), o->total_price);
        for (int k = 0; k < o->item_count && k < 4; ++k) {
            printw("%s x%d ", o->items[k].name, o->items[k].qty);
        }
    }
}

void ui_run_table(const TableUiArgs *args) {
    ui_locale_utf8();
    initscr();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    timeout(150);

    char err[160];
    int sock = ui_tcp_connect(args->host, args->port, err, sizeof(err));
    if (sock < 0) {
        endwin();
        fprintf(stderr, "%s\n", err);
        return;
    }

    UiNetQueue nq;
    memset(&nq, 0, sizeof(nq));
    nq.sock = sock;
    pthread_mutex_init(&nq.mu, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, ui_net_reader_main, &nq);

    char hello[64];
    snprintf(hello, sizeof(hello), "HELLO TABLE %d\n", args->table_id);
    ui_send_line(sock, hello);

    // 초기 메뉴 로드 (서버에서 안 오면 빈 메뉴로 시작) fallback
    MenuCatalog cat;
    menu_init_catalog(&cat);

    char menu_err[160];
    if (menu_load_file(&cat, "data/menu.csv", menu_err, sizeof(menu_err)) != 0) {
        menu_init_catalog(&cat);
    }
    Order mirror[MAX_ORDERS];
    int mirror_count = 0;
    memset(mirror, 0, sizeof(mirror));

    Cart cart;
    cart_init(&cart);
    TableScreen scr = TABLE_SCR_MENU;
    int sel = 0;
    char banner[160];
    banner[0] = '\0';

    int handshake_ok = 0;
    while (!handshake_ok) {
        char tmp[MAX_PROTO_LINE];
        if (!ui_net_pop(&nq, tmp, sizeof(tmp))) {
            napms(15);
            continue;
        }
        if (strncmp(tmp, "OK|", 3) == 0) {
            handshake_ok = 1;
            break;
        }
        if (strncmp(tmp, "MENU_RESPONSE|", 14) == 0) {
            ui_apply_menu_response(&cat, tmp);
        } else if (strncmp(tmp, "ORDER_EVENT|", 12) == 0) {
            Order ordtmp;
            char er[80];
            if (proto_parse_order_broadcast(tmp, &ordtmp, er, sizeof(er)) ==
                0) {
                ui_merge_order_mirror(mirror, &mirror_count, &ordtmp);
            }
        } else if (strncmp(tmp, "STAFF_CALL|", 11) == 0) {
            ui_copy_banner(banner, sizeof(banner), tmp);
        }
    }

    ui_send_line(sock, "MENU_REQUEST\n");

    while (1) {
        int rows = 0, cols = 0;
        getmaxyx(stdscr, rows, cols);

        char netline[MAX_PROTO_LINE];
        while (ui_net_pop(&nq, netline, sizeof(netline))) {
            if (strncmp(netline, "MENU_RESPONSE|", 14) == 0) {
                ui_apply_menu_response(&cat, netline);
                snprintf(banner, sizeof(banner), "메뉴 동기화 완료");
            } else if (strncmp(netline, "ORDER_EVENT|", 12) == 0) {
                Order tmp;
                char er[80];
                if (proto_parse_order_broadcast(netline, &tmp, er, sizeof(er)) ==
                    0) {
                    ui_merge_order_mirror(mirror, &mirror_count, &tmp);
                }
            } else if (strncmp(netline, "MENU_SYNC", 7) == 0) {
                ui_send_line(sock, "MENU_REQUEST\n");
            } else if (strncmp(netline, "STAFF_CALL|", 11) == 0) {
                ui_copy_banner(banner, sizeof(banner), netline);
            } else if (strncmp(netline, "ORDER_CREATED|", 14) == 0) {
                ui_prefix_banner(banner, sizeof(banner), "서버:", netline);
            } else if (strncmp(netline, "ERROR|", 6) == 0) {
                ui_copy_banner(banner, sizeof(banner), netline);
            }
        }

        if (scr != TABLE_SCR_STATUS) {
            ui_draw_table_menu(&cat, scr, &cart, rows, cols, sel, args->table_id,
                               banner);
        } else {
            clear();
            mvprintw(0, 0, "Table %d 주문 상태", args->table_id);
            ui_draw_table_status_lines(mirror, mirror_count, args->table_id,
                                       rows, cols, 3);
            mvprintw(rows - 2, 0, "m 메뉴화면 k 직원호출");
        }

        int ch = wgetch(stdscr);
        if (ch == 'q' || ch == 'Q') {
            break;
        }

        if (scr == TABLE_SCR_MENU) {
            int mc = cat.count;
            if (ch == KEY_UP) {
                sel = (sel + mc - 1) % (mc ? mc : 1);
            }
            if (ch == KEY_DOWN) {
                sel = (sel + 1) % (mc ? mc : 1);
            }
            if (ch == ' ') {
                if (mc > 0) {
                    const MenuItem *m = &cat.items[sel];
                    if (!m->sold_out) {
                        CartItem ci;
                        memset(&ci, 0, sizeof(ci));
                        ci.menu_id = m->id;
                        strncpy(ci.name, m->name, sizeof(ci.name) - 1);
                        ci.price = m->price;
                        ci.qty = 1;
                        cart_add(&cart, &ci, err, sizeof(err));
                    } else {
                        snprintf(banner, sizeof(banner), "품절 메뉴입니다");
                    }
                }
            }
            if (ch == 'c') {
                scr = TABLE_SCR_CART;
                sel = 0;
            }
            if (ch == 'o') {
                scr = TABLE_SCR_STATUS;
            }
            if (ch == 'm') {
                scr = TABLE_SCR_MENU;
            }
            if (ch == 'k') {
                char buf[80];
                snprintf(buf, sizeof(buf), "CALL_STAFF|table=%d\n",
                         args->table_id);
                ui_send_line(sock, buf);
            }
            if (ch == 'i') {
                if (mc > 0) {
                    char path[128];
                    snprintf(path, sizeof(path), "data/img_%d.png",
                             cat.items[sel].id);
                    ui_optional_chafa(path);
                }
            }
        } else if (scr == TABLE_SCR_CART) {
            int lines = cart.count;
            if (ch == KEY_UP) {
                sel = (sel + lines - 1) % (lines ? lines : 1);
            }
            if (ch == KEY_DOWN) {
                sel = (sel + 1) % (lines ? lines : 1);
            }
            if ((ch == '+' || ch == '=') && lines > 0) {
                int mid = cart.items[sel].menu_id;
                cart_set_qty(&cart, mid, cart.items[sel].qty + 1, err,
                             sizeof(err));
            }
            if (ch == '-' && lines > 0) {
                int mid = cart.items[sel].menu_id;
                cart_set_qty(&cart, mid, cart.items[sel].qty - 1, err,
                             sizeof(err));
            }
            if (ch == 'r' && lines > 0) {
                cart_remove_line(&cart, cart.items[sel].menu_id);
                sel = 0;
            }
            if (ch == 'z') {
                scr = TABLE_SCR_CONFIRM;
            }
            if (ch == 'm') {
                scr = TABLE_SCR_MENU;
            }
        } else if (scr == TABLE_SCR_CONFIRM) {
            if (ch == 27) {
                scr = TABLE_SCR_CART;
            }
            if (ch == '\n' || ch == KEY_ENTER) {
                if (cart.count > 0) {
                    char payload[MAX_PROTO_LINE];
                    char items[MAX_PROTO_LINE];
                    items[0] = '\0';
                    for (int i = 0; i < cart.count; ++i) {
                        char chunk[64];
                        snprintf(chunk, sizeof(chunk), "%s%d:%d",
                                 i ? "," : "", cart.items[i].menu_id,
                                 cart.items[i].qty);
                        strncat(items, chunk, sizeof(items) - strlen(items) - 1);
                    }
                    snprintf(payload, sizeof(payload),
                             "ORDER_CREATE|table=%d|items=%.*s\n",
                             args->table_id,
                             (int)(sizeof(payload) - 48),
                             items);
                    ui_send_line(sock, payload);
                    snprintf(banner, sizeof(banner), "주문 전송 완료(응답대기)");
                    cart_init(&cart);
                    scr = TABLE_SCR_STATUS;
                }
            }
        } else if (scr == TABLE_SCR_STATUS) {
            if (ch == 'm') {
                scr = TABLE_SCR_MENU;
            }
            if (ch == 'k') {
                char buf[80];
                snprintf(buf, sizeof(buf), "CALL_STAFF|table=%d\n",
                         args->table_id);
                ui_send_line(sock, buf);
            }
        }
    }

    nq.stop = 1;
    shutdown(sock, SHUT_RDWR);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&nq.mu);
    close(sock);
    endwin();
}

static void ui_draw_kitchen(Order *orders, int count, int sel, int rows,
                             int cols) {
    (void)cols;
    clear();
    mvprintw(0, 0, "Kitchen Display | ↑↓ 선택 · c 조리중 · d 완료");
    int shown = 0;
    for (int i = 0; i < count && shown < rows - 5; ++i) {
        Order *o = &orders[i];
        if (o->status == STATUS_PAID) {
            continue;
        }
        attrset(shown == sel ? A_REVERSE : A_NORMAL);
        mvprintw(3 + shown, 0, "#%04d T%02d %-8s ₩%-6d", o->order_id,
                 o->table_id, status_to_string(o->status), o->total_price);
        attrset(A_NORMAL);
        mvprintw(3 + shown, 40, "%s",
                 o->item_count ? o->items[0].name : "-");
        shown++;
    }
}

static int ui_kitchen_collect_active(Order *orders, int count, int *ids,
                                     int max_ids) {
    int n = 0;
    for (int i = 0; i < count && n < max_ids; ++i) {
        if (orders[i].status != STATUS_PAID) {
            ids[n++] = orders[i].order_id;
        }
    }
    return n;
}

void ui_run_kitchen(const KitchenUiArgs *args) {
    ui_locale_utf8();
    initscr();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    timeout(120);

    char err[160];
    int sock = ui_tcp_connect(args->host, args->port, err, sizeof(err));
    if (sock < 0) {
        endwin();
        fprintf(stderr, "%s\n", err);
        return;
    }

    UiNetQueue nq;
    memset(&nq, 0, sizeof(nq));
    nq.sock = sock;
    pthread_mutex_init(&nq.mu, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, ui_net_reader_main, &nq);

    ui_send_line(sock, "HELLO KITCHEN\n");

    Order board[MAX_ORDERS];
    int bn = 0;
    memset(board, 0, sizeof(board));
    int sel = 0;

    int handshake_ok = 0;
    while (!handshake_ok) {
        char tmp[MAX_PROTO_LINE];
        if (!ui_net_pop(&nq, tmp, sizeof(tmp))) {
            napms(15);
            continue;
        }
        if (strncmp(tmp, "OK|", 3) == 0) {
            handshake_ok = 1;
            break;
        }
        if (strncmp(tmp, "ORDER_EVENT|", 12) == 0) {
            Order ordtmp;
            char er[80];
            if (proto_parse_order_broadcast(tmp, &ordtmp, er, sizeof(er)) ==
                0) {
                ui_merge_order_mirror(board, &bn, &ordtmp);
            }
        }
    }

    while (1) {
        int rows = 0, cols = 0;
        getmaxyx(stdscr, rows, cols);

        char netline[MAX_PROTO_LINE];
        while (ui_net_pop(&nq, netline, sizeof(netline))) {
            if (strncmp(netline, "ORDER_EVENT|", 12) == 0) {
                Order tmp;
                char er[80];
                if (proto_parse_order_broadcast(netline, &tmp, er, sizeof(er)) ==
                    0) {
                    ui_merge_order_mirror(board, &bn, &tmp);
                }
            } else if (strncmp(netline, "STAFF_CALL|", 11) == 0) {
                mvprintw(1, 0, "%s", netline);
            } else if (strncmp(netline, "MENU_SYNC", 7) == 0) {
                /* kitchen ignores menu sync */
            }
        }

        int ids[MAX_ORDERS];
        int nids = ui_kitchen_collect_active(board, bn, ids, MAX_ORDERS);
        ui_draw_kitchen(board, bn, sel, rows, cols);

        int ch = wgetch(stdscr);
        if (ch == 'q' || ch == 'Q') {
            break;
        }
        if (ch == KEY_UP) {
            sel = (sel + nids - 1) % (nids ? nids : 1);
        }
        if (ch == KEY_DOWN) {
            sel = (sel + 1) % (nids ? nids : 1);
        }
        if (nids > 0 && sel < nids) {
            int oid = ids[sel];
            if (ch == 'c') {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "ORDER_UPDATE|order_id=%d|status=COOKING\n", oid);
                ui_send_line(sock, buf);
            }
            if (ch == 'd') {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "ORDER_UPDATE|order_id=%d|status=DONE\n", oid);
                ui_send_line(sock, buf);
            }
        }
    }

    nq.stop = 1;
    shutdown(sock, SHUT_RDWR);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&nq.mu);
    close(sock);
    endwin();
}
