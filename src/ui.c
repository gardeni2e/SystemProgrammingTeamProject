#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static int ui_get_png_size(const char *path, int *out_w, int *out_h) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    unsigned char buf[24];
    if (fread(buf, 1, sizeof(buf), fp) != sizeof(buf)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (buf[0] != 0x89 || buf[1] != 'P' || buf[2] != 'N' ||
        buf[3] != 'G') {
        return -1;
    }

    *out_w = ((int)buf[16] << 24) | ((int)buf[17] << 16) |
             ((int)buf[18] << 8) | (int)buf[19];
    *out_h = ((int)buf[20] << 24) | ((int)buf[21] << 16) |
             ((int)buf[22] << 8) | (int)buf[23];

    return 0;
}

static void ui_clear_rect(int y, int x, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int r = 0; r < h; ++r) {
        printf("\033[%d;%dH%*s", y + r + 1, x + 1, w, "");
    }
    fflush(stdout);
}

static void ui_draw_chafa_image(int y, int x, int w, int h, int menu_id) {
    char path[128];
    struct stat st;

    if (w <= 0 || h <= 0) {
        return;
    }

    snprintf(path, sizeof(path), "data/img_%d.png", menu_id);

    if (stat(path, &st) != 0) {
        const char *msg = "[no image]";
        int msg_x = x + (w - (int)strlen(msg)) / 2;
        int msg_y = y + h / 2;
        if (msg_x < x) {
            msg_x = x;
        }
        printf("\033[%d;%dH%s\033[0m", msg_y + 1, msg_x + 1, msg);
        fflush(stdout);
        return;
    }

    int img_w = 0;
    int img_h_px = 0;
    int draw_w = w;
    int draw_h = h;

    if (ui_get_png_size(path, &img_w, &img_h_px) == 0 &&
        img_w > 0 && img_h_px > 0) {
        draw_w = w;
        draw_h = (img_h_px * draw_w) / img_w / 2;

        if (draw_h < 1) {
            draw_h = 1;
        }
        if (draw_h > h) {
            draw_h = h;
            draw_w = (img_w * draw_h * 2) / img_h_px;
            if (draw_w < 1) {
                draw_w = 1;
            }
            if (draw_w > w) {
                draw_w = w;
            }
        }
    }

    int draw_x = x + (w - draw_w) / 2;
    int draw_y = y + (h - draw_h) / 2;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "chafa --size %dx%d --symbols=block --dither=ordered %s 2>/dev/null",
             draw_w, draw_h, path);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        const char *msg = "[image error]";
        printf("\033[%d;%dH%s\033[0m", y + h / 2 + 1, x + 2, msg);
        fflush(stdout);
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int r = 0;

    while (r < draw_h && (len = getline(&line, &cap, fp)) != -1) {
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        printf("\033[%d;%dH%s\033[0m", draw_y + r + 1, draw_x + 1, line);
        r++;
    }

    free(line);
    pclose(fp);
    printf("\033[0m");
    fflush(stdout);
}

#define CP_NORMAL        1
#define CP_SIDEBAR       2
#define CP_SIDEBAR_SEL   3
#define CP_CART_BTN      4
#define CP_CALL_BTN      5
#define CP_BADGE         6
#define CP_MENU_SEL      7
#define CP_FOOTER        8
#define CP_CARD          9

static void ui_init_colors(void) {
    if (!has_colors()) {
        return;
    }

    start_color();
    use_default_colors();

    init_pair(CP_NORMAL, COLOR_WHITE, -1);
    init_pair(CP_SIDEBAR, COLOR_WHITE, -1);
    init_pair(CP_SIDEBAR_SEL, COLOR_YELLOW, -1);
    init_pair(CP_CART_BTN, COLOR_GREEN, -1);
    init_pair(CP_CALL_BTN, COLOR_RED, -1);
    init_pair(CP_BADGE, COLOR_YELLOW, -1);
    init_pair(CP_MENU_SEL, COLOR_YELLOW, -1);
    init_pair(CP_FOOTER, COLOR_YELLOW, -1);
}

static void ui_attr_on(int pair) {
    if (has_colors()) {
        attron(COLOR_PAIR(pair));
    }
}

static void ui_attr_off(int pair) {
    if (has_colors()) {
        attroff(COLOR_PAIR(pair));
    }
}

static void ui_fill_rect_color(int y, int x, int w, int h, int color_pair) {
    ui_attr_on(color_pair);
    for (int r = 0; r < h; ++r) {
        mvprintw(y + r, x, "%*s", w, "");
    }
    ui_attr_off(color_pair);
}

static int ui_cart_total_qty(const Cart *cart) {
    int total = 0;
    if (!cart) {
        return 0;
    }
    for (int i = 0; i < cart->count; ++i) {
        total += cart->items[i].qty;
    }
    return total;
}

