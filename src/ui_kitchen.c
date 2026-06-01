#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#include <ncursesw/ncurses.h>

#include "order.h"
#include "protocol.h"
#include "ui.h"
#include "ui_common.h"

static void ui_draw_kitchen(Order *orders, int count, int sel, int rows, int cols) {
    clear();
    
    mvprintw(0, 0, "Kitchen Display | ↑↓ 선택 · c 조리 · d 완료");

    int col_w = cols / 3;

    
    for (int i = 0; i < 3; ++i) {
        int start_x = i * col_w + 1;
        int end_x = (i + 1) * col_w - 2;

        
        if (i == 0) mvprintw(2, start_x + 2, "Waiting");
        if (i == 1) mvprintw(2, start_x + 2, "Cooking");
        if (i == 2) mvprintw(2, start_x + 2, "조리완료");

        
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
        if (o->status == STATUS_PAID || o->status == STATUS_CANCELLED) {
            continue;
        }
        
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
        if (orders[i].status != STATUS_PAID && orders[i].status != STATUS_CANCELLED) {
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