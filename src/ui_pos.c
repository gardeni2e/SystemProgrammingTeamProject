#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
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
#include "ui_common.h"

#define MENU_PATH UI_MENU_PATH
#define SALES_LOG_PATH UI_SALES_LOG_PATH

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
        if (o->status == STATUS_CANCELLED || o->status == STATUS_PAID) {
            continue;
        }
        sum->order_count++;
        sum->unpaid_total += o->total_price;
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
            ui_pos_print_in(side, 4, 0, A_NORMAL, "청구금액: ₩%d", sum.unpaid_total);
            ui_pos_print_in(side, 5, 0, A_NORMAL, "조리완료(결제대기): ₩%d",
                            sum.done_total);
            if (staff) {
                ui_pos_print_in(side, 7, 0,
                                A_BOLD | COLOR_PAIR(POS_COLOR_STAFF),
                                "직원 호출: 활성");
            } else {
                ui_pos_print_in(side, 7, 0, A_DIM, "직원 호출: 없음");
            }
            ui_pos_draw_button_in(side, 10, POS_COLOR_ACTIVE, "ENTER  결제화면");
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
             "방향키 이동 · Enter 결제화면 · q 종료");
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
            ui_pos_print_in(side, 3, 0, A_NORMAL, "청구 합계");
            ui_pos_print_in(side, 4, 0, A_BOLD, "₩%d", sum.unpaid_total);
            ui_pos_print_in(side, 6, 0, A_NORMAL, "조리완료(결제대기)");
            ui_pos_print_in(side, 7, 0, A_BOLD, "₩%d", sum.done_total);
        }
        ui_pos_print_in(side, side.h - 5, 0, A_DIM,
                        "Enter: 결제 확인(조리완료만)");
        ui_pos_print_in(side, side.h - 4, 0, A_DIM, "y: 테이블 조리완료 전체 결제");
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
        if (o->status == STATUS_CANCELLED) {
            continue;
        }
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
                        o->order_id, o->table_id, status_to_label(o->status),
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
    mvprintw(footer.top + 1, 1,
             "팁: 조리완료 상태만 결제할 수 있습니다. Enter 두 번으로 확정합니다.");
}

static void ui_draw_pos_pay_confirm(int rows, int cols, int order_id, int total,
                                    int table_id, int batch) {
    int w = 44;
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

    for (int dy = 0; dy < h; ++dy) {
        mvaddch(y + dy, x, ACS_VLINE);
        mvaddch(y + dy, x + w - 1, ACS_VLINE);
        for (int dx = 1; dx < w - 1; ++dx) {
            mvaddch(y + dy, x + dx, ' ');
        }
    }
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    for (int dx = 1; dx < w - 1; ++dx) {
        mvaddch(y, x + dx, ACS_HLINE);
        mvaddch(y + h - 1, x + dx, ACS_HLINE);
    }

    attron(A_BOLD);
    mvprintw(y + 1, x + 2, "결제 확인");
    attroff(A_BOLD);
    mvhline(y + 2, x + 1, ACS_HLINE, w - 2);

    if (batch) {
        mvprintw(y + 4, x + 4, "T%02d 테이블 조리완료 주문을", table_id);
        mvprintw(y + 5, x + 4, "결제하시겠습니까?");
    } else {
        mvprintw(y + 4, x + 4, "주문 #%04d (₩%d)", order_id, total);
        mvprintw(y + 5, x + 4, "결제하시겠습니까?");
    }

    attron(A_BOLD);
    mvprintw(y + 7, x + 6, "ENTER 확정");
    mvprintw(y + 7, x + 24, "ESC/n 취소");
    attroff(A_BOLD);
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
        if (ctx->orders[i].status == STATUS_CANCELLED) {
            continue;
        }
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

typedef struct {
    int sale_count;
    long long total_revenue;
    long long daily_revenue;
} PosSalesSummary;

static void ui_pos_format_sales_log_line(const char *raw, char *out,
                                         size_t outsz) {
    if (!raw || !out || outsz == 0) {
        return;
    }
    out[0] = '\0';

    int id = 0;
    int table = 0;
    int total = 0;
    long long ts = 0;
    if (sscanf(raw, "SALE id=%d table=%d total=%d ts=%lld", &id, &table, &total,
               &ts) < 4) {
        snprintf(out, outsz, "%s", raw);
        return;
    }

    char when[32] = "";
    const char *pt = strstr(raw, "time=");
    if (pt) {
        pt += 5;
        const char *end = strstr(pt, " detail=");
        size_t n = end ? (size_t)(end - pt) : strlen(pt);
        if (n >= sizeof(when)) {
            n = sizeof(when) - 1;
        }
        memcpy(when, pt, n);
        when[n] = '\0';
    }
    if (when[0] == '\0' && ts > 0) {
        struct tm sale_buf;
        time_t sale_ts = (time_t)ts;
        struct tm *sale = localtime_r(&sale_ts, &sale_buf);
        if (sale) {
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", sale);
        }
    }
    if (when[0] == '\0') {
        strncpy(when, "-", sizeof(when) - 1);
        when[sizeof(when) - 1] = '\0';
    }

    snprintf(out, outsz, "[%s]  #%04d  T%02d  ₩%d", when, id, table, total);
}

static void ui_pos_summarize_sales_log(const char *path, PosSalesSummary *sum) {
    memset(sum, 0, sizeof(*sum));

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }

    time_t now = time(NULL);
    struct tm today_buf;
    struct tm *today = localtime_r(&now, &today_buf);

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        int id = 0;
        int table = 0;
        int total = 0;
        long long ts = 0;
        if (sscanf(line, "SALE id=%d table=%d total=%d ts=%lld", &id, &table,
                   &total, &ts) < 4) {
            continue;
        }

        sum->sale_count++;
        sum->total_revenue += total;

        if (today) {
            struct tm sale_buf;
            time_t sale_ts = (time_t)ts;
            struct tm *sale = localtime_r(&sale_ts, &sale_buf);
            if (sale && sale->tm_year == today->tm_year &&
                sale->tm_mon == today->tm_mon &&
                sale->tm_mday == today->tm_mday) {
                sum->daily_revenue += total;
            }
        }
    }
    fclose(fp);
}