static void ui_draw_box(int y, int x, int w, int h) {
    if (w < 2 || h < 2) {
        return;
    }

    mvhline(y, x, ACS_HLINE, w - 1);
    mvhline(y + h - 1, x, ACS_HLINE, w - 1);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);

    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
}

static void ui_draw_card_box(int y, int x, int w, int h, int selected) {
    if (w < 2 || h < 2) {
        return;
    }

    if (selected) {
        mvhline(y, x, '#', w);
        mvhline(y + h - 1, x, '#', w);
        mvvline(y, x, '#', h);
        mvvline(y, x + w - 1, '#', h);
        mvaddch(y, x, '#');
        mvaddch(y, x + w - 1, '#');
        mvaddch(y + h - 1, x, '#');
        mvaddch(y + h - 1, x + w - 1, '#');
    } else {
        ui_draw_box(y, x, w, h);
    }
}

static void ui_draw_sidebar_button(int y, int x, int w,
                                   const char *label,
                                   int selected,
                                   int color_pair) {
    int inner_w = w - 2;

    if (has_colors()) {
        attron(COLOR_PAIR(color_pair));
    }

    if (selected) {
        attron(A_BOLD | A_REVERSE);
    } else {
        attron(A_BOLD);
    }

    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + 2, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, 3);
    mvvline(y, x + w - 1, ACS_VLINE, 3);

    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + 2, x, ACS_LLCORNER);
    mvaddch(y + 2, x + w - 1, ACS_LRCORNER);

    /* 버튼 내부 한 줄 전체를 먼저 비움 */
    mvprintw(y + 1, x + 1, "%-*s", inner_w, "");

    /* 라벨 출력 */
    mvprintw(y + 1, x + 2, "%-*.*s", inner_w - 2, inner_w - 2, label);

    if (selected) {
        attroff(A_BOLD | A_REVERSE);
    } else {
        attroff(A_BOLD);
    }

    if (has_colors()) {
        attroff(COLOR_PAIR(color_pair));
    }
}

static void ui_draw_badge(int y, int x, int count) {
    if (count <= 0) {
        return;
    }

    ui_attr_on(CP_BADGE);
    ui_draw_box(y, x, 4, 3);
    if (count > 9) {
        mvprintw(y + 1, x + 1, "9+");
    } else {
        mvprintw(y + 1, x + 1, "%d", count);
    }
    ui_attr_off(CP_BADGE);
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

typedef enum {
    TABLE_FOCUS_MENU = 0,
    TABLE_FOCUS_SIDEBAR
} TableFocus;

#define SIDEBAR_POPULAR 0
#define SIDEBAR_MEAL    1
#define SIDEBAR_MEAT    2
#define SIDEBAR_ALCOHOL 3
#define SIDEBAR_DRINK   4
#define SIDEBAR_CART    5
#define SIDEBAR_ORDER   6
#define SIDEBAR_CALL    7
#define SIDEBAR_COUNT   8

static const char *TABLE_CATEGORY_NAMES[] = {
    "인기 메뉴",
    "식사류",
    "고기류",
    "주류",
    "음료"
};

static void ui_send_line(int sock, const char *s) {
    ui_write_all(sock, s, strlen(s));
}

static int ui_submit_cart_order(int sock, int table_id, Cart *cart,
                                char *banner, size_t banner_sz) {
    char payload[MAX_PROTO_LINE];
    char items[MAX_PROTO_LINE];

    if (!cart || cart->count <= 0) {
        snprintf(banner, banner_sz, "장바구니가 비어 있습니다");
        return 0;
    }

    items[0] = '\0';

    for (int i = 0; i < cart->count; ++i) {
        char chunk[64];

        snprintf(chunk, sizeof(chunk), "%s%d:%d",
                 i ? "," : "",
                 cart->items[i].menu_id,
                 cart->items[i].qty);

        strncat(items, chunk, sizeof(items) - strlen(items) - 1);
    }

    snprintf(payload, sizeof(payload),
             "ORDER_CREATE|table=%d|items=%.*s\n",
             table_id,
             (int)(sizeof(payload) - 48),
             items);

    ui_send_line(sock, payload);

    snprintf(banner, banner_sz, "주문 전송 완료(응답대기)");
    cart_init(cart);

    return 1;
}

static int last_menu_sel = -1;
static int last_menu_page = -1;
static int last_menu_rows = -1;
static int last_menu_cols = -1;
static int last_menu_count = -1;
static int table_confirm_popup = 0;
static int table_call_popup_ticks = 0;
static TableScreen last_table_screen = -1;

static void ui_table_reset_images(void) {
    last_menu_page = -1;
    last_menu_rows = -1;
    last_menu_cols = -1;
    last_menu_count = -1;
    last_menu_sel = -1;
}

static int ui_menu_is_selectable(const MenuCatalog *cat, int idx) {
    if (!cat || idx < 0 || idx >= cat->count) {
        return 0;
    }

    return !cat->items[idx].sold_out;
}

static int ui_find_selectable_from(const MenuCatalog *cat, int start, int step) {
    int idx = start;

    if (!cat || cat->count <= 0 || step == 0) {
        return -1;
    }

    while (idx >= 0 && idx < cat->count) {
        if (ui_menu_is_selectable(cat, idx)) {
            return idx;
        }

        idx += step;
    }

    return -1;
}

static void ui_draw_table_header(int table_id, int cart_qty, int cols,
                                 const char *banner) {
    mvprintw(0, 0, "%-*s", cols - 1, "");
    mvprintw(0, 2, "Table %d | 테이블 오더", table_id);
    mvprintw(0, cols - 18, "장바구니 %d개", cart_qty);
    mvhline(1, 0, ACS_HLINE, cols - 1);

    if (banner && banner[0]) {
        mvprintw(1, 2, "%.*s", cols - 4, banner);
    }
}


static int cart_total_qty(const Cart *cart) {
    int total = 0;

    if (!cart) {
        return 0;
    }

    for (int i = 0; i < cart->count; ++i) {
        total += cart->items[i].qty;
    }

    return total;
}

static void ui_draw_simple_box(int y, int x, int w, int h) {
    if (w < 2 || h < 2) {
        return;
    }

    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + h - 1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);

    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
}

static void ui_draw_confirm_popup(int rows, int cols, const Cart *cart) {
    int w = 42;
    int h = 9;
    int y = rows / 2 - h / 2;
    int x = cols / 2 - w / 2;

    if (x < 1) {
        x = 1;
    }

    if (y < 1) {
        y = 1;
    }

    /*
     * 팝업 영역을 먼저 비운다.
     */
    for (int r = 0; r < h; ++r) {
        mvprintw(y + r, x, "%*s", w, "");
    }

    ui_draw_simple_box(y, x, w, h);

    attron(A_BOLD);
    mvprintw(y + 1, x + 2, "주문 확정 확인");
    attroff(A_BOLD);

    mvhline(y + 2, x + 1, ACS_HLINE, w - 2);

    mvprintw(y + 4, x + 4, "주문을 확정하시겠습니까?");

    if (cart) {
        mvprintw(y + 5, x + 4, "합계: %d원", cart_total(cart));
    }

    if (has_colors()) {
        attron(COLOR_PAIR(CP_MENU_SEL));
    }

    attron(A_BOLD);
    mvprintw(y + 7, x + 7, "y 확정");
    mvprintw(y + 7, x + 24, "n 취소");
    attroff(A_BOLD);

    if (has_colors()) {
        attroff(COLOR_PAIR(CP_MENU_SEL));
    }
}

static void ui_draw_call_popup(int rows, int cols) {
    int w = 34;
    int h = 7;
    int y = rows / 2 - h / 2;
    int x = cols / 2 - w / 2;

    if (x < 1) {
        x = 1;
    }

    if (y < 1) {
        y = 1;
    }

    for (int r = 0; r < h; ++r) {
        mvprintw(y + r, x, "%*s", w, "");
    }

    if (has_colors()) {
        attron(COLOR_PAIR(CP_CALL_BTN));
    }

    attron(A_BOLD);
    ui_draw_simple_box(y, x, w, h);
    mvprintw(y + 1, x + 2, "직원 호출");
    attroff(A_BOLD);

    if (has_colors()) {
        attroff(COLOR_PAIR(CP_CALL_BTN));
    }

    mvprintw(y + 3, x + 5, "직원을 호출하였습니다.");
    mvprintw(y + 5, x + 5, "잠시만 기다려주세요.");
}

