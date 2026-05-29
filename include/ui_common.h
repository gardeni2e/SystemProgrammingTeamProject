#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#include "common.h"
#include "menu.h"
#include "order.h"

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

#define UI_MENU_PATH "data/menu.csv"
#define UI_CONFIG_PATH "data/tables.conf"
#define UI_SALES_LOG_PATH "data/sales.log"

void ui_locale_utf8(void);
int ui_write_all(int fd, const void *data, size_t len);
int ui_sock_read_line(UiSockReader *sr, char *out, size_t outsz);
void ui_net_enqueue(UiNetQueue *nq, const char *line);
int ui_net_pop(UiNetQueue *nq, char *out, size_t outsz);
void *ui_net_reader_main(void *arg);
int ui_tcp_connect(const char *host, uint16_t port, char *err, size_t errsz);
void ui_apply_menu_response(MenuCatalog *cat, const char *line);
void ui_merge_order_mirror(Order *orders, int *count, const Order *incoming);
void ui_send_line(int sock, const char *s);

#endif
