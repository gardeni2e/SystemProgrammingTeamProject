#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static void __attribute__((unused)) ui_optional_chafa(const char *path) {
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
        mvprintw(y + r, x, "%*s", w, "");
    }
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

static void __attribute__((unused)) ui_fill_rect_color(int y, int x, int w, int h, int color_pair) {
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

static void ui_draw_centered_text(int y, int x, int w, const char *text) {
    int len = (int)strlen(text);
    int tx = x + (w - len) / 2;

    if (tx < x + 1) {
        tx = x + 1;
    }

    mvprintw(y, tx, "%s", text);
}

static void ui_draw_sidebar_notice(int y, int x, int w) {
    int h = 5;

    if (w < 16) {
        return;
    }

    if (has_colors()) {
        attron(COLOR_PAIR(CP_CALL_BTN));
    }

    attron(A_BOLD);

    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + h - 1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);

    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

    ui_draw_centered_text(y + 1, x + 1, w - 2, "직원 호출 완료");
    ui_draw_centered_text(y + 2, x + 2, w - 2, "잠시만");
    ui_draw_centered_text(y + 3, x + 1, w - 2, "기다려주세요!");

    attroff(A_BOLD);

    if (has_colors()) {
        attroff(COLOR_PAIR(CP_CALL_BTN));
    }
}

static void ui_clear_sidebar_notice(int y, int x, int w) {
    for (int r = 0; r < 5; ++r) {
        mvprintw(y + r, x, "%-*s", w, "");
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

    
    mvprintw(y + 1, x + 1, "%-*s", inner_w, "");

    
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

static void __attribute__((unused)) ui_draw_badge(int y, int x, int count) {
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

typedef struct {
    int top;
    int left;
    int h;
    int w;
} UiRect;

static UiRect ui_rect(int top, int left, int h, int w) {
    UiRect r;
    r.top = top;
    r.left = left;
    r.h = h;
    r.w = w;
    return r;
}

static int ui_rect_contains(const UiRect *r, int y, int x) {
    return y >= r->top && y < r->top + r->h && x >= r->left &&
           x < r->left + r->w;
}


static void ui_pos_draw_box(UiRect r, const char *title) {
    if (r.h < 3 || r.w < 6) {
        return;
    }
    mvaddch(r.top, r.left, ACS_ULCORNER);
    mvaddch(r.top, r.left + r.w - 1, ACS_URCORNER);
    mvaddch(r.top + r.h - 1, r.left, ACS_LLCORNER);
    mvaddch(r.top + r.h - 1, r.left + r.w - 1, ACS_LRCORNER);
    for (int x = 1; x < r.w - 1; ++x) {
        mvaddch(r.top, r.left + x, ACS_HLINE);
        mvaddch(r.top + r.h - 1, r.left + x, ACS_HLINE);
    }
    for (int y = 1; y < r.h - 1; ++y) {
        mvaddch(r.top + y, r.left, ACS_VLINE);
        mvaddch(r.top + y, r.left + r.w - 1, ACS_VLINE);
    }
    for (int y = 1; y < r.h - 1; ++y) {
        move(r.top + y, r.left + 1);
        for (int x = 1; x < r.w - 1; ++x) {
            addch(' ');
        }
    }

    if (title && title[0] && r.w > 4) {
        int max = r.w - 4;
        char tbuf[96];
        snprintf(tbuf, sizeof(tbuf), "%.*s", max, title);
        mvprintw(r.top, r.left + 2, "%s", tbuf);
    }
}

static void ui_pos_draw_button_in(UiRect panel, int row, int color_pair,
                                  const char *label) {
    int y = panel.top + 1 + row;
    int x = panel.left + 2;
    int w = panel.w - 4;
    int h = 3;

    if (panel.w < 10 || panel.h < row + h + 2) {
        return;
    }
    if (w < 8) {
        return;
    }

    if (has_colors()) {
        attron(COLOR_PAIR(color_pair));
    }
    attron(A_BOLD);

    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + h - 1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

    mvprintw(y + 1, x + 1, "%-*s", w - 2, "");
    mvprintw(y + 1, x + 2, "%-*.*s", w - 4, w - 4, label);

    attroff(A_BOLD);
    if (has_colors()) {
        attroff(COLOR_PAIR(color_pair));
    }
}

static void ui_pos_print_in(UiRect r, int row, int col, attr_t attrs,
                            const char *fmt, ...) {
    if (row < 0 || col < 0) {
        return;
    }
    int y = r.top + 1 + row;
    int x = r.left + 1 + col;
    if (!ui_rect_contains(&r, y, x)) {
        return;
    }
    int maxw = r.left + r.w - 2 - x;
    if (maxw <= 0) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    attrset(attrs);
    mvprintw(y, x, "%.*s", maxw, buf);
    attrset(A_NORMAL);
}

static const char *ui_pos_tab_name(PosTab t) {
    switch (t) {
    case POS_TAB_HOME:
        return "테이블";
    case POS_TAB_PAYMENT:
        return "결제";
    case POS_TAB_MENU:
        return "메뉴";
    case POS_TAB_SALES:
        return "매출";
    case POS_TAB_LAYOUT:
        return "배치";
    }
    return "POS";
}

static int ui_display_width(const char *s) {
    if (!s || !s[0]) {
        return 0;
    }

    mbstate_t st;
    memset(&st, 0, sizeof(st));

    int w = 0;
    const char *p = s;
    while (*p) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (r == (size_t)-1 || r == (size_t)-2) {
            p++;
            w++;
            memset(&st, 0, sizeof(st));
            continue;
        }
        if (r == 0) {
            break;
        }
        int cw = wcwidth(wc);
        if (cw < 0) {
            cw = 1;
        }
        w += cw;
        p += r;
    }
    return w;
}

static void ui_pos_layout_frame(int rows, int cols, PosTab tab,
                                UiRect *out_header, UiRect *out_main,
                                UiRect *out_side, UiRect *out_footer) {
    int header_h = 2;
    int footer_h = 2;
    int inner_top = header_h;
    int inner_h = rows - header_h - footer_h;
    if (inner_h < 6) {
        inner_h = 6;
    }
    int side_w = cols / 3;
    if (side_w < 26) {
        side_w = 26;
    }
    if (side_w > 42) {
        side_w = 42;
    }
    int main_w = cols - side_w - 1;
    if (main_w < 20) {
        main_w = cols;
        side_w = 0;
    }

    *out_header = ui_rect(0, 0, header_h, cols);
    *out_footer = ui_rect(rows - footer_h, 0, footer_h, cols);

    *out_main = ui_rect(inner_top, 0, inner_h, main_w);
    if (side_w > 0) {
        *out_side = ui_rect(inner_top, main_w + 1, inner_h, side_w);
    } else {
        *out_side = ui_rect(inner_top, cols, inner_h, 0);
    }

    move(0, 0);
    clrtoeol();
    mvprintw(0, 1, "POS");
    mvprintw(0, 6, "│ %s", ui_pos_tab_name(tab));
    
    const char *keys =
        "F1 테이블  F2 결제  F3 메뉴  F4 매출  F5 배치편집";
    int klen = ui_display_width(keys);
    int x = cols - klen;
    if (x < 0) {
        x = 0;
    }
    mvprintw(0, x, "%s", keys);

    move(1, 0);
    clrtoeol();
    mvhline(1, 0, ' ', cols);
    (void)tab;
}

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
        if (staff_call) {
            mvprintw(top + 1, left + 1, "직원호출");
            if (h >= 5 && sum->order_count > 0) {
                mvprintw(top + 2, left + 1, "%d건", sum->order_count);
            }
        } else if (sum->order_count == 0) {
            mvprintw(top + 1, left + 1, "주문없음");
        } else {
            mvprintw(top + 1, left + 1, "%d건", sum->order_count);
            if (h >= 5) {
                mvprintw(top + 2, left + 1, "₩%d", sum->unpaid_total);
            }
        }
    }
    if (staff_call && h >= 6 && sum->order_count > 0) {
        mvprintw(top + h - 2, left + 1, "!호출!");
    }
    attrset(A_NORMAL);
}