static void ui_draw_table_menu(const MenuCatalog *cat, TableScreen scr,
                               const Cart *cart, int rows, int cols, int sel,
                               int table_id, const char *banner,
                               TableFocus focus, int sidebar_sel,
                               int current_category) {
    int base = 3;

    if (scr != TABLE_SCR_MENU) {
        clear();
        mvprintw(0, 0, "Table %d | 화면:%s | 장바구니 줄 %d", table_id,
                 scr == TABLE_SCR_CART ? "장바구니"
                 : scr == TABLE_SCR_CONFIRM ? "확정"
                                           : "상태",
                 cart->count);
        if (banner && banner[0]) {
            mvprintw(1, 0, "%s", banner);
        }
    }

    if (scr == TABLE_SCR_MENU) {
        const int sidebar_w = 24;
        const int grid_cols = 3;
        const int grid_rows = 2;
        const int page_size = grid_cols * grid_rows;

        int page = sel / page_size;
        int page_start = page * page_size;
        int total_pages = (cat->count + page_size - 1) / page_size;
        if (total_pages <= 0) {
            total_pages = 1;
        }

        int body_y = 2;
        int body_h = rows - 4;
        int menu_x = sidebar_w + 1;
        int menu_w = cols - menu_x;
        int footer_y = rows - 2;

        if (cols < 95 || rows < 28 || menu_w < 60) {
            clear();
            mvprintw(1, 2, "터미널 크기가 너무 작습니다.");
            mvprintw(3, 2, "사이드바 + 3x2 이미지 메뉴판을 보려면 터미널을 더 크게 해주세요.");
            mvprintw(5, 2, "권장 크기: 가로 100 이상, 세로 30 이상");
            refresh();
            return;
        }

        int need_image_redraw = 0;

        if (page != last_menu_page ||
            rows != last_menu_rows ||
            cols != last_menu_cols ||
            cat->count != last_menu_count){
            need_image_redraw = 1;
        }

        if (need_image_redraw) {
            clear();
        }

        ui_draw_table_header(table_id, ui_cart_total_qty(cart), cols, banner);

        ui_attr_on(CP_SIDEBAR);
        ui_draw_box(body_y, 0, sidebar_w, body_h);
        mvprintw(body_y + 1, 2, "카테고리");
        ui_attr_off(CP_SIDEBAR);

        for (int i = 0; i < 5; ++i) {
            int y = body_y + 3 + i * 2;
            int selected = (focus == TABLE_FOCUS_SIDEBAR && sidebar_sel == i);
            int active = (current_category == i);

            if (selected) {
                ui_attr_on(CP_SIDEBAR_SEL);
                mvprintw(y, 2, "▶ %-14s", TABLE_CATEGORY_NAMES[i]);
                ui_attr_off(CP_SIDEBAR_SEL);
            } else {
                ui_attr_on(CP_SIDEBAR);
                mvprintw(y, 2, "%s %-14s", active ? "●" : " ",
                         TABLE_CATEGORY_NAMES[i]);
                ui_attr_off(CP_SIDEBAR);
            }
        }

        int button_x = 2;
        int button_w = sidebar_w - 7;
        int cart_y = body_y + body_h - 10;
        int order_y = body_y + body_h - 7;
        int call_y = body_y + body_h - 4;
        int cart_qty = ui_cart_total_qty(cart);

        char cart_label[64];
        snprintf(cart_label, sizeof(cart_label), "🛒 CART [%d]",
                cart_total_qty(cart));

        ui_draw_sidebar_button(cart_y, button_x, button_w,
                            cart_label,
                            sidebar_sel == SIDEBAR_CART,
                            CP_CART_BTN);

        ui_draw_sidebar_button(order_y, button_x, button_w,
                            "📃 ORDER",
                            focus == TABLE_FOCUS_SIDEBAR &&
                            sidebar_sel == SIDEBAR_ORDER,
                            CP_MENU_SEL);

        ui_draw_sidebar_button(call_y, button_x, button_w,
                               "🔔 CALL",
                               focus == TABLE_FOCUS_SIDEBAR &&
                                   sidebar_sel == SIDEBAR_CALL,
                               CP_CALL_BTN);

        mvprintw(body_y, menu_x + 2, "%-*s", menu_w - 4, "");
        mvprintw(body_y, menu_x + 2, "%s", TABLE_CATEGORY_NAMES[current_category]);
        mvprintw(body_y, cols - 10, "%d / %d", page + 1, total_pages);

        int grid_top = body_y + 2;
        int grid_h = body_h - 7;
        int cell_gap_x = 2;
        int cell_gap_y = 1;
        int cell_w = (menu_w - 4 - (grid_cols - 1) * cell_gap_x) / grid_cols;
        int cell_h = (grid_h - (grid_rows - 1) * cell_gap_y) / grid_rows;

        if (cell_w < 16 || cell_h < 8) {
            mvprintw(body_y + 2, menu_x + 2, "메뉴 카드 영역이 너무 작습니다.");
            refresh();
            return;
        }

        for (int n = 0; n < page_size; ++n) {
            int idx = page_start + n;
            int gr = n / grid_cols;
            int gc = n % grid_cols;

            int y = grid_top + gr * (cell_h + cell_gap_y);
            int x = menu_x + 2 + gc * (cell_w + cell_gap_x);
            int inner_x = x + 1;
            int inner_y = y + 1;
            int inner_w = cell_w - 2;
            int img_h = cell_h - 4;
            int selected = (idx == sel && focus == TABLE_FOCUS_MENU);

            if (selected) {
                attron(A_BOLD);
                if (has_colors()) {
                    attron(COLOR_PAIR(CP_MENU_SEL));
                }

                mvhline(y, x, '#', cell_w);
                mvhline(y + cell_h - 1, x, '#', cell_w);
                mvvline(y, x, '#', cell_h);
                mvvline(y, x + cell_w - 1, '#', cell_h);

                if (has_colors()) {
                    attroff(COLOR_PAIR(CP_MENU_SEL));
                }
                attroff(A_BOLD);
            } else {
                mvhline(y, x, ACS_HLINE, cell_w);
                mvhline(y + cell_h - 1, x, ACS_HLINE, cell_w);
                mvvline(y, x, ACS_VLINE, cell_h);
                mvvline(y, x + cell_w - 1, ACS_VLINE, cell_h);
            }

            ui_draw_card_box(y, x, cell_w, cell_h, selected);

            if (idx < cat->count) {
                const MenuItem *m = &cat->items[idx];

                if (selected) {
                    attron(A_REVERSE | A_BOLD);
                }

                mvprintw(y + cell_h - 3, inner_x, "%-*.*s",
                         inner_w, inner_w, m->name);

            int text_x = x + 1;
            int name_y = y + cell_h - 3;
            int price_y = y + cell_h - 2;
            int text_w = cell_w - 2;

            /* 기존 글자 잔상 제거 */
            mvprintw(name_y, text_x, "%-*s", text_w, "");
            mvprintw(price_y, text_x, "%-*s", text_w, "");

            /* 메뉴 이름 */
            mvprintw(name_y, text_x, "%-*.*s", text_w, text_w, m->name);

            /* 가격 또는 품절 */
            if (m->sold_out) {
                attron(A_DIM);
            }

            mvprintw(name_y, text_x, "%-*.*s", text_w, text_w, m->name);

            if (m->sold_out) {
                mvprintw(price_y, text_x, "%-*.*s", text_w, text_w, "[품절]");
            } else {
                char price_buf[32];
                snprintf(price_buf, sizeof(price_buf), "%d원", m->price);
                mvprintw(price_y, text_x, "%-*.*s", text_w, text_w, price_buf);
            }

            if (m->sold_out) {
                attroff(A_DIM);
            }
            if (selected) {
                    attroff(A_REVERSE | A_BOLD);
                }
            } else {
                mvprintw(y + cell_h - 3, inner_x, "%-*s", inner_w, "");
                mvprintw(y + cell_h - 2, inner_x, "%-*s", inner_w, "");
            }

            if (selected) {
                attroff(A_BOLD);
                ui_attr_off(CP_MENU_SEL);
            } else {
                ui_attr_off(CP_CARD);
            }
        }

        int info_y = footer_y - 1;
        mvhline(info_y - 1, menu_x, ACS_HLINE, menu_w - 1);

        if (cat->count > 0 && sel >= 0 && sel < cat->count) {
            const MenuItem *m = &cat->items[sel];
            if (m->sold_out) {
                mvprintw(info_y, menu_x + 2,
                         "선택 메뉴: %-24s | 품절 | SPACE 사용 불가",
                         m->name);
            } else {
                mvprintw(info_y, menu_x + 2,
                         "선택 메뉴: %-24s | %d원 | SPACE 담기",
                         m->name, m->price);
            }
        } else {
            mvprintw(info_y, menu_x + 2, "표시할 메뉴가 없습니다.");
        }

        ui_attr_on(CP_FOOTER);
        mvprintw(rows - 1, 0,
                 "%-*s",
                 cols - 1,
                 "← 사이드바  → 메뉴판  ↑↓ 선택  SPACE 실행/담기  c 장바구니  k 직원호출  q 종료");
        ui_attr_off(CP_FOOTER);

        refresh();

        if (need_image_redraw) {
            for (int n = 0; n < page_size; ++n) {
                int idx = page_start + n;
                if (idx >= cat->count) {
                    continue;
                }

                int gr = n / grid_cols;
                int gc = n % grid_cols;
                int y = grid_top + gr * (cell_h + cell_gap_y);
                int x = menu_x + 2 + gc * (cell_w + cell_gap_x);
                int inner_x = x + 1;
                int inner_y = y + 1;
                int inner_w = cell_w - 2;
                int img_h = cell_h - 4;

                const MenuItem *m = &cat->items[idx];
                ui_clear_rect(inner_y, inner_x, inner_w, img_h);
                ui_draw_chafa_image(inner_y, inner_x, inner_w, img_h, m->id);
            }
        }

        last_menu_page = page;
        last_menu_rows = rows;
        last_menu_cols = cols;
        last_menu_count = cat->count;
        last_menu_sel = sel;
        last_table_screen = scr;
    } else if (scr == TABLE_SCR_CART) {
        int content_y = base;
        int content_h = rows - base - 4;

        int list_x = 2;
        int list_y = content_y + 1;
        int summary_w = 26;
        int gap = 3;

        int summary_x = cols - summary_w - 2;
        int list_w = summary_x - list_x - gap;
        int box_h = content_h;

        if (content_h < 10 || cols < 80) {
            mvprintw(base, 0, "터미널 크기가 너무 작습니다. 장바구니 화면을 보려면 창을 더 크게 해주세요.");
            mvprintw(rows - 2, 0, "m 메뉴판  q 종료");
            return;
        }

        /*
        * 장바구니가 비어 있는 경우
        */
        if (cart->count <= 0) {
            int msg_y = base + content_h / 2 - 2;
            const char *msg1 = "장바구니가 비어 있습니다";
            const char *msg2 = "메뉴판에서 원하는 메뉴를 담아주세요";
            const char *msg3 = "m : 메뉴판으로 돌아가기";

            ui_draw_simple_box(3, 2, cols - 4, rows - 6);

            attron(A_BOLD);
            mvprintw(msg_y,     (cols - (int)strlen(msg1)) / 2, "%s", msg1);
            attroff(A_BOLD);

            mvprintw(msg_y + 2, (cols - (int)strlen(msg2)) / 2, "%s", msg2);
            mvprintw(msg_y + 4, (cols - (int)strlen(msg3)) / 2, "%s", msg3);

            if (has_colors()) {
                attron(COLOR_PAIR(CP_FOOTER));
            }
            mvprintw(rows - 2, 0, "m 메뉴판  k 직원호출  q 종료");


            if (has_colors()) {
                attroff(COLOR_PAIR(CP_FOOTER));
            }


            return;
        }

        /*
        * 왼쪽 주문 메뉴 목록 박스
        */
        ui_draw_simple_box(list_y, list_x, list_w, box_h);
        attron(A_BOLD);
        mvprintw(list_y, list_x + 2, " 주문 메뉴 ");
        attroff(A_BOLD);

        /*
        * 오른쪽 주문 요약 박스
        */
        ui_draw_simple_box(list_y, summary_x, summary_w, box_h);
        attron(A_BOLD);
        mvprintw(list_y, summary_x + 2, " 주문 요약 ");
        attroff(A_BOLD);

        /*
        * 장바구니 목록 출력
        * 한 메뉴당 3줄 사용:
        * 1) 메뉴명
        * 2) 단가 x 수량 = 금액
        * 3) 빈 줄
        */
        int item_y = list_y + 2;
        int max_visible = (box_h - 4) / 3;

        int start = 0;
        if (sel >= max_visible) {
            start = sel - max_visible + 1;
        }

        for (int n = 0; n < max_visible && start + n < cart->count; ++n) {
            int idx = start + n;
            const CartItem *ci = &cart->items[idx];

            int y = item_y + n * 3;
            int line_total = ci->price * ci->qty;
            int selected = (idx == sel);

            /*
            * 이전 잔상 제거
            */
            mvprintw(y,     list_x + 2, "%-*s", list_w - 4, "");
            mvprintw(y + 1, list_x + 2, "%-*s", list_w - 4, "");

            if (selected) {
                if (has_colors()) {
                    attron(COLOR_PAIR(CP_MENU_SEL));
                }
                attron(A_BOLD);
                mvprintw(y, list_x + 2, "▶ %-*.*s",
                        list_w - 6, list_w - 6, ci->name);
                attroff(A_BOLD);
                if (has_colors()) {
                    attroff(COLOR_PAIR(CP_MENU_SEL));
                }
            } else {
                mvprintw(y, list_x + 2, "  %-*.*s",
                        list_w - 6, list_w - 6, ci->name);
            }

            mvprintw(y + 1, list_x + 4, "%d원 x %d", ci->price, ci->qty);

            /*
            * 오른쪽 끝에 항목별 합계 표시
            */
            char total_buf[32];
            snprintf(total_buf, sizeof(total_buf), "%d원", line_total);
            mvprintw(y + 1,
                    list_x + list_w - 2 - (int)strlen(total_buf),
                    "%s",
                    total_buf);
        }

        /*
        * 오른쪽 주문 요약
        */
        int total_qty = ui_cart_total_qty(cart);
        int total_price = cart_total(cart);

        int sy = list_y + 3;

        mvprintw(sy, summary_x + 2, "총 메뉴: %d종", cart->count);
        mvprintw(sy + 2, summary_x + 2, "총 수량: %d개", total_qty);

        mvhline(sy + 4, summary_x + 2, ACS_HLINE, summary_w - 4);

        attron(A_BOLD);
        mvprintw(sy + 6, summary_x + 2, "합계");
        if (has_colors()) {
            attron(COLOR_PAIR(CP_MENU_SEL));
        }
        mvprintw(sy + 8, summary_x + 2, "%d원", total_price);
        if (has_colors()) {
            attroff(COLOR_PAIR(CP_MENU_SEL));
        }
        attroff(A_BOLD);

        /*
        * 주문하기 / 메뉴판 안내 버튼 느낌
        */
        int btn_w = summary_w - 6;
        int order_y = sy + 11;
        int menu_y = sy + 15;

        if (order_y + 2 < list_y + box_h - 1) {
            if (has_colors()) {
                attron(COLOR_PAIR(CP_CART_BTN));
            }
            attron(A_BOLD);
            ui_draw_simple_box(order_y, summary_x + 3, btn_w, 3);
            mvprintw(order_y + 1, summary_x + 5, "ENTER 주문하기");
            attroff(A_BOLD);
            if (has_colors()) {
                attroff(COLOR_PAIR(CP_CART_BTN));
            }
        }

        if (menu_y + 2 < list_y + box_h - 1) {
            ui_draw_simple_box(menu_y, summary_x + 3, btn_w, 3);
            mvprintw(menu_y + 1, summary_x + 5, "m 메뉴판으로");
        }

        /*
        * 하단 안내문
        */
        if (has_colors()) {
            attron(COLOR_PAIR(CP_FOOTER));
        }

        mvprintw(rows - 2, 0,
                "↑↓ 선택  + 증가  - 감소  r 삭제  ENTER 주문하기  m 메뉴판  q 종료");

        if (has_colors()) {
            attroff(COLOR_PAIR(CP_FOOTER));
        }

        if (table_confirm_popup) {
            ui_draw_confirm_popup(rows, cols, cart);
            refresh();
        }
    } else if (scr == TABLE_SCR_STATUS) {
        last_table_screen = scr;
        ui_table_reset_images();
        mvprintw(base, 0, "%s", "네트워크 동기화된 주문 내역");
        refresh();
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
    ui_init_colors();
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

    MenuCatalog cat;
    menu_init_catalog(&cat);

    char menu_err[160];
    if (menu_load_file(&cat, MENU_PATH, menu_err, sizeof(menu_err)) != 0) {
        menu_init_catalog(&cat);
    }

    Order mirror[MAX_ORDERS];
    int mirror_count = 0;
    memset(mirror, 0, sizeof(mirror));

    Cart cart;
    cart_init(&cart);
    TableScreen scr = TABLE_SCR_MENU;
    TableFocus focus = TABLE_FOCUS_MENU;
    int sidebar_sel = SIDEBAR_POPULAR;
    int current_category = SIDEBAR_POPULAR;
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
            ui_table_reset_images();
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
                ui_table_reset_images();
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
            ui_draw_table_menu(&cat, scr, &cart, rows, cols, sel,
                               args->table_id, banner, focus, sidebar_sel,
                               current_category);
        } else {
            clear();
            mvprintw(0, 0, "Table %d 주문 상태", args->table_id);
            ui_draw_table_status_lines(mirror, mirror_count, args->table_id,
                                       rows, cols, 3);
            mvprintw(rows - 2, 0, "m 메뉴화면 k 직원호출 q 종료");
            refresh();
        }

        int ch = wgetch(stdscr);
        if (ch == ERR) {
            continue;
        }
        if (ch == 'q' || ch == 'Q') {
            break;
        }

        if (scr == TABLE_SCR_MENU) {
            int mc = cat.count;
            const int grid_cols = 3;
            const int page_size = 6;
            int old_page = sel / page_size;

            if (ch == KEY_LEFT) {
                if (focus == TABLE_FOCUS_MENU) {
                    if (sel % grid_cols == 0) {
                        focus = TABLE_FOCUS_SIDEBAR;
                        sidebar_sel = current_category;
                    } else {
                        int next = ui_find_selectable_from(&cat, sel - 1, -1);

                        if (next >= 0) {
                            sel = next;
                        }
                    }
                }
            } else if (ch == KEY_RIGHT) {
                if (focus == TABLE_FOCUS_SIDEBAR) {
                    focus = TABLE_FOCUS_MENU;
                    ui_table_reset_images();
                } else {
                    int next = ui_find_selectable_from(&cat, sel + 1, 1);

                    if (next >= 0) {
                        sel = next;
                    }
                }
            } else if (ch == KEY_UP) {
                if (focus == TABLE_FOCUS_SIDEBAR) {
                    if (sidebar_sel > 0) {
                        sidebar_sel--;
                    }
                } else {
                    int next = ui_find_selectable_from(&cat, sel - grid_cols, -grid_cols);

                    if (next >= 0) {
                        sel = next;
                    }
                }
            } else if (ch == KEY_DOWN) {
                if (focus == TABLE_FOCUS_SIDEBAR) {
                    if (sidebar_sel + 1 < SIDEBAR_COUNT) {
                        sidebar_sel++;
                    }
                } else {
                    int next = ui_find_selectable_from(&cat, sel + grid_cols, grid_cols);

                    if (next >= 0) {
                        sel = next;
                    }
                }
            } else if (ch == ' ') {
                if (focus == TABLE_FOCUS_SIDEBAR) {
                    if (sidebar_sel >= 0 && sidebar_sel <= SIDEBAR_DRINK) {
                        current_category = sidebar_sel;

                        {
                            int first = ui_find_selectable_from(&cat, 0, 1);
                            sel = (first >= 0) ? first : 0;
                        }

                        snprintf(banner, sizeof(banner),
                                "%s 선택됨 - 현재는 UI만 적용되어 전체 메뉴를 표시합니다",
                                TABLE_CATEGORY_NAMES[current_category]);

                        ui_table_reset_images();
                    } else if (sidebar_sel == SIDEBAR_CART) {
                        scr = TABLE_SCR_CART;
                        sel = 0;
                        ui_table_reset_images();
                    } else if (sidebar_sel == SIDEBAR_ORDER) {
                        scr = TABLE_SCR_STATUS;
                        sel = 0;
                        ui_table_reset_images();
                    } else if (sidebar_sel == SIDEBAR_CALL) {
                        char buf[80];
                        snprintf(buf, sizeof(buf), "CALL_STAFF|table=%d\n", args->table_id);
                        ui_send_line(sock, buf);
                        snprintf(banner, sizeof(banner), "직원을 호출했습니다");
                    }
                } else {
                    if (mc > 0 && sel >= 0 && sel < mc) {
                        const MenuItem *m = &cat.items[sel];

                        if (!m->sold_out) {
                            CartItem ci;
                            memset(&ci, 0, sizeof(ci));

                            ci.menu_id = m->id;
                            strncpy(ci.name, m->name, sizeof(ci.name) - 1);
                            ci.price = m->price;
                            ci.qty = 1;

                            cart_add(&cart, &ci, err, sizeof(err));
                            snprintf(banner, sizeof(banner), "%s 담기 완료", m->name);
                        } else {
                            snprintf(banner, sizeof(banner), "품절 메뉴입니다");
                        }
                    }
                }
            }

            int new_page = sel / page_size;

            if (old_page != new_page) {
                ui_table_reset_images();
            }

            if (mc > 0) {
                if (sel < 0) {
                    sel = 0;
                }

                if (sel >= mc) {
                    sel = mc - 1;
                }

                if (!ui_menu_is_selectable(&cat, sel)) {
                    int next = ui_find_selectable_from(&cat, sel + 1, 1);

                    if (next < 0) {
                        next = ui_find_selectable_from(&cat, sel - 1, -1);
                    }

                    if (next >= 0) {
                        sel = next;
                    }
                }
            } else {
                sel = 0;
            }

            {
                int new_page = sel / page_size;

                if (new_page != last_menu_page) {
                    ui_table_reset_images();
                }
            }
        }
        else if (scr == TABLE_SCR_CART) {
    int lines = cart.count;

    /*
     * 주문 확정 팝업이 떠 있을 때는 y/n/ESC만 처리한다.
     * 이 continue는 반드시 table_confirm_popup == 1인 경우 안에만 있어야 한다.
     */
    if (table_confirm_popup) {
        if (ch == 'y' || ch == 'Y') {
            if (ui_submit_cart_order(sock, args->table_id,
                                     &cart, banner, sizeof(banner))) {
                scr = TABLE_SCR_STATUS;
                sel = 0;
                ui_table_reset_images();
            }

            table_confirm_popup = 0;
        } else if (ch == 'n' || ch == 'N' || ch == 27) {
            table_confirm_popup = 0;
            snprintf(banner, sizeof(banner), "주문이 취소되었습니다");
        }

        continue;
    }

    /*
     * 여기부터는 팝업이 없을 때의 기존 장바구니 조작
     */
    if (ch == KEY_UP && lines > 0) {
        sel = (sel + lines - 1) % lines;
    }

    if (ch == KEY_DOWN && lines > 0) {
        sel = (sel + 1) % lines;
    }

    if ((ch == '+' || ch == '=') && lines > 0) {
        int mid = cart.items[sel].menu_id;
        cart_set_qty(&cart, mid, cart.items[sel].qty + 1, err, sizeof(err));
    }

    if (ch == '-' && lines > 0) {
        int mid = cart.items[sel].menu_id;
        cart_set_qty(&cart, mid, cart.items[sel].qty - 1, err, sizeof(err));

        if (sel >= cart.count) {
            sel = cart.count - 1;
        }

        if (sel < 0) {
            sel = 0;
        }
    }

    if (ch == 'r' && lines > 0) {
        cart_remove_line(&cart, cart.items[sel].menu_id);

        if (sel >= cart.count) {
            sel = cart.count - 1;
        }

        if (sel < 0) {
            sel = 0;
        }
    }

    if (ch == 'm') {
        scr = TABLE_SCR_MENU;
        sel = 0;
        table_confirm_popup = 0;
        ui_table_reset_images();
    }

    if (ch == 'k') {
        char buf[80];
        snprintf(buf, sizeof(buf), "CALL_STAFF|table=%d\n", args->table_id);
        ui_send_line(sock, buf);
        snprintf(banner, sizeof(banner), "직원을 호출했습니다");
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 'z') {
        if (cart.count > 0) {
            table_confirm_popup = 1;
        } else {
            snprintf(banner, sizeof(banner), "장바구니가 비어 있습니다");
        }
    } 
}else if (scr == TABLE_SCR_STATUS) {
            if (ch == 'm') {
                scr = TABLE_SCR_MENU;
                focus = TABLE_FOCUS_MENU;
                ui_table_reset_images();
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
