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

#include "layout.h"
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
    POS_TAB_PAYMENT,
    POS_TAB_MENU,
    POS_TAB_SALES,
    POS_TAB_LAYOUT
} PosTab;

typedef struct {
    int order_count;
    int unpaid_total;
    int done_total;
} PosTableSummary;

#define POS_COLOR_OUTSIDE 1
#define POS_COLOR_INSIDE 2
#define POS_COLOR_STAFF 3
#define POS_COLOR_ACTIVE 4

static int ui_layout_active_count(const StoreLayout *layout);

static void ui_pos_init_colors(void) {
    if (!has_colors()) {
        return;
    }
    start_color();
    use_default_colors();
    init_pair(POS_COLOR_OUTSIDE, COLOR_CYAN, COLOR_BLACK);
    init_pair(POS_COLOR_INSIDE, COLOR_YELLOW, COLOR_BLACK);
    init_pair(POS_COLOR_STAFF, COLOR_RED, COLOR_BLACK);
    init_pair(POS_COLOR_ACTIVE, COLOR_GREEN, COLOR_BLACK);
}

static void ui_pos_table_summary(ServerContext *ctx, int table_id,
                                 PosTableSummary *sum) {
    memset(sum, 0, sizeof(*sum));
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->order_count; ++i) {
        Order *o = &ctx->orders[i];
        if (o->table_id != table_id) {
            continue;
        }
        sum->order_count++;
        if (o->status != STATUS_PAID) {
            sum->unpaid_total += o->total_price;
        }
        if (o->status == STATUS_DONE) {
            sum->done_total += o->total_price;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
}

static int ui_pos_staff_active(ServerContext *ctx, int table_id) {
    time_t at = 0;
    pthread_mutex_lock(&ctx->lock);
    if (table_id > 0 && table_id <= MAX_TABLES) {
        at = ctx->staff_call_at[table_id];
    }
    pthread_mutex_unlock(&ctx->lock);
    if (at == 0) {
        return 0;
    }
    return (time(NULL) - at) < 180;
}

static void ui_pos_draw_table_box(int top, int left, int h, int w,
                                  const TablePlacement *tp,
                                  const PosTableSummary *sum, int selected,
                                  int staff_call) {
    if (h < 3 || w < 6) {
        return;
    }
    int pair = tp->zone == TABLE_ZONE_INSIDE ? POS_COLOR_INSIDE
                                             : POS_COLOR_OUTSIDE;
    if (staff_call) {
        pair = POS_COLOR_STAFF;
    } else if (sum->order_count > 0) {
        pair = POS_COLOR_ACTIVE;
    }

    attr_t attrs = COLOR_PAIR(pair);
    if (selected) {
        attrs |= A_REVERSE;
    }
    if (staff_call) {
        attrs |= A_BLINK;
    }
    attrset(attrs);

    for (int y = 0; y < h; ++y) {
        mvaddch(top + y, left, ACS_VLINE);
        mvaddch(top + y, left + w - 1, ACS_VLINE);
        for (int x = 1; x < w - 1; ++x) {
            mvaddch(top + y, left + x, ' ');
        }
    }
    mvaddch(top, left, ACS_ULCORNER);
    mvaddch(top, left + w - 1, ACS_URCORNER);
    mvaddch(top + h - 1, left, ACS_LLCORNER);
    mvaddch(top + h - 1, left + w - 1, ACS_LRCORNER);
    for (int x = 1; x < w - 1; ++x) {
        mvaddch(top, left + x, ACS_HLINE);
        mvaddch(top + h - 1, left + x, ACS_HLINE);
    }

    char title[32];
    snprintf(title, sizeof(title), "%.*s", w - 2, tp->label);
    mvprintw(top, left + 1, "%-*s", w - 2, title);

    if (h >= 4) {
        if (sum->order_count == 0) {
            mvprintw(top + 1, left + 1, "주문없음");
        } else {
            mvprintw(top + 1, left + 1, "%d건", sum->order_count);
            if (h >= 5) {
                mvprintw(top + 2, left + 1, "₩%d", sum->unpaid_total);
            }
        }
    }
    if (staff_call && h >= 6) {
        mvprintw(top + h - 2, left + 1, "!호출!");
    }
    attrset(A_NORMAL);
}

static void ui_draw_pos_home(ServerContext *ctx, const StoreLayout *layout,
                             int cur_row, int cur_col, int rows, int cols) {
    clear();
    mvprintw(0, 0, "POS | 테이블 배치 (%dx%d 그리드, %d석)",
             layout->grid_cols, layout->grid_rows,
             ui_layout_active_count(layout));

    int top_base = 2;
    int usable_h = rows - top_base - 2;
    int usable_w = cols;
    if (usable_h < 4 || usable_w < 10) {
        mvprintw(2, 0, "터미널 크기를 키워 주세요.");
        return;
    }

    int cell_w = usable_w / layout->grid_cols;
    int cell_h = usable_h / layout->grid_rows;
    if (cell_w < 8) {
        cell_w = 8;
    }
    if (cell_h < 4) {
        cell_h = 4;
    }

    for (int i = 0; i < layout->count; ++i) {
        const TablePlacement *tp = &layout->tables[i];
        if (!tp->active) {
            continue;
        }
        int left = tp->col * cell_w;
        int top = top_base + tp->row * cell_h;
        if (left + cell_w > cols || top + cell_h > rows - 1) {
            continue;
        }
        PosTableSummary sum;
        ui_pos_table_summary(ctx, tp->table_id, &sum);
        int staff = ui_pos_staff_active(ctx, tp->table_id);
        int sel = (tp->row == cur_row && tp->col == cur_col);
        ui_pos_draw_table_box(top, left, cell_h, cell_w, tp, &sum, sel, staff);
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->last_staff_message[0] != '\0') {
        mvprintw(rows - 3, 0, "[직원호출] %s", ctx->last_staff_message);
    }
    pthread_mutex_unlock(&ctx->lock);

    mvprintw(rows - 1, 0,
             "F1배치 F2결제 F3메뉴 F4매출 F5배치관리 | 방향키 이동 Enter결제 q종료");
}

static void ui_draw_pos_payment(ServerContext *ctx, int rows, int cols,
                                int pay_table, int sel) {
    (void)cols;
    clear();
    mvprintw(0, 0, "결제하기 | 테이블 번호 입력 후 주문 확인 (t:번호변경)");
    mvprintw(1, 0, "대상 테이블: %s",
             pay_table > 0 ? "" : "(미지정)");
    if (pay_table > 0) {
        TablePlacement *tp = NULL;
        StoreLayout tmp;
        layout_init(&tmp);
        char err[64];
        layout_load(LAYOUT_PATH, &tmp, err, sizeof(err));
        tp = layout_find_by_id(&tmp, pay_table);
        mvprintw(1, 14, "%d %s", pay_table, tp ? tp->label : "");
    }

    PosTableSummary sum;
    if (pay_table > 0) {
        ui_pos_table_summary(ctx, pay_table, &sum);
        mvprintw(2, 0, "미결제 합계: ₩%d | 결제가능(DONE): ₩%d", sum.unpaid_total,
                 sum.done_total);
    }

    pthread_mutex_lock(&ctx->lock);
    int shown = 0;
    for (int i = 0; i < ctx->order_count && shown < rows - 8; ++i) {
        Order *o = &ctx->orders[i];
        if (pay_table > 0 && o->table_id != pay_table) {
            continue;
        }
        attrset(shown == sel ? A_REVERSE : A_NORMAL);
        mvprintw(4 + shown, 0, "#%04d T%02d %-8s ₩%-7d", o->order_id,
                 o->table_id, status_to_string(o->status), o->total_price);
        if (o->item_count > 0) {
            printw(" | %s x%d", o->items[0].name, o->items[0].qty);
        }
        attrset(A_NORMAL);
        shown++;
    }
    pthread_mutex_unlock(&ctx->lock);

    mvprintw(rows - 3, 0, "Enter: DONE 주문 결제 | y: 해당 테이블 DONE 전체 결제");
    mvprintw(rows - 2, 0, "t 테이블번호 변경");
}

static int ui_pos_collect_table_done_ids(ServerContext *ctx, int table_id,
                                         int *ids, int max_ids) {
    int n = 0;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->order_count && n < max_ids; ++i) {
        Order *o = &ctx->orders[i];
        if (o->status == STATUS_DONE &&
            (table_id <= 0 || o->table_id == table_id)) {
            ids[n++] = o->order_id;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return n;
}

static int ui_pos_collect_table_order_indices(ServerContext *ctx, int table_id,
                                              int *idxs, int max_idxs) {
    int n = 0;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->order_count && n < max_idxs; ++i) {
        if (table_id <= 0 || ctx->orders[i].table_id == table_id) {
            idxs[n++] = i;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return n;
}

static void ui_draw_pos_menu_admin(ServerContext *ctx, int rows, int cols,
                                   int sel) {
    (void)cols;
    clear();
    mvprintw(0, 0, "메뉴 관리 | a추가 e수정 d삭제 s품절 | F1배치 F2결제 F4매출");
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

#define POS_WGETCH_TIMEOUT_MS 200

/* POS 메인 루프는 timeout(200)으로 폴링한다. wgetnstr도 동일 timeout을
 * 상속해 입력 없이 즉시 반환되므로, 프롬프트 동안만 블로킹으로 전환한다. */
static void ui_pos_prompt_begin(void) {
    timeout(-1);
    nodelay(stdscr, FALSE);
    flushinp();
}

static void ui_pos_prompt_end(void) {
    timeout(POS_WGETCH_TIMEOUT_MS);
}

static void ui_prompt_string(WINDOW *w, int row, const char *label, char *buf,
                             size_t bufsz, int require_nonempty) {
    ui_pos_prompt_begin();
    echo();
    curs_set(1);
    for (;;) {
        move(row, 0);
        clrtoeol();
        mvprintw(row, 0, "%s", label);
        refresh();
        if (wgetnstr(w, buf, (int)bufsz - 1) == ERR && bufsz > 0) {
            buf[0] = '\0';
        }
        if (!require_nonempty || buf[0] != '\0') {
            break;
        }
    }
    noecho();
    curs_set(0);
    ui_pos_prompt_end();
}

static void ui_prompt_int(WINDOW *w, int row, const char *label, int *out) {
    char buf[64];
    ui_pos_prompt_begin();
    echo();
    curs_set(1);
    for (;;) {
        move(row, 0);
        clrtoeol();
        mvprintw(row, 0, "%s", label);
        refresh();
        buf[0] = '\0';
        if (wgetnstr(w, buf, (int)sizeof(buf) - 1) == ERR) {
            buf[0] = '\0';
        }
        if (buf[0] != '\0') {
            break;
        }
    }
    noecho();
    curs_set(0);
    ui_pos_prompt_end();
    *out = atoi(buf);
}

static void ui_draw_pos_layout_editor(const StoreLayout *layout, int cur_row,
                                    int cur_col, int edit_sel, int rows,
                                    int cols) {
    clear();
    mvprintw(0, 0, "테이블 배치 관리 | 그리드 %dx%d | 테이블 %d개",
             layout->grid_cols, layout->grid_rows,
             ui_layout_active_count(layout));
    mvprintw(1, 0,
             "g그리드 a추가 d삭제 m이동 z구역 l라벨 s저장 | 방향키 커서 Enter선택");

    int top_base = 3;
    int usable_h = rows - top_base - 2;
    int usable_w = cols;
    int cell_w = usable_w / layout->grid_cols;
    int cell_h = usable_h / layout->grid_rows;
    if (cell_w < 8) {
        cell_w = 8;
    }
    if (cell_h < 4) {
        cell_h = 4;
    }

    for (int r = 0; r < layout->grid_rows; ++r) {
        for (int c = 0; c < layout->grid_cols; ++c) {
            int left = c * cell_w;
            int top = top_base + r * cell_h;
            TablePlacement *tp = layout_find_at((StoreLayout *)layout, r, c);
            if (tp) {
                PosTableSummary dummy;
                memset(&dummy, 0, sizeof(dummy));
                int sel = (r == cur_row && c == cur_col);
                ui_pos_draw_table_box(top, left, cell_h, cell_w, tp, &dummy,
                                      sel, 0);
            } else if (r == cur_row && c == cur_col) {
                attrset(A_REVERSE);
                for (int y = 0; y < cell_h && top + y < rows - 1; ++y) {
                    mvprintw(top + y, left, "%-*s", cell_w, "");
                }
                attrset(A_NORMAL);
            }
        }
    }

    if (edit_sel >= 0 && edit_sel < layout->count) {
        const TablePlacement *tp = &layout->tables[edit_sel];
        mvprintw(rows - 2, 0,
                 "선택: id=%d %s zone=%s (%d,%d)", tp->table_id, tp->label,
                 tp->zone == TABLE_ZONE_INSIDE ? "안" : "밖", tp->row, tp->col);
    } else {
        mvprintw(rows - 2, 0, "커서 (%d,%d) — 빈 칸", cur_row, cur_col);
    }
}

static void ui_pos_sync_max_tables(ServerContext *ctx, const StoreLayout *layout) {
    int max_id = layout_max_table_id(layout);
    if (max_id <= 0) {
        return;
    }
    pthread_mutex_lock(&ctx->lock);
    if (max_id > ctx->cfg.max_tables) {
        ctx->cfg.max_tables = max_id;
    }
    pthread_mutex_unlock(&ctx->lock);
}

static int ui_pos_layout_find_index(const StoreLayout *layout, int row,
                                    int col) {
    for (int i = 0; i < layout->count; ++i) {
        if (layout->tables[i].active && layout->tables[i].row == row &&
            layout->tables[i].col == col) {
            return i;
        }
    }
    return -1;
}

static int ui_layout_active_count(const StoreLayout *layout) {
    int n = 0;
    for (int i = 0; i < layout->count; ++i) {
        if (layout->tables[i].active) {
            n++;
        }
    }
    return n;
}

/* 삭제 시 active=0만 두면 count가 줄지 않아 계속 증가하는 문제 방지 */
static void ui_pos_layout_compact(StoreLayout *layout) {
    int w = 0;
    for (int i = 0; i < layout->count; ++i) {
        if (!layout->tables[i].active) {
            continue;
        }
        if (w != i) {
            layout->tables[w] = layout->tables[i];
        }
        w++;
    }
    layout->count = w;
}

static void ui_pos_layout_remove_at(StoreLayout *layout, int idx) {
    if (idx < 0 || idx >= layout->count) {
        return;
    }
    for (int i = idx; i < layout->count - 1; ++i) {
        layout->tables[i] = layout->tables[i + 1];
    }
    layout->count--;
}

static int ui_pos_layout_alloc_slot(StoreLayout *layout) {
    for (int i = 0; i < layout->count; ++i) {
        if (!layout->tables[i].active) {
            return i;
        }
    }
    if (layout->count >= MAX_LAYOUT_TABLES) {
        return -1;
    }
    return layout->count++;
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
    cbreak();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    timeout(POS_WGETCH_TIMEOUT_MS);
    ui_pos_init_colors();

    StoreLayout layout;
    char layout_err[160];
    if (layout_load(LAYOUT_PATH, &layout, layout_err, sizeof(layout_err)) != 0) {
        layout_init_default(&layout);
    }
    ui_pos_layout_compact(&layout);
    ui_pos_sync_max_tables(ctx, &layout);
    {
        char err[160];
        server_save_config(ctx, err, sizeof(err));
    }

    PosTab tab = POS_TAB_HOME;
    int sel_menu = 0;
    int sel_payment = 0;
    int pay_table = 0;
    int grid_row = 0;
    int grid_col = 0;
    int layout_edit_idx = -1;
    int layout_move_mode = 0;
    uint32_t last_rev = 0;

    while (!ctx->shutting_down) {
        int rows = 0, cols = 0;
        getmaxyx(stdscr, rows, cols);
        if ((uint32_t)ctx->orders_revision != last_rev) {
            last_rev = (uint32_t)ctx->orders_revision;
            sel_payment = 0;
        }

        switch (tab) {
        case POS_TAB_HOME:
            ui_draw_pos_home(ctx, &layout, grid_row, grid_col, rows, cols);
            break;
        case POS_TAB_PAYMENT:
            ui_draw_pos_payment(ctx, rows, cols, pay_table, sel_payment);
            break;
        case POS_TAB_MENU:
            ui_draw_pos_menu_admin(ctx, rows, cols, sel_menu);
            break;
        case POS_TAB_SALES:
            ui_draw_pos_sales(rows, cols);
            break;
        case POS_TAB_LAYOUT:
            ui_draw_pos_layout_editor(&layout, grid_row, grid_col,
                                      layout_edit_idx, rows, cols);
            break;
        }

        int ch = wgetch(stdscr);
        if (ch == ERR) {
            continue;
        }
        if (ch == 'q' || ch == 'Q') {
            ctx->shutting_down = 1;
            break;
        }
        if (ch == KEY_F(1)) {
            tab = POS_TAB_HOME;
        }
        if (ch == KEY_F(2)) {
            tab = POS_TAB_PAYMENT;
        }
        if (ch == KEY_F(3)) {
            tab = POS_TAB_MENU;
        }
        if (ch == KEY_F(4)) {
            tab = POS_TAB_SALES;
        }
        if (ch == KEY_F(5)) {
            tab = POS_TAB_LAYOUT;
        }

        if (tab == POS_TAB_HOME) {
            if (ch == KEY_UP && grid_row > 0) {
                grid_row--;
            }
            if (ch == KEY_DOWN && grid_row + 1 < layout.grid_rows) {
                grid_row++;
            }
            if (ch == KEY_LEFT && grid_col > 0) {
                grid_col--;
            }
            if (ch == KEY_RIGHT && grid_col + 1 < layout.grid_cols) {
                grid_col++;
            }
            TablePlacement *tp =
                layout_find_at(&layout, grid_row, grid_col);
            if ((ch == '\n' || ch == KEY_ENTER) && tp) {
                pay_table = tp->table_id;
                tab = POS_TAB_PAYMENT;
                sel_payment = 0;
            }
            if (ch == 'c' && tp) {
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                if (tp->table_id > 0 && tp->table_id <= MAX_TABLES) {
                    ctx->staff_call_at[tp->table_id] = 0;
                }
                pthread_mutex_unlock(&ctx->lock);
                (void)err;
            }
        }

        if (tab == POS_TAB_PAYMENT) {
            int idxs[MAX_ORDERS];
            int nshow =
                ui_pos_collect_table_order_indices(ctx, pay_table, idxs,
                                                   MAX_ORDERS);
            if (ch == 't') {
                int nt = 0;
                ui_prompt_int(stdscr, rows - 5, "결제 테이블 번호: ", &nt);
                if (nt > 0 && nt <= MAX_TABLES) {
                    pay_table = nt;
                    sel_payment = 0;
                }
            }
            if (ch == KEY_UP) {
                sel_payment = (sel_payment + nshow - 1) % (nshow ? nshow : 1);
            }
            if (ch == KEY_DOWN) {
                sel_payment = (sel_payment + 1) % (nshow ? nshow : 1);
            }
            if (ch == '\n' || ch == KEY_ENTER) {
                if (sel_payment < nshow) {
                    int oid = 0;
                    OrderStatus st = STATUS_PAID;
                    pthread_mutex_lock(&ctx->lock);
                    oid = ctx->orders[idxs[sel_payment]].order_id;
                    st = ctx->orders[idxs[sel_payment]].status;
                    pthread_mutex_unlock(&ctx->lock);
                    if (st == STATUS_DONE) {
                        char err[160];
                        server_pay_order(ctx, oid, err, sizeof(err));
                    }
                }
            }
            if (ch == 'y' && pay_table > 0) {
                int ids[MAX_ORDERS];
                int nids = ui_pos_collect_table_done_ids(ctx, pay_table, ids,
                                                         MAX_ORDERS);
                for (int i = 0; i < nids; ++i) {
                    char err[160];
                    server_pay_order(ctx, ids[i], err, sizeof(err));
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
                                 sizeof(namebuf), 1);
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
                ui_prompt_string(stdscr, rows - 6, "새 이름: ", nb, sizeof(nb),
                                 0);
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

        if (tab == POS_TAB_LAYOUT) {
            if (ch == KEY_UP && grid_row > 0) {
                grid_row--;
            }
            if (ch == KEY_DOWN && grid_row + 1 < layout.grid_rows) {
                grid_row++;
            }
            if (ch == KEY_LEFT && grid_col > 0) {
                grid_col--;
            }
            if (ch == KEY_RIGHT && grid_col + 1 < layout.grid_cols) {
                grid_col++;
            }
            layout_edit_idx = ui_pos_layout_find_index(&layout, grid_row,
                                                     grid_col);

            if (ch == 'g') {
                int nr = 0, nc = 0;
                ui_prompt_int(stdscr, rows - 6, "그리드 행(rows): ", &nr);
                ui_prompt_int(stdscr, rows - 5, "그리드 열(cols): ", &nc);
                if (nr > 0 && nr <= MAX_GRID_ROWS && nc > 0 &&
                    nc <= MAX_GRID_COLS) {
                    layout.grid_rows = nr;
                    layout.grid_cols = nc;
                    if (grid_row >= nr) {
                        grid_row = nr - 1;
                    }
                    if (grid_col >= nc) {
                        grid_col = nc - 1;
                    }
                }
            }

            if (ch == 'a') {
                if (layout_find_at(&layout, grid_row, grid_col)) {
                    continue;
                }
                int slot = ui_pos_layout_alloc_slot(&layout);
                if (slot < 0) {
                    continue;
                }
                int new_id = layout_max_table_id(&layout) + 1;
                TablePlacement *tp = &layout.tables[slot];
                memset(tp, 0, sizeof(*tp));
                tp->active = 1;
                tp->table_id = new_id;
                tp->row = grid_row;
                tp->col = grid_col;
                tp->zone = TABLE_ZONE_OUTSIDE;
                snprintf(tp->label, sizeof(tp->label), "T%d", new_id);
                layout_edit_idx = slot;
                ui_pos_sync_max_tables(ctx, &layout);
            }

            if (ch == 'd' && layout_edit_idx >= 0) {
                ui_pos_layout_remove_at(&layout, layout_edit_idx);
                layout_edit_idx = -1;
            }

            if (ch == 'z' && layout_edit_idx >= 0) {
                TablePlacement *tp = &layout.tables[layout_edit_idx];
                tp->zone = tp->zone == TABLE_ZONE_INSIDE ? TABLE_ZONE_OUTSIDE
                                                       : TABLE_ZONE_INSIDE;
            }

            if (ch == 'l' && layout_edit_idx >= 0) {
                char lb[MAX_TABLE_LABEL];
                lb[0] = '\0';
                ui_prompt_string(stdscr, rows - 5, "테이블 라벨: ", lb,
                                 sizeof(lb), 0);
                if (lb[0]) {
                    snprintf(layout.tables[layout_edit_idx].label,
                             MAX_TABLE_LABEL, "%s", lb);
                }
            }

            if (ch == 'm' && layout_edit_idx >= 0) {
                layout_move_mode = !layout_move_mode;
            }

            if (layout_move_mode && layout_edit_idx >= 0 &&
                (ch == '\n' || ch == KEY_ENTER)) {
                if (!layout_find_at(&layout, grid_row, grid_col)) {
                    layout.tables[layout_edit_idx].row = grid_row;
                    layout.tables[layout_edit_idx].col = grid_col;
                }
                layout_move_mode = 0;
            }

            if (ch == 's') {
                char err[160];
                ui_pos_sync_max_tables(ctx, &layout);
                if (layout_save(LAYOUT_PATH, &layout, err, sizeof(err)) == 0) {
                    server_save_config(ctx, err, sizeof(err));
                }
            }
        }
    }

    {
        char err[160];
        layout_save(LAYOUT_PATH, &layout, err, sizeof(err));
        ui_pos_sync_max_tables(ctx, &layout);
        server_save_config(ctx, err, sizeof(err));
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