static void ui_draw_pos_home(ServerContext *ctx, const StoreLayout *layout,
                             int cur_row, int cur_col, int rows, int cols) {
    clear();
    UiRect header, main, side, footer;
    ui_pos_layout_frame(rows, cols, POS_TAB_HOME, &header, &main, &side, &footer);

    ui_pos_draw_box(main, "테이블 현황");
    if (side.w > 0) {
        ui_pos_draw_box(side, "선택 테이블 / 요약");
    }

    int usable_h = main.h - 2;
    int usable_w = main.w - 2;
    if (usable_h < 4 || usable_w < 10) {
        ui_pos_print_in(main, 0, 0, A_BOLD, "터미널 크기를 키워 주세요.");
        return;
    }

    
    const int min_cell_w = 12;
    const int min_cell_h = 6;

    int view_cols = usable_w / min_cell_w;
    int view_rows = usable_h / min_cell_h;
    if (view_cols < 1) {
        view_cols = 1;
    }
    if (view_rows < 1) {
        view_rows = 1;
    }
    if (view_cols > layout->grid_cols) {
        view_cols = layout->grid_cols;
    }
    if (view_rows > layout->grid_rows) {
        view_rows = layout->grid_rows;
    }

    int cell_w = usable_w / view_cols;
    int cell_h = usable_h / view_rows;

    int view_c0 = cur_col - view_cols / 2;
    int view_r0 = cur_row - view_rows / 2;
    if (view_c0 < 0) {
        view_c0 = 0;
    }
    if (view_r0 < 0) {
        view_r0 = 0;
    }
    if (view_c0 + view_cols > layout->grid_cols) {
        view_c0 = layout->grid_cols - view_cols;
        if (view_c0 < 0) {
            view_c0 = 0;
        }
    }
    if (view_r0 + view_rows > layout->grid_rows) {
        view_r0 = layout->grid_rows - view_rows;
        if (view_r0 < 0) {
            view_r0 = 0;
        }
    }

    for (int i = 0; i < layout->count; ++i) {
        const TablePlacement *tp = &layout->tables[i];
        if (!tp->active) {
            continue;
        }
        if (tp->col < view_c0 || tp->col >= view_c0 + view_cols ||
            tp->row < view_r0 || tp->row >= view_r0 + view_rows) {
            continue;
        }
        int rel_c = tp->col - view_c0;
        int rel_r = tp->row - view_r0;
        int left = main.left + 1 + rel_c * cell_w;
        int top = main.top + 1 + rel_r * cell_h;
        if (left + cell_w > main.left + main.w - 1 ||
            top + cell_h > main.top + main.h - 1) {
            continue;
        }
        PosTableSummary sum;
        ui_pos_table_summary(ctx, tp->table_id, &sum);
        int staff = ui_pos_staff_active(ctx, tp->table_id);
        int sel = (tp->row == cur_row && tp->col == cur_col);
        ui_pos_draw_table_box(top, left, cell_h, cell_w, tp, &sum, sel, staff);
    }

    if (side.w > 0) {
        TablePlacement *tp =
            layout_find_at((StoreLayout *)layout, cur_row, cur_col);
        if (!tp) {
            ui_pos_print_in(side, 0, 0, A_DIM, "커서: (%d,%d)", cur_row, cur_col);
            ui_pos_print_in(side, 1, 0, A_BOLD, "빈 칸");
        } else {
            PosTableSummary sum;
            ui_pos_table_summary(ctx, tp->table_id, &sum);
            int staff = ui_pos_staff_active(ctx, tp->table_id);
            ui_pos_print_in(side, 0, 0, A_BOLD, "%s", tp->label);
            ui_pos_print_in(side, 1, 0, A_DIM, "Table ID: %d  Zone: %s",
                            tp->table_id,
                            tp->zone == TABLE_ZONE_INSIDE ? "안" : "밖");
            ui_pos_print_in(side, 3, 0, A_NORMAL, "주문: %d건", sum.order_count);
            ui_pos_print_in(side, 4, 0, A_NORMAL, "미결제: ₩%d", sum.unpaid_total);
            ui_pos_print_in(side, 5, 0, A_NORMAL, "DONE: ₩%d", sum.done_total);
            if (staff) {
                ui_pos_print_in(side, 7, 0,
                                A_BOLD | COLOR_PAIR(POS_COLOR_STAFF),
                                "직원 호출: 활성");
            } else {
                ui_pos_print_in(side, 7, 0, A_DIM, "직원 호출: 없음");
            }
            ui_pos_draw_button_in(side, 10, POS_COLOR_ACTIVE, "ENTER  결제하기");
            ui_pos_draw_button_in(side, 13, POS_COLOR_STAFF, "C  직원호출 해제");
        }

        pthread_mutex_lock(&ctx->lock);
        if (ctx->last_staff_message[0] != '\0') {
            ui_pos_print_in(side, side.h - 4, 0, A_BOLD, "최근 호출");
            ui_pos_print_in(side, side.h - 3, 0, A_NORMAL, "%s",
                            ctx->last_staff_message);
        }
        pthread_mutex_unlock(&ctx->lock);
    }

    move(footer.top, 0);
    clrtoeol();
    mvprintw(footer.top, 1,
             "방향키 이동 · Enter 결제 · q 종료");
    move(footer.top + 1, 0);
    clrtoeol();
    mvprintw(footer.top + 1, 1,
             "표시: %s(밖) / %s(안) / %s(주문있음) / %s(직원호출)",
             "CYAN", "YELLOW", "GREEN", "RED");
}

