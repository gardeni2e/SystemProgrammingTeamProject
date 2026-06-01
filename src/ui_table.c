#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ncursesw/ncurses.h>

#include "layout.h"
#include "menu.h"
#include "order.h"
#include "protocol.h"
#include "ui.h"
#include "ui_common.h"

#define MENU_PATH UI_MENU_PATH

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
        if (orders[i].table_id == table_id &&
            orders[i].status != STATUS_CANCELLED) {
            n++;
        }
    }

    return n;
}

static Order *ui_table_get_nth_order(Order *orders, int count, int table_id, int nth) {
    int n = 0;

    for (int i = 0; i < count; ++i) {
        if (orders[i].table_id != table_id) {
            continue;
        }

        if (orders[i].status == STATUS_CANCELLED) {
            continue;
        }

        if (n == nth) {
            return &orders[i];
        }

        n++;
    }

    return NULL;
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

        if (o->status == STATUS_CANCELLED) {
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

        mvprintw(line, box_x + 20, "상태: %s", status_to_label(o->status));

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
             "↑↓ 주문 선택  c 취소(WAITING 상태만 가능)  m 메뉴화면  q 종료");

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

            if (ch == 'c' || ch == 'C') {
                Order *target = ui_table_get_nth_order(mirror, mirror_count, args->table_id, sel);

                if (!target) {
                    snprintf(banner, sizeof(banner), "취소할 주문이 없습니다");
                } else if (target->status != STATUS_WAITING) {
                    snprintf(banner, sizeof(banner), "조리 시작 후에는 주문을 취소할 수 없습니다");
                } else {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "ORDER_CANCEL|order_id=%d\n", target->order_id);
                    ui_send_line(sock, buf);

                    snprintf(banner, sizeof(banner),
                            "주문 #%04d 취소 요청을 보냈습니다",
                            target->order_id);
                }
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