static void ui_draw_pos_sales(int rows, int cols) {
    clear();
    UiRect header, main, side, footer;
    ui_pos_layout_frame(rows, cols, POS_TAB_SALES, &header, &main, &side, &footer);
    ui_pos_draw_box(main, "매출 로그 (tail)");
    if (side.w > 0) {
        ui_pos_draw_box(side, "매출 요약");
    }

    PosSalesSummary sum;
    ui_pos_summarize_sales_log(SALES_LOG_PATH, &sum);

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
            if (ln[0] == '\0') {
                ln = strtok_r(NULL, "\n", &save);
                continue;
            }
            if (strncmp(ln, "SALE ", 5) == 0) {
                char formatted[160];
                ui_pos_format_sales_log_line(ln, formatted, sizeof(formatted));
                ui_pos_print_in(main, line, 0, A_NORMAL, "%s", formatted);
            } else {
                ui_pos_print_in(main, line, 0, A_NORMAL, "%s", ln);
            }
            line++;
            ln = strtok_r(NULL, "\n", &save);
        }
    }

    if (side.w > 0) {
        time_t now = time(NULL);
        struct tm today_buf;
        struct tm *today = localtime_r(&now, &today_buf);
        char date_label[32] = "";
        if (today) {
            (void)strftime(date_label, sizeof(date_label), "%Y-%m-%d %H:%M",
                           today);
        }

        ui_pos_print_in(side, 0, 0, A_DIM, "결제완료(SALE) 기준");
        if (date_label[0] != '\0') {
            ui_pos_print_in(side, 1, 0, A_DIM, "오늘: %s", date_label);
        }

        ui_pos_print_in(side, 3, 0, A_NORMAL, "총 매출 건수");
        ui_pos_print_in(side, 4, 0, A_BOLD, "%d건", sum.sale_count);

        ui_pos_print_in(side, 6, 0, A_NORMAL, "총 매출 금액");
        ui_pos_print_in(side, 7, 0, A_BOLD, "₩%lld", sum.total_revenue);

        ui_pos_print_in(side, 9, 0, A_NORMAL, "일 매출 금액");
        ui_pos_print_in(side, 10, 0, A_BOLD, "₩%lld", sum.daily_revenue);
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
    int pay_confirm_oid = 0;
    int pay_confirm_total = 0;
    int pay_confirm_batch_table = 0;
    char pos_banner[128] = "";
    time_t pos_banner_until = 0;

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
            if (pay_confirm_oid > 0 || pay_confirm_batch_table > 0) {
                ui_draw_pos_pay_confirm(rows, cols, pay_confirm_oid,
                                        pay_confirm_total,
                                        pay_confirm_batch_table,
                                        pay_confirm_batch_table > 0);
            }
            if (pos_banner[0] != '\0' && time(NULL) <= pos_banner_until) {
                attron(A_BOLD);
                mvprintw(rows - 1, 1, "%-*s", cols > 2 ? cols - 2 : 0,
                         pos_banner);
                attroff(A_BOLD);
            }
            refresh();
            dirty = 0;
        }

        int ch = wgetch(stdscr);
        if (ch == ERR) {
            continue;
        }

        if (pay_confirm_oid > 0 || pay_confirm_batch_table > 0) {
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                if (pay_confirm_batch_table > 0) {
                    int ids[MAX_ORDERS];
                    int nids = ui_pos_collect_table_done_ids(
                        ctx, pay_confirm_batch_table, ids, MAX_ORDERS);
                    for (int i = 0; i < nids; ++i) {
                        char err[160];
                        server_pay_order(ctx, ids[i], err, sizeof(err));
                    }
                    snprintf(pos_banner, sizeof(pos_banner),
                             "테이블 T%02d 결제 완료 (%d건)",
                             pay_confirm_batch_table, nids);
                } else {
                    char err[160];
                    if (server_pay_order(ctx, pay_confirm_oid, err,
                                         sizeof(err)) == 0) {
                        snprintf(pos_banner, sizeof(pos_banner),
                                 "주문 #%04d 결제 완료", pay_confirm_oid);
                    } else {
                        snprintf(pos_banner, sizeof(pos_banner), "%.127s", err);
                    }
                }
                pay_confirm_oid = 0;
                pay_confirm_total = 0;
                pay_confirm_batch_table = 0;
                pos_banner_until = time(NULL) + 3;
                dirty = 1;
                continue;
            }
            if (ch == 27 || ch == 'n' || ch == 'N') {
                pay_confirm_oid = 0;
                pay_confirm_total = 0;
                pay_confirm_batch_table = 0;
                dirty = 1;
                continue;
            }
            continue;
        }

        if (ch == 'q' || ch == 'Q') {
            ctx->shutting_down = 1;
            break;
        }
        if (ch == KEY_F(1)) {
            tab = POS_TAB_HOME;
            pay_confirm_oid = 0;
            pay_confirm_batch_table = 0;
            dirty = 1;
        }
        if (ch == KEY_F(2)) {
            tab = POS_TAB_PAYMENT;
            pay_confirm_oid = 0;
            pay_confirm_batch_table = 0;
            dirty = 1;
        }
        if (ch == KEY_F(3)) {
            tab = POS_TAB_MENU;
            pay_confirm_oid = 0;
            pay_confirm_batch_table = 0;
            dirty = 1;
        }
        if (ch == KEY_F(4)) {
            tab = POS_TAB_SALES;
            pay_confirm_oid = 0;
            pay_confirm_batch_table = 0;
            dirty = 1;
        }
        if (ch == KEY_F(5)) {
            tab = POS_TAB_LAYOUT;
            pay_confirm_oid = 0;
            pay_confirm_batch_table = 0;
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
            if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && tp) {
                pay_table = tp->table_id;
                tab = POS_TAB_PAYMENT;
                sel_payment = 0;
                pay_confirm_oid = 0;
                pay_confirm_batch_table = 0;
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
        } else if (tab == POS_TAB_PAYMENT) {
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
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                if (sel_payment < nshow) {
                    int oid = 0;
                    int total = 0;
                    OrderStatus st = STATUS_PAID;
                    pthread_mutex_lock(&ctx->lock);
                    oid = ctx->orders[idxs[sel_payment]].order_id;
                    st = ctx->orders[idxs[sel_payment]].status;
                    total = ctx->orders[idxs[sel_payment]].total_price;
                    pthread_mutex_unlock(&ctx->lock);
                    if (st == STATUS_DONE) {
                        pay_confirm_oid = oid;
                        pay_confirm_total = total;
                        pay_confirm_batch_table = 0;
                        dirty = 1;
                    } else if (st == STATUS_PAID) {
                        snprintf(pos_banner, sizeof(pos_banner),
                                 "이미 결제된 주문입니다");
                        pos_banner_until = time(NULL) + 3;
                        dirty = 1;
                    } else {
                        snprintf(pos_banner, sizeof(pos_banner),
                                 "조리완료 후 결제할 수 있습니다 (현재: %s)",
                                 status_to_label(st));
                        pos_banner_until = time(NULL) + 3;
                        dirty = 1;
                    }
                }
            }
            if (ch == 'y' && pay_table > 0) {
                int ids[MAX_ORDERS];
                int nids = ui_pos_collect_table_done_ids(ctx, pay_table, ids,
                                                         MAX_ORDERS);
                if (nids > 0) {
                    pay_confirm_batch_table = pay_table;
                    pay_confirm_oid = 0;
                    pay_confirm_total = 0;
                    dirty = 1;
                } else {
                    snprintf(pos_banner, sizeof(pos_banner),
                             "조리완료된 주문이 없습니다");
                    pos_banner_until = time(NULL) + 3;
                    dirty = 1;
                }
            }
        } else if (tab == POS_TAB_MENU) {
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