static void ui_draw_pos_payment(ServerContext *ctx, int rows, int cols,
                                int pay_table, int sel) {
    clear();
    UiRect header, main, side, footer;
    ui_pos_layout_frame(rows, cols, POS_TAB_PAYMENT, &header, &main, &side, &footer);
    ui_pos_draw_box(main, "주문 목록");
    if (side.w > 0) {
        ui_pos_draw_box(side, "결제 요약");
    }

    char tlabel[48];
    tlabel[0] = '\0';
    if (pay_table > 0) {
        StoreLayout tmp;
        layout_init(&tmp);
        char err[64];
        if (layout_load(LAYOUT_PATH, &tmp, err, sizeof(err)) == 0) {
            TablePlacement *tp = layout_find_by_id(&tmp, pay_table);
            if (tp) {
                snprintf(tlabel, sizeof(tlabel), "%s", tp->label);
            }
        }
    }

    if (side.w > 0) {
        ui_pos_print_in(side, 0, 0, A_NORMAL, "대상 테이블");
        if (pay_table > 0) {
            ui_pos_print_in(side, 1, 0, A_BOLD, "T%02d %s", pay_table, tlabel);
        } else {
            ui_pos_print_in(side, 1, 0, A_DIM, "(미지정)  t 로 설정");
        }
        PosTableSummary sum;
        if (pay_table > 0) {
            ui_pos_table_summary(ctx, pay_table, &sum);
            ui_pos_print_in(side, 3, 0, A_NORMAL, "미결제 합계");
            ui_pos_print_in(side, 4, 0, A_BOLD, "₩%d", sum.unpaid_total);
            ui_pos_print_in(side, 6, 0, A_NORMAL, "결제가능(DONE)");
            ui_pos_print_in(side, 7, 0, A_BOLD, "₩%d", sum.done_total);
        }
        ui_pos_print_in(side, side.h - 5, 0, A_DIM, "Enter: 선택 주문 결제(DONE)");
        ui_pos_print_in(side, side.h - 4, 0, A_DIM, "y: 해당 테이블 DONE 전체 결제");
        ui_pos_print_in(side, side.h - 3, 0, A_DIM, "t: 테이블 변경");
    }

    int line = 0;
    ui_pos_print_in(main, line++, 0, A_DIM,
                    "필터: %s", pay_table > 0 ? "선택 테이블" : "전체");
    line++;
    pthread_mutex_lock(&ctx->lock);
    int shown = 0;
    int max_rows = main.h - 3 - line;
    for (int i = 0; i < ctx->order_count && shown < max_rows; ++i) {
        Order *o = &ctx->orders[i];
        if (pay_table > 0 && o->table_id != pay_table) {
            continue;
        }
        attr_t a = (shown == sel ? A_REVERSE : A_NORMAL);
        char first[40] = "";
        if (o->item_count > 0) {
            snprintf(first, sizeof(first), "%.*s x%d",
                     (int)sizeof(first) - 6, o->items[0].name,
                     o->items[0].qty);
        }
        ui_pos_print_in(main, line + shown, 0, a,
                        "#%04d  T%02d  %-8s  ₩%-7d  %s",
                        o->order_id, o->table_id, status_to_string(o->status),
                        o->total_price, first);
        shown++;
    }
    pthread_mutex_unlock(&ctx->lock);

    move(footer.top, 0);
    clrtoeol();
    mvprintw(footer.top, 1,
             "↑↓ 선택 · Enter 결제 · y 전체결제 · t 테이블변경 · q 종료");
    move(footer.top + 1, 0);
    clrtoeol();
    mvprintw(footer.top + 1, 1, "팁: DONE 상태만 결제 가능합니다.");
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
    clear();
    UiRect header, main, side, footer;
    ui_pos_layout_frame(rows, cols, POS_TAB_MENU,
                        &header, &main, &side, &footer);

    ui_pos_draw_box(main, "메뉴 목록");
    if (side.w > 0) {
        ui_pos_draw_box(side, "선택 항목");
    }

    pthread_mutex_lock(&ctx->lock);

    int mc = ctx->menu.count;
    int base = 0;

    ui_pos_print_in(main, base++, 0, A_DIM,
                    "a 추가 · e 수정 · d 삭제 · s 품절 · c 카테고리 · p 인기");
    base++;

    int shown = 0;
    int max_rows = main.h - 3 - base;

    for (int i = 0; i < mc && shown < max_rows; ++i) {
        MenuItem *m = &ctx->menu.items[i];
        attr_t a = (i == sel ? A_REVERSE : A_NORMAL);

        ui_pos_print_in(main, base + shown, 0, a,
                        "%3d  %-22s  ₩%-6d  %-8s  %s %s",
                        m->id,
                        m->name,
                        m->price,
                        m->category[0] ? m->category : "기타",
                        m->popular ? "[인기]" : "",
                        m->sold_out ? "[품절]" : "");

        shown++;
    }

    if (side.w > 0) {
        if (mc <= 0) {
            ui_pos_print_in(side, 0, 0, A_DIM, "메뉴가 없습니다.");
        } else {
            if (sel < 0) {
                sel = 0;
            }
            if (sel >= mc) {
                sel = mc - 1;
            }

            MenuItem *m = &ctx->menu.items[sel];

            ui_pos_print_in(side, 0, 0, A_BOLD, "%s", m->name);
            ui_pos_print_in(side, 1, 0, A_DIM, "ID: %d", m->id);

            ui_pos_print_in(side, 3, 0, A_NORMAL, "가격");
            ui_pos_print_in(side, 4, 0, A_BOLD, "₩%d", m->price);

            ui_pos_print_in(side, 6, 0, A_NORMAL, "카테고리");
            ui_pos_print_in(side, 7, 0, A_BOLD, "%s",
                            m->category[0] ? m->category : "기타");

            ui_pos_print_in(side, 9, 0, A_NORMAL, "인기메뉴");
            ui_pos_print_in(side, 10, 0, A_BOLD, "%s",
                            m->popular ? "예" : "아니오");

            ui_pos_print_in(side, 12, 0, A_NORMAL, "상태");
            ui_pos_print_in(side, 13, 0,
                            m->sold_out
                                ? (A_BOLD | COLOR_PAIR(POS_COLOR_STAFF))
                                : (A_BOLD | COLOR_PAIR(POS_COLOR_ACTIVE)),
                            "%s", m->sold_out ? "품절" : "판매중");
        }

        ui_pos_print_in(side, side.h - 4, 0, A_DIM, "c: 카테고리 변경");
        ui_pos_print_in(side, side.h - 3, 0, A_DIM, "p: 인기메뉴 토글");
        ui_pos_print_in(side, side.h - 2, 0, A_DIM, "↑↓ 선택 · q 종료");
    }

    pthread_mutex_unlock(&ctx->lock);

    move(footer.top, 0);
    clrtoeol();
    mvprintw(footer.top, 1,
             "↑↓ 선택 · a 추가 · e 수정 · d 삭제 · s 품절 · c 카테고리 · p 인기 · q 종료");

    move(footer.top + 1, 0);
    clrtoeol();
    mvprintw(footer.top + 1, 1,
             "주의: 변경사항은 즉시 서버 메뉴에 반영됩니다.");
}

#define POS_WGETCH_TIMEOUT_MS 200


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
    UiRect header, main, side, footer;
    ui_pos_layout_frame(rows, cols, POS_TAB_LAYOUT, &header, &main, &side, &footer);
    ui_pos_draw_box(main, "배치 편집");
    if (side.w > 0) {
        ui_pos_draw_box(side, "도움말 / 선택");
    }

    int usable_h = main.h - 2;
    int usable_w = main.w - 2;
    if (usable_h < 4 || usable_w < 10) {
        ui_pos_print_in(main, 0, 0, A_BOLD, "터미널 크기를 키워 주세요.");
        return;
    }

    
    const int min_cell_w = 12;
    const int min_cell_h = 6;

    int view_cols = usable_w / min_cell_w;
    int view_rows = usable_h / min_cell_h;
    if (view_cols < 1) {
        view_cols = 1;
    }
    if (view_rows < 1) {
        view_rows = 1;
    }
    if (view_cols > layout->grid_cols) {
        view_cols = layout->grid_cols;
    }
    if (view_rows > layout->grid_rows) {
        view_rows = layout->grid_rows;
    }

    int cell_w = usable_w / view_cols;
    int cell_h = usable_h / view_rows;

    int view_c0 = cur_col - view_cols / 2;
    int view_r0 = cur_row - view_rows / 2;
    if (view_c0 < 0) {
        view_c0 = 0;
    }
    if (view_r0 < 0) {
        view_r0 = 0;
    }
    if (view_c0 + view_cols > layout->grid_cols) {
        view_c0 = layout->grid_cols - view_cols;
        if (view_c0 < 0) {
            view_c0 = 0;
        }
    }
    if (view_r0 + view_rows > layout->grid_rows) {
        view_r0 = layout->grid_rows - view_rows;
        if (view_r0 < 0) {
            view_r0 = 0;
        }
    }

    for (int vr = 0; vr < view_rows; ++vr) {
        for (int vc = 0; vc < view_cols; ++vc) {
            int gr = view_r0 + vr;
            int gc = view_c0 + vc;
            int left = main.left + 1 + vc * cell_w;
            int top = main.top + 1 + vr * cell_h;
            TablePlacement *tp = layout_find_at((StoreLayout *)layout, gr, gc);
            if (tp) {
                PosTableSummary dummy;
                memset(&dummy, 0, sizeof(dummy));
                int sel = (gr == cur_row && gc == cur_col);
                ui_pos_draw_table_box(top, left, cell_h, cell_w, tp, &dummy,
                                      sel, 0);
            } else if (gr == cur_row && gc == cur_col) {
                attrset(A_REVERSE);
                for (int y = 0;
                     y < cell_h && top + y < main.top + main.h - 1;
                     ++y) {
                    mvprintw(top + y, left, "%-*s", cell_w, "");
                }
                attrset(A_NORMAL);
            }
        }
    }

    if (side.w > 0) {
        ui_pos_print_in(side, 0, 0, A_BOLD, "최대 테이블 배치 수");
        ui_pos_print_in(side, 1, 0, A_NORMAL, "행 x 열: %d x %d",
                        layout->grid_rows, layout->grid_cols);
        ui_pos_print_in(side, 2, 0, A_DIM, "활성 테이블: %d",
                        ui_layout_active_count(layout));

        ui_pos_print_in(side, 4, 0, A_BOLD, "단축키");
        ui_pos_print_in(side, 5, 0, A_DIM, "↑↓←→  이동");
        ui_pos_print_in(side, 6, 0, A_DIM, "Enter  선택");
        ui_pos_print_in(side, 7, 0, A_DIM, "a      추가");
        ui_pos_print_in(side, 8, 0, A_DIM, "d      삭제");
        ui_pos_print_in(side, 9, 0, A_DIM, "z      구역 변경");
        ui_pos_print_in(side, 10, 0, A_DIM, "l      테이블 이름");
        ui_pos_print_in(side, 11, 0, A_DIM, "m      이동모드");
        ui_pos_print_in(side, 12, 0, A_DIM, "g      최대 배치 사이즈 수정");
        ui_pos_print_in(side, 13, 0, A_DIM, "s      저장");

        ui_pos_print_in(side, 15, 0, A_BOLD, "커서");
        ui_pos_print_in(side, 16, 0, A_NORMAL, "(%d,%d)", cur_row, cur_col);

        if (edit_sel >= 0 && edit_sel < layout->count) {
            const TablePlacement *tp = &layout->tables[edit_sel];
            ui_pos_print_in(side, 18, 0, A_BOLD, "선택됨");
            ui_pos_print_in(side, 19, 0, A_NORMAL, "id=%d  %s", tp->table_id,
                            tp->label);
            ui_pos_print_in(side, 20, 0, A_DIM, "zone=%s  (%d,%d)",
                            tp->zone == TABLE_ZONE_INSIDE ? "안" : "밖", tp->row,
                            tp->col);
        } else {
            TablePlacement *tp = layout_find_at((StoreLayout *)layout, cur_row, cur_col);
            if (tp) {
                ui_pos_print_in(side, 18, 0, A_BOLD, "커서 위치");
                ui_pos_print_in(side, 19, 0, A_NORMAL, "id=%d  %s", tp->table_id,
                                tp->label);
                ui_pos_print_in(side, 20, 0, A_DIM, "zone=%s  (%d,%d)",
                                tp->zone == TABLE_ZONE_INSIDE ? "안" : "밖", tp->row,
                                tp->col);
            } else {
                ui_pos_print_in(side, 18, 0, A_DIM, "빈 칸");
            }
        }
    }

    move(footer.top, 0);
    clrtoeol();
    mvprintw(footer.top, 1,
             "배치편집: a 추가 · d 삭제 · z 구역 · l 테이블이름 · m 이동 · g 배치사이즈 · s 저장 · q 종료");
    move(footer.top + 1, 0);
    clrtoeol();
    mvprintw(footer.top + 1, 1, "팁: 이동모드(m)에서 Enter로 빈 칸에 배치합니다.");
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
    UiRect header, main, side, footer;
    ui_pos_layout_frame(rows, cols, POS_TAB_SALES, &header, &main, &side, &footer);
    ui_pos_draw_box(main, "매출 로그 (tail)");
    if (side.w > 0) {
        ui_pos_draw_box(side, "정보");
    }

    char chunk[16000];
    char err[128];
    ssize_t n =
        storage_read_tail(SALES_LOG_PATH, chunk, sizeof(chunk), err, sizeof(err));
    if (n < 0) {
        ui_pos_print_in(main, 0, 0, A_DIM, "(로그 없음 또는 읽기 실패)");
    } else {
        int line = 0;
        char *save = NULL;
        char bufcopy[sizeof(chunk)];
        strncpy(bufcopy, chunk, sizeof(bufcopy) - 1);
        bufcopy[sizeof(bufcopy) - 1] = '\0';
        char *ln = strtok_r(bufcopy, "\n", &save);
        while (ln && line < main.h - 3) {
            ui_pos_print_in(main, line, 0, A_NORMAL, "%s", ln);
            line++;
            ln = strtok_r(NULL, "\n", &save);
        }
    }

    if (side.w > 0) {
        ui_pos_print_in(side, 0, 0, A_DIM, "파일");
        ui_pos_print_in(side, 1, 0, A_BOLD, "%s", SALES_LOG_PATH);
        ui_pos_print_in(side, 3, 0, A_DIM, "새로고침");
        ui_pos_print_in(side, 4, 0, A_NORMAL, "자동(200ms) 갱신");
    }

    move(footer.top, 0);
    clrtoeol();
    mvprintw(footer.top, 1, "q 종료 · F1~F5 탭 이동");
    move(footer.top + 1, 0);
    clrtoeol();
    mvprintw(footer.top + 1, 1, "팁: 로그가 너무 길면 화면 크기를 키워주세요.");
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
    uint32_t last_staff_rev = 0;
    int dirty = 1;
    time_t last_auto_redraw = 0;

    while (!ctx->shutting_down) {
        int rows = 0, cols = 0;
        getmaxyx(stdscr, rows, cols);
        if ((uint32_t)ctx->orders_revision != last_rev) {
            last_rev = (uint32_t)ctx->orders_revision;
            sel_payment = 0;
            dirty = 1;
        }
        if ((uint32_t)ctx->staff_revision != last_staff_rev) {
            last_staff_rev = (uint32_t)ctx->staff_revision;
            dirty = 1;
        }

        
        if (!dirty && tab == POS_TAB_SALES) {
            time_t now = time(NULL);
            if (now != last_auto_redraw) { 
                last_auto_redraw = now;
                dirty = 1;
            }
        }

        if (dirty) {
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
            refresh();
            dirty = 0;
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
            dirty = 1;
        }
        if (ch == KEY_F(2)) {
            tab = POS_TAB_PAYMENT;
            dirty = 1;
        }
        if (ch == KEY_F(3)) {
            tab = POS_TAB_MENU;
            dirty = 1;
        }
        if (ch == KEY_F(4)) {
            tab = POS_TAB_SALES;
            dirty = 1;
        }
        if (ch == KEY_F(5)) {
            tab = POS_TAB_LAYOUT;
            dirty = 1;
        }

        if (tab == POS_TAB_HOME) {
            if (ch == KEY_UP && grid_row > 0) {
                grid_row--;
                dirty = 1;
            }
            if (ch == KEY_DOWN && grid_row + 1 < layout.grid_rows) {
                grid_row++;
                dirty = 1;
            }
            if (ch == KEY_LEFT && grid_col > 0) {
                grid_col--;
                dirty = 1;
            }
            if (ch == KEY_RIGHT && grid_col + 1 < layout.grid_cols) {
                grid_col++;
                dirty = 1;
            }
            TablePlacement *tp =
                layout_find_at(&layout, grid_row, grid_col);
            if ((ch == '\n' || ch == KEY_ENTER) && tp) {
                pay_table = tp->table_id;
                tab = POS_TAB_PAYMENT;
                sel_payment = 0;
                dirty = 1;
            }
            if (ch == 'c' && tp) {
                char err[160];
                pthread_mutex_lock(&ctx->lock);
                if (tp->table_id > 0 && tp->table_id <= MAX_TABLES) {
                    ctx->staff_call_at[tp->table_id] = 0;
                }
                pthread_mutex_unlock(&ctx->lock);
                server_touch_staff(ctx);
                (void)err;
                dirty = 1;
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
                    dirty = 1;
                }
            }
            if (ch == KEY_UP) {
                sel_payment = (sel_payment + nshow - 1) % (nshow ? nshow : 1);
                dirty = 1;
            }
            if (ch == KEY_DOWN) {
                sel_payment = (sel_payment + 1) % (nshow ? nshow : 1);
                dirty = 1;
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
                        dirty = 1;
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
                if (nids > 0) {
                    dirty = 1;
                }
            }
        }

        if (tab == POS_TAB_MENU) {
            pthread_mutex_lock(&ctx->lock);
            int mc = ctx->menu.count;
            pthread_mutex_unlock(&ctx->lock);
            if (ch == KEY_UP) {
                sel_menu = (sel_menu + mc - 1) % (mc ? mc : 1);
                dirty = 1;
            }
            if (ch == KEY_DOWN) {
                sel_menu = (sel_menu + 1) % (mc ? mc : 1);
                dirty = 1;
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
                dirty = 1;
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
                dirty = 1;
            }
            if (ch == 'a') {
                MenuItem mi;
                memset(&mi, 0, sizeof(mi));

                char namebuf[MAX_NAME];
                char catbuf[MAX_CATEGORY];
                int price = 0;
                int popular = 0;

                namebuf[0] = '\0';
                catbuf[0] = '\0';

                ui_prompt_string(stdscr, rows - 8, "이름: ",
                                namebuf, sizeof(namebuf), 1);

                ui_prompt_int(stdscr, rows - 7, "가격: ", &price);

                ui_prompt_string(stdscr, rows - 6,
                                "카테고리(식사류/고기류/주류/음료/기타): ",
                                catbuf, sizeof(catbuf), 0);

                ui_prompt_int(stdscr, rows - 5,
                            "인기메뉴? 1=예 0=아니오: ",
                            &popular);

                strncpy(mi.name, namebuf, sizeof(mi.name) - 1);
                mi.name[sizeof(mi.name) - 1] = '\0';

                mi.price = price;

                strncpy(mi.category,
                        catbuf[0] ? catbuf : "기타",
                        sizeof(mi.category) - 1);
                mi.category[sizeof(mi.category) - 1] = '\0';

                mi.popular = popular ? 1 : 0;
                mi.sold_out = 0;

                char err[160];

                pthread_mutex_lock(&ctx->lock);
                menu_add_item(&ctx->menu, &mi, err, sizeof(err));
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);

                server_broadcast_line(ctx, "MENU_SYNC\n");
                dirty = 1;
            }
            if (ch == 'e' && mc > 0) {
                pthread_mutex_lock(&ctx->lock);
                int id = ctx->menu.items[sel_menu].id;
                MenuItem mi = ctx->menu.items[sel_menu];
                pthread_mutex_unlock(&ctx->lock);

                char nb[MAX_NAME];
                char cb[MAX_CATEGORY];

                strncpy(nb, mi.name, sizeof(nb) - 1);
                nb[sizeof(nb) - 1] = '\0';

                strncpy(cb,
                        mi.category[0] ? mi.category : "기타",
                        sizeof(cb) - 1);
                cb[sizeof(cb) - 1] = '\0';

                ui_prompt_string(stdscr, rows - 8,
                                "새 이름: ", nb, sizeof(nb), 0);

                ui_prompt_int(stdscr, rows - 7,
                            "새 가격: ", &mi.price);

                ui_prompt_string(stdscr, rows - 6,
                                "새 카테고리: ", cb, sizeof(cb), 0);

                ui_prompt_int(stdscr, rows - 5,
                            "인기메뉴? 1=예 0=아니오: ",
                            &mi.popular);

                strncpy(mi.name, nb, sizeof(mi.name) - 1);
                mi.name[sizeof(mi.name) - 1] = '\0';

                strncpy(mi.category,
                        cb[0] ? cb : "기타",
                        sizeof(mi.category) - 1);
                mi.category[sizeof(mi.category) - 1] = '\0';

                mi.popular = mi.popular ? 1 : 0;

                char err[160];

                pthread_mutex_lock(&ctx->lock);
                menu_update_item(&ctx->menu, id, &mi, err, sizeof(err));
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);

                server_broadcast_line(ctx, "MENU_SYNC\n");
                dirty = 1;
            }
            if (ch == 'c' && mc > 0) {
                pthread_mutex_lock(&ctx->lock);

                int id = ctx->menu.items[sel_menu].id;
                char catbuf[MAX_CATEGORY];

                strncpy(catbuf,
                        ctx->menu.items[sel_menu].category[0]
                            ? ctx->menu.items[sel_menu].category
                            : "기타",
                        sizeof(catbuf) - 1);
                catbuf[sizeof(catbuf) - 1] = '\0';

                pthread_mutex_unlock(&ctx->lock);

                ui_prompt_string(stdscr, rows - 5,
                                "새 카테고리: ",
                                catbuf, sizeof(catbuf), 0);

                char err[160];

                pthread_mutex_lock(&ctx->lock);
                menu_set_category(&ctx->menu, id,
                                catbuf[0] ? catbuf : "기타",
                                err, sizeof(err));
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));
                pthread_mutex_unlock(&ctx->lock);

                server_broadcast_line(ctx, "MENU_SYNC\n");
                dirty = 1;
            }

            if (ch == 'p' && mc > 0) {
                char err[160];

                pthread_mutex_lock(&ctx->lock);

                int id = ctx->menu.items[sel_menu].id;
                int cur = ctx->menu.items[sel_menu].popular;

                menu_set_popular(&ctx->menu, id, cur ? 0 : 1, err, sizeof(err));
                menu_save_file(&ctx->menu, MENU_PATH, err, sizeof(err));

                pthread_mutex_unlock(&ctx->lock);

                server_broadcast_line(ctx, "MENU_SYNC\n");
                dirty = 1;
            }
        }

        if (tab == POS_TAB_LAYOUT) {
            if (ch == KEY_UP && grid_row > 0) {
                grid_row--;
                dirty = 1;
            }
            if (ch == KEY_DOWN && grid_row + 1 < layout.grid_rows) {
                grid_row++;
                dirty = 1;
            }
            if (ch == KEY_LEFT && grid_col > 0) {
                grid_col--;
                dirty = 1;
            }
            if (ch == KEY_RIGHT && grid_col + 1 < layout.grid_cols) {
                grid_col++;
                dirty = 1;
            }
            layout_edit_idx = ui_pos_layout_find_index(&layout, grid_row,
                                                     grid_col);

            if (ch == 'g') {
                int nr = 0, nc = 0;
                ui_prompt_int(stdscr, rows - 6, "최대 배치 행(rows): ", &nr);
                ui_prompt_int(stdscr, rows - 5, "최대 배치 열(cols): ", &nc);
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
                    dirty = 1;
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
                dirty = 1;
            }

            if (ch == 'd' && layout_edit_idx >= 0) {
                ui_pos_layout_remove_at(&layout, layout_edit_idx);
                layout_edit_idx = -1;
                dirty = 1;
            }

            if (ch == 'z' && layout_edit_idx >= 0) {
                TablePlacement *tp = &layout.tables[layout_edit_idx];
                tp->zone = tp->zone == TABLE_ZONE_INSIDE ? TABLE_ZONE_OUTSIDE
                                                       : TABLE_ZONE_INSIDE;
                dirty = 1;
            }

            if (ch == 'l' && layout_edit_idx >= 0) {
                char lb[MAX_TABLE_LABEL];
                lb[0] = '\0';
                ui_prompt_string(stdscr, rows - 5, "테이블 이름: ", lb,
                                 sizeof(lb), 0);
                if (lb[0]) {
                    snprintf(layout.tables[layout_edit_idx].label,
                             MAX_TABLE_LABEL, "%s", lb);
                }
                dirty = 1;
            }

            if (ch == 'm' && layout_edit_idx >= 0) {
                layout_move_mode = !layout_move_mode;
                dirty = 1;
            }

            if (layout_move_mode && layout_edit_idx >= 0 &&
                (ch == '\n' || ch == KEY_ENTER)) {
                if (!layout_find_at(&layout, grid_row, grid_col)) {
                    layout.tables[layout_edit_idx].row = grid_row;
                    layout.tables[layout_edit_idx].col = grid_col;
                }
                layout_move_mode = 0;
                dirty = 1;
            }

            if (ch == 's') {
                char err[160];
                ui_pos_sync_max_tables(ctx, &layout);
                if (layout_save(LAYOUT_PATH, &layout, err, sizeof(err)) == 0) {
                    server_save_config(ctx, err, sizeof(err));
                }
                dirty = 1;
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

typedef enum {
    TABLE_FOCUS_MENU = 0,
    TABLE_FOCUS_SIDEBAR
} TableFocus;

#define SIDEBAR_ALL     0
#define SIDEBAR_POPULAR 1
#define SIDEBAR_MEAL    2
#define SIDEBAR_MEAT    3
#define SIDEBAR_ALCOHOL 4
#define SIDEBAR_DRINK   5
#define SIDEBAR_CART    6
#define SIDEBAR_ORDER   7
#define SIDEBAR_CALL    8
#define SIDEBAR_COUNT   9

static const char *TABLE_CATEGORY_NAMES[] = {
    "전체 메뉴",
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
static int last_menu_category = -1;
static int table_confirm_popup = 0;
static int table_call_popup_ticks = 0;
static TableScreen last_table_screen = -1;

static void ui_table_reset_images(void) {
    last_menu_page = -1;
    last_menu_rows = -1;
    last_menu_cols = -1;
    last_menu_count = -1;
    last_menu_sel = -1;
    last_menu_category = -1;
}

static int ui_menu_is_selectable(const MenuCatalog *cat, int idx) {
    if (!cat || idx < 0 || idx >= cat->count) {
        return 0;
    }

    return !cat->items[idx].sold_out;
}

static void ui_trim_field(char *s) {
    if (!s) {
        return;
    }

    int len = (int)strlen(s);

    while (len > 0 &&
           (s[len - 1] == ' ' ||
            s[len - 1] == '\t' ||
            s[len - 1] == '\r' ||
            s[len - 1] == '\n')) {
        s[len - 1] = '\0';
        len--;
    }

    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') {
        start++;
    }

    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

static const char *ui_category_value_from_sidebar(int sidebar_category) {
    switch (sidebar_category) {
    case SIDEBAR_MEAL:
        return "식사류";
    case SIDEBAR_MEAT:
        return "고기류";
    case SIDEBAR_ALCOHOL:
        return "주류";
    case SIDEBAR_DRINK:
        return "음료";
    default:
        return "";
    }
}

static int ui_menu_match_category(const MenuItem *m, int current_category) {
    if (!m) {
        return 0;
    }

    if (current_category == SIDEBAR_ALL) {
        return 1;
    }

    if (current_category == SIDEBAR_POPULAR) {
        return m->popular ? 1 : 0;
    }

    char saved[MAX_CATEGORY];
    char target[MAX_CATEGORY];

    strncpy(saved, m->category, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';

    strncpy(target, ui_category_value_from_sidebar(current_category),
            sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';

    ui_trim_field(saved);
    ui_trim_field(target);

    if (target[0] == '\0') {
        return 1;
    }

    return strcmp(saved, target) == 0;
}

static int ui_menu_visible_count(const MenuCatalog *cat, int current_category) {
    int n = 0;

    if (!cat) {
        return 0;
    }

    for (int i = 0; i < cat->count; ++i) {
        if (ui_menu_match_category(&cat->items[i], current_category)) {
            n++;
        }
    }

    return n;
}

static int ui_menu_index_at_visible(const MenuCatalog *cat,
                                    int current_category,
                                    int visible_index) {
    int shown = 0;

    if (!cat) {
        return -1;
    }

    for (int i = 0; i < cat->count; ++i) {
        if (!ui_menu_match_category(&cat->items[i], current_category)) {
            continue;
        }

        if (shown == visible_index) {
            return i;
        }

        shown++;
    }

    return -1;
}

static int ui_menu_visible_pos_of_index(const MenuCatalog *cat,
                                        int current_category,
                                        int real_index) {
    int shown = 0;

    if (!cat || real_index < 0 || real_index >= cat->count) {
        return -1;
    }

    for (int i = 0; i < cat->count; ++i) {
        if (!ui_menu_match_category(&cat->items[i], current_category)) {
            continue;
        }

        if (i == real_index) {
            return shown;
        }

        shown++;
    }

    return -1;
}

static int ui_menu_first_selectable_in_category(const MenuCatalog *cat,
                                                int current_category) {
    if (!cat) {
        return -1;
    }

    for (int i = 0; i < cat->count; ++i) {
        if (!ui_menu_match_category(&cat->items[i], current_category)) {
            continue;
        }

        if (ui_menu_is_selectable(cat, i)) {
            return i;
        }
    }

    return -1;
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


static int ui_find_selectable_visible_from(const MenuCatalog *cat,
                                           int current_category,
                                           int visible_start,
                                           int visible_step) {
    int visible_count = ui_menu_visible_count(cat, current_category);
    int v = visible_start;

    if (!cat || visible_count <= 0 || visible_step == 0) {
        return -1;
    }

    while (v >= 0 && v < visible_count) {
        int real_idx = ui_menu_index_at_visible(cat, current_category, v);

        if (real_idx >= 0 && ui_menu_is_selectable(cat, real_idx)) {
            return real_idx;
        }

        v += visible_step;
    }

    return -1;
}

static void ui_draw_table_header(const char *table_name, int table_id,
                                 int cart_qty, int cols,
                                 const char *banner) {
    mvprintw(0, 0, "%-*s", cols - 1, "");
    mvprintw(0, 2, "%s (Table %d) | 테이블 오더", table_name, table_id);
    mvprintw(0, cols - 18, "장바구니 %d개", cart_qty);
    mvhline(1, 0, ACS_HLINE, cols - 1);
    mvprintw(1, 0, "%-*s", cols - 1, "");

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


static void ui_draw_table_menu(const MenuCatalog *cat, TableScreen scr,
                               const Cart *cart, int rows, int cols, int sel,
                               const char *table_name,
                               int table_id, const char *banner,
                               TableFocus focus, int sidebar_sel,
                               int current_category) {
    int base = 3;

        if (scr != TABLE_SCR_MENU) {
            clear();
            mvprintw(0, 0, "%s (Table %d) | 화면:%s | 장바구니 줄 %d",
                     table_name, table_id,
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

        int visible_count = ui_menu_visible_count(cat, current_category);
        int visible_sel = ui_menu_visible_pos_of_index(cat, current_category, sel);

        if (visible_sel < 0) {
            visible_sel = 0;
        }

        int page = visible_sel / page_size;
        int page_start = page * page_size;
        int total_pages = (visible_count + page_size - 1) / page_size;

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
            visible_count != last_menu_count ||
            current_category != last_menu_category) {
            need_image_redraw = 1;
        }

        if (need_image_redraw) {
            clear();
        }

        ui_draw_table_header(table_name, table_id, ui_cart_total_qty(cart), cols,
                             banner);

        ui_attr_on(CP_SIDEBAR);
        ui_draw_box(body_y, 0, sidebar_w, body_h);
        mvprintw(body_y + 1, 2, "카테고리");
        ui_attr_off(CP_SIDEBAR);

        for (int i = 0; i < 6; ++i) {
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
        int notice_y = cart_y - 5;
        (void)ui_cart_total_qty(cart);

        if (notice_y >= body_y + 3) {
            if (table_call_popup_ticks > 0) {
                ui_draw_sidebar_notice(notice_y, button_x, button_w);
            } else {
                ui_clear_sidebar_notice(notice_y, button_x, button_w);
            }
        }

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

int dbg_meat = 0;
int dbg_meal = 0;
int dbg_alcohol = 0;
int dbg_drink = 0;
int dbg_popular = 0;

for (int di = 0; di < cat->count; ++di) {
    if (strcmp(cat->items[di].category, "고기류") == 0) {
        dbg_meat++;
    }
    if (strcmp(cat->items[di].category, "식사류") == 0) {
        dbg_meal++;
    }
    if (strcmp(cat->items[di].category, "주류") == 0) {
        dbg_alcohol++;
    }
    if (strcmp(cat->items[di].category, "음료") == 0) {
        dbg_drink++;
    }
    if (cat->items[di].popular) {
        dbg_popular++;
    }
}

mvprintw(body_y + 1, menu_x + 2,
         "DEBUG all=%d pop=%d meat=%d meal=%d alcohol=%d drink=%d",
         cat->count,
         dbg_popular,
         dbg_meat,
         dbg_meal,
         dbg_alcohol,
         dbg_drink);

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
            int visible_idx = page_start + n;
            int idx = ui_menu_index_at_visible(cat, current_category, visible_idx);

            int gr = n / grid_cols;
            int gc = n % grid_cols;

            int y = grid_top + gr * (cell_h + cell_gap_y);
            int x = menu_x + 2 + gc * (cell_w + cell_gap_x);

            int inner_x = x + 1;
            int inner_w = cell_w - 2;

            int selected = (idx >= 0 && idx == sel && focus == TABLE_FOCUS_MENU);

            ui_draw_card_box(y, x, cell_w, cell_h, selected);

            if (idx < 0 || idx >= cat->count) {
                mvprintw(y + cell_h - 3, inner_x, "%-*s", inner_w, "");
                mvprintw(y + cell_h - 2, inner_x, "%-*s", inner_w, "");
                continue;
            }

            const MenuItem *m = &cat->items[idx];

            int text_x = x + 1;
            int name_y = y + cell_h - 3;
            int price_y = y + cell_h - 2;
            int text_w = cell_w - 2;

            mvprintw(name_y, text_x, "%-*s", text_w, "");
            mvprintw(price_y, text_x, "%-*s", text_w, "");

            if (selected) {
                attron(A_REVERSE | A_BOLD);
            }

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
        }

        int info_y = footer_y - 1;
        mvhline(info_y - 1, menu_x, ACS_HLINE, menu_w - 1);

        if (cat->count > 0 && sel >= 0 && sel < cat->count && ui_menu_match_category(&cat->items[sel], current_category)) {
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
                 "← 사이드바  → 메뉴판  ↑↓ 선택  SPACE 실행/담기  q 종료");
        ui_attr_off(CP_FOOTER);

        refresh();

        if (need_image_redraw) {
            for (int n = 0; n < page_size; ++n) {
                int visible_idx = page_start + n;
                int idx = ui_menu_index_at_visible(cat, current_category, visible_idx);

                if (idx < 0 || idx >= cat->count) {
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
            
            refresh();
        }

        last_menu_page = page;
        last_menu_rows = rows;
        last_menu_cols = cols;
        last_menu_count = visible_count;
        last_menu_sel = sel;
        last_table_screen = scr;
        last_menu_category = current_category;
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
            mvprintw(rows - 2, 0, "m 메뉴판  q 종료");


            if (has_colors()) {
                attroff(COLOR_PAIR(CP_FOOTER));
            }


            return;
        }

        
        ui_draw_simple_box(list_y, list_x, list_w, box_h);
        attron(A_BOLD);
        mvprintw(list_y, list_x + 2, " 주문 메뉴 ");
        attroff(A_BOLD);

        
        ui_draw_simple_box(list_y, summary_x, summary_w, box_h);
        attron(A_BOLD);
        mvprintw(list_y, summary_x + 2, " 주문 요약 ");
        attroff(A_BOLD);

        
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

            
            char total_buf[32];
            snprintf(total_buf, sizeof(total_buf), "%d원", line_total);
            mvprintw(y + 1,
                    list_x + list_w - 2 - (int)strlen(total_buf),
                    "%s",
                    total_buf);
        }

        
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

static void ui_draw_order_box(int y, int x, int w, int h) {
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

static void ui_print_money(int y, int x, int value) {
    mvprintw(y, x, "%d원", value);
}

static int ui_table_count_orders(Order *orders, int count, int table_id) {
    int n = 0;

    for (int i = 0; i < count; ++i) {
        if (orders[i].table_id == table_id) {
            n++;
        }
    }

    return n;
}

static void ui_draw_table_status_lines(Order *orders, int count, int table_id,
                                       int rows, int cols, int base, int sel) {
    int body_y = base;
    int body_h = rows - body_y - 3;

    int box_x = 2;
    int box_y = body_y;
    int box_w = cols - 4;
    int box_h = body_h;

    int order_no = 0;
    int total_amount = 0;
    int max_visible = 0;
    int start_order = 0;

    if (cols < 70 || rows < 18) {
        mvprintw(base, 0,
                 "터미널 크기가 너무 작습니다. 주문내역 화면을 보려면 창을 더 크게 해주세요.");
        mvprintw(rows - 2, 0, "m 메뉴화면  q 종료");
        return;
    }

    max_visible = (box_h - 4) / 5;
    if (max_visible < 1) {
        max_visible = 1;
    }

    if (sel >= max_visible) {
        start_order = sel - max_visible + 1;
    }

    ui_draw_order_box(box_y, box_x, box_w, box_h);

    attron(A_BOLD);
    mvprintw(box_y, box_x + 2, " 주문 내역 ");
    attroff(A_BOLD);

    int line = box_y + 2;
    int visible_no = 0;

    for (int i = 0; i < count && visible_no < max_visible; ++i) {
        Order *o = &orders[i];

        if (o->table_id != table_id) {
            continue;
        }

        if (order_no++ < start_order) {
            continue;
        }

        total_amount += o->total_price;

        int selected = (order_no - 1 == sel);

        
        if (line + 5 >= box_y + box_h - 1) {
            mvprintw(line, box_x + 2, "... 더 많은 주문이 있습니다 ...");
            break;
        }

        if (selected) {
            if (has_colors()) {
                attron(COLOR_PAIR(CP_MENU_SEL));
            }
            attron(A_BOLD);
            mvprintw(line, box_x + 2, "▶ 주문 #%04d", o->order_id);
            attroff(A_BOLD);
            if (has_colors()) {
                attroff(COLOR_PAIR(CP_MENU_SEL));
            }
        } else {
            attron(A_BOLD);
            mvprintw(line, box_x + 2, "  주문 #%04d", o->order_id);
            attroff(A_BOLD);
        }

        mvprintw(line, box_x + 20, "상태: %s", status_to_string(o->status));

        if (o->status == STATUS_WAITING) {
            mvprintw(line, box_x + box_w - 24, "[취소 가능]");
        } else if (o->status == STATUS_COOKING) {
            mvprintw(line, box_x + box_w - 26, "[조리중 취소 불가]");
        }

        mvprintw(line + 1, box_x + 4, "합계: ");
        ui_print_money(line + 1, box_x + 10, o->total_price);

        mvprintw(line + 2, box_x + 4, "메뉴: ");

        int item_x = box_x + 10;
        int item_y = line + 2;

        for (int k = 0; k < o->item_count && k < 5; ++k) {
            char item_buf[80];

            
            snprintf(item_buf, sizeof(item_buf), "%.*s x%d",
                     (int)sizeof(item_buf) - 6,
                     o->items[k].name, o->items[k].qty);

            if (item_x + (int)strlen(item_buf) + 2 >= box_x + box_w - 2) {
                item_y++;
                item_x = box_x + 10;

                if (item_y >= line + 4) {
                    mvprintw(item_y, item_x, "...");
                    break;
                }
            }

            mvprintw(item_y, item_x, "%s", item_buf);
            item_x += (int)strlen(item_buf) + 3;
        }

        mvhline(line + 4, box_x + 2, ACS_HLINE, box_w - 4);

        line += 5;
        visible_no++;
    }

    if (order_no == 0) {
        const char *msg1 = "아직 주문 내역이 없습니다";
        const char *msg2 = "장바구니에서 주문을 확정해 주세요";

        int cy = box_y + box_h / 2 - 1;

        attron(A_BOLD);
        mvprintw(cy, (cols - (int)strlen(msg1)) / 2, "%s", msg1);
        attroff(A_BOLD);

        mvprintw(cy + 2, (cols - (int)strlen(msg2)) / 2, "%s", msg2);
    }

    if (has_colors()) {
        attron(COLOR_PAIR(CP_MENU_SEL));
    }

    attron(A_BOLD);
    mvprintw(rows - 3, 2,
            "%-*s",
            cols - 4,
            "");
    mvprintw(rows - 3, 2,
            "현재 테이블 주문 총액: %d원", total_amount);
    attroff(A_BOLD);

    if (has_colors()) {
        attrset(COLOR_PAIR(CP_FOOTER));
    } else {
        attrset(A_BOLD);
    }

    mvprintw(rows - 1, 0,
             "↑↓ 주문 선택  c 취소(추후 연결)  m 메뉴화면  q 종료");

    if (has_colors()) {
        attroff(COLOR_PAIR(CP_FOOTER));
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

    char table_name[64];
    snprintf(table_name, sizeof(table_name), "T%d", args->table_id);
    {
        StoreLayout lay;
        char lerr[128];
        if (layout_load(LAYOUT_PATH, &lay, lerr, sizeof(lerr)) == 0) {
            TablePlacement *tp = layout_find_by_id(&lay, args->table_id);
            if (tp && tp->label[0]) {
                snprintf(table_name, sizeof(table_name), "%s", tp->label);
            }
        }
    }

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
    int sidebar_sel = SIDEBAR_ALL;
    int current_category = SIDEBAR_ALL;
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
                               table_name,
                               args->table_id, banner, focus, sidebar_sel,
                               current_category);
        } else {
            clear();
            mvprintw(0, 0, "%s (Table %d) 주문 상태", table_name, args->table_id);
            int status_count = ui_table_count_orders(mirror, mirror_count,
                                                       args->table_id);
            if (sel >= status_count) {
                sel = status_count - 1;
            }
            if (sel < 0) {
                sel = 0;
            }
            ui_draw_table_status_lines(mirror, mirror_count, args->table_id,
                                       rows, cols, 3, sel);
            refresh();
        }

        if (table_call_popup_ticks > 0) {
            table_call_popup_ticks--;
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

            int old_visible_sel =
                ui_menu_visible_pos_of_index(&cat, current_category, sel);
            if (old_visible_sel < 0) {
                old_visible_sel = 0;
            }
            int old_page = old_visible_sel / page_size;

            if (ch == KEY_LEFT) {
                if (focus == TABLE_FOCUS_MENU) {
                    int visible_sel =
                        ui_menu_visible_pos_of_index(&cat, current_category, sel);

                    if (visible_sel < 0) {
                        visible_sel = 0;
                    }

                    if (visible_sel % grid_cols == 0) {
                        focus = TABLE_FOCUS_SIDEBAR;
                        sidebar_sel = current_category;
                    } else {
                        int next =
                            ui_find_selectable_visible_from(&cat,
                                                            current_category,
                                                            visible_sel - 1,
                                                            -1);

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
                    int visible_sel =
                        ui_menu_visible_pos_of_index(&cat, current_category, sel);

                    if (visible_sel < 0) {
                        visible_sel = 0;
                    }

                    int next =
                        ui_find_selectable_visible_from(&cat,
                                                        current_category,
                                                        visible_sel + 1,
                                                        1);

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
                    int visible_sel =
                        ui_menu_visible_pos_of_index(&cat, current_category, sel);

                    if (visible_sel < 0) {
                        visible_sel = 0;
                    }

                    int next =
                        ui_find_selectable_visible_from(&cat,
                                                        current_category,
                                                        visible_sel - grid_cols,
                                                        -grid_cols);

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
                    int visible_sel =
                        ui_menu_visible_pos_of_index(&cat, current_category, sel);

                    if (visible_sel < 0) {
                        visible_sel = 0;
                    }

                    int next =
                        ui_find_selectable_visible_from(&cat,
                                                        current_category,
                                                        visible_sel + grid_cols,
                                                        grid_cols);

                    if (next >= 0) {
                        sel = next;
                    }
                }
            } else if (ch == ' ') {
                if (focus == TABLE_FOCUS_SIDEBAR) {
                    if (sidebar_sel >= 0 && sidebar_sel <= SIDEBAR_DRINK) {
                        current_category = sidebar_sel;

                        {
                            int first =
                                ui_menu_first_selectable_in_category(&cat, current_category);
                            sel = (first >= 0) ? first : 0;
                        }

                        snprintf(banner, sizeof(banner),
                                "%s 선택됨",
                                TABLE_CATEGORY_NAMES[current_category]);

                        focus = TABLE_FOCUS_MENU;
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

                        snprintf(banner, sizeof(banner),
                                 "직원 호출을 보냈습니다. 잠시만 기다려주세요.");
                        table_call_popup_ticks = 24;
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

        {
            int new_visible_sel =
                ui_menu_visible_pos_of_index(&cat, current_category, sel);

            if (new_visible_sel < 0) {
                new_visible_sel = 0;
            }

            int new_page = new_visible_sel / page_size;

            if (old_page != new_page) {
                ui_table_reset_images();
            }
        }

        if (mc > 0) {
            int visible_count = ui_menu_visible_count(&cat, current_category);

            if (visible_count <= 0) {
                sel = 0;
            } else {
                if (sel < 0 || sel >= mc ||
                    !ui_menu_match_category(&cat.items[sel], current_category)) {
                    int first =
                        ui_menu_first_selectable_in_category(&cat, current_category);
                    sel = (first >= 0) ? first : 0;
                }

                if (sel >= 0 && sel < mc && !ui_menu_is_selectable(&cat, sel)) {
                    int visible_sel =
                        ui_menu_visible_pos_of_index(&cat, current_category, sel);

                    int next = -1;

                    if (visible_sel >= 0) {
                        next =
                            ui_find_selectable_visible_from(&cat,
                                                            current_category,
                                                            visible_sel + 1,
                                                            1);

                        if (next < 0) {
                            next =
                                ui_find_selectable_visible_from(&cat,
                                                                current_category,
                                                                visible_sel - 1,
                                                                -1);
                        }
                    }

                    if (next >= 0) {
                        sel = next;
                    }
                }
            }
        } else {
            sel = 0;
        }

            {
                int new_visible_sel = ui_menu_visible_pos_of_index(&cat, current_category, sel);

                if (new_visible_sel < 0) {
                    new_visible_sel = 0;
                }

                int new_page = new_visible_sel / page_size;

                if (new_page != last_menu_page) {
                    ui_table_reset_images();
                }
            }
        }
        else if (scr == TABLE_SCR_CART) {
    int lines = cart.count;

    
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

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 'z') {
        if (cart.count > 0) {
            table_confirm_popup = 1;
        } else {
            snprintf(banner, sizeof(banner), "장바구니가 비어 있습니다");
        }
    } 
}else if (scr == TABLE_SCR_STATUS) {
            int lines = ui_table_count_orders(mirror, mirror_count,
                                              args->table_id);

            if (ch == KEY_UP && lines > 0) {
                sel = (sel + lines - 1) % lines;
            }

            if (ch == KEY_DOWN && lines > 0) {
                sel = (sel + 1) % lines;
            }

            

            if (ch == 'm') {
                scr = TABLE_SCR_MENU;
                focus = TABLE_FOCUS_MENU;
                ui_table_reset_images();
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

static void ui_draw_kitchen(Order *orders, int count, int sel, int rows, int cols) {
    clear();
    
    mvprintw(0, 0, "Kitchen Display | ↑↓ 선택 · c 조리 · d 완료");

    int col_w = cols / 3;

    
    for (int i = 0; i < 3; ++i) {
        int start_x = i * col_w + 1;
        int end_x = (i + 1) * col_w - 2;

        
        if (i == 0) mvprintw(2, start_x + 2, "Waiting");
        if (i == 1) mvprintw(2, start_x + 2, "Cooking");
        if (i == 2) mvprintw(2, start_x + 2, "Done.");

        
        for (int y = 3; y < rows - 2; ++y) {
            mvaddch(y, start_x, ACS_VLINE);
            mvaddch(y, end_x, ACS_VLINE);
        }
        mvhline(rows - 2, start_x + 1, ACS_HLINE, end_x - start_x - 1);
        mvaddch(rows - 2, start_x, ACS_LLCORNER);  
        mvaddch(rows - 2, end_x, ACS_LRCORNER);    
    }

    
    int y_wait = 4, y_cook = 4, y_done = 4;
    int shown = 0; 

    for (int i = 0; i < count; ++i) {
        Order *o = &orders[i];
        
        
        if (o->status == STATUS_PAID) {
            continue;
        }

        int curr_x = 0;
        int curr_y = 0;

        
        if (o->status == STATUS_WAITING) {       
            curr_x = 0 * col_w + 3;
            curr_y = y_wait++;
        } else if (o->status == STATUS_COOKING) {
            curr_x = 1 * col_w + 3;
            curr_y = y_cook++;
        } else if (o->status == STATUS_DONE) {
            curr_x = 2 * col_w + 3;
            curr_y = y_done++;
        } else {
            continue;
        }

        
        if (curr_y >= rows - 3) continue;

        
        if (shown == sel) {
            attron(A_REVERSE);
        }
        
        
        mvprintw(curr_y, curr_x, "#%04d T%02d", o->order_id, o->table_id);
        
        
        if (o->item_count > 0) {
            mvprintw(curr_y, curr_x + 12, "%.8s", o->items[0].name);
        }

        if (shown == sel) {
            attroff(A_REVERSE);
        }
        
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
            if (proto_parse_order_broadcast(tmp, &ordtmp, er, sizeof(er)) == 0) {
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
                if (proto_parse_order_broadcast(netline, &tmp, er, sizeof(er)) == 0) {
                    ui_merge_order_mirror(board, &bn, &tmp);
                }
            } else if (strncmp(netline, "STAFF_CALL|", 11) == 0) {
                mvprintw(1, 0, "%s", netline);
            } else if (strncmp(netline, "MENU_SYNC", 7) == 0) {
                
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
            
            
            Order *selected_order = NULL;
            for (int i = 0; i < bn; ++i) {
                if (board[i].order_id == oid) {
                    selected_order = &board[i];
                    break;
                }
            }

            if (selected_order != NULL) {
                char buf[128] = {0};

                
                if (ch == KEY_RIGHT) {
                    if (selected_order->status == STATUS_WAITING) { 
                        snprintf(buf, sizeof(buf), "ORDER_UPDATE|order_id=%d|status=COOKING\n", oid);
                    } else if (selected_order->status == STATUS_COOKING) {
                        snprintf(buf, sizeof(buf), "ORDER_UPDATE|order_id=%d|status=DONE\n", oid);
                    }
                }
                
                
                else if (ch == KEY_LEFT) {
                    if (selected_order->status == STATUS_DONE) {
                        snprintf(buf, sizeof(buf), "ORDER_UPDATE|order_id=%d|status=COOKING\n", oid);
                    } else if (selected_order->status == STATUS_COOKING) {
                        snprintf(buf, sizeof(buf), "ORDER_UPDATE|order_id=%d|status=WAITING\n", oid); 
                    }
                }

                
                else if (ch == 'c' || ch == 'C') {
                    
                    if (selected_order->status != STATUS_COOKING) {
                        snprintf(buf, sizeof(buf), "ORDER_UPDATE|order_id=%d|status=COOKING\n", oid);
                    }
                }
                
                
                else if (ch == 'd' || ch == 'D') {
                    if (selected_order->status != STATUS_DONE) {
                        snprintf(buf, sizeof(buf), "ORDER_UPDATE|order_id=%d|status=DONE\n", oid);
                    }
                }

                
                if (buf[0] != '\0') {
                    ui_send_line(sock, buf);
                }
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