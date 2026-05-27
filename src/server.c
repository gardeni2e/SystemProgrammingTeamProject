#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "menu.h"
#include "order.h"
#include "protocol.h"
#include "server.h"
#include "storage.h"

#define MENU_PATH "data/menu.csv"
#define CONFIG_PATH "data/tables.conf"
#define ORDERS_LOG_PATH "data/orders.log"
#define SALES_LOG_PATH "data/sales.log"
#define SERVER_LOG_PATH "data/server.log"

typedef struct {
    ServerContext *ctx;
    int client_fd;
} ClientThreadArg;

typedef struct {
    int fd;
    unsigned char buf[8192];
    size_t len;
} SockReader;

static ServerContext *g_sig_ctx;

static void strip_crnl(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int write_all(int fd, const void *data, size_t len) {
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

static int sock_read_line(SockReader *sr, char *out, size_t outsz) {
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
                memmove(sr->buf, sr->buf + take, sr->len - take);
                sr->len -= take;
                strip_crnl(out);
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

static void sigint_handler(int signo) {
    (void)signo;
    if (!g_sig_ctx) {
        return;
    }
    g_sig_ctx->shutting_down = 1;
    int fd = g_sig_ctx->listen_fd;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        g_sig_ctx->listen_fd = -1;
    }
}

void server_install_sigint_handler(ServerContext *ctx) {
    g_sig_ctx = ctx;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

static void append_orders_meta(ServerContext *ctx, const char *text) {
    char err[128];
    if (storage_append_orders_log(ORDERS_LOG_PATH, text, err, sizeof(err)) !=
        0) {
        storage_append_server_log(SERVER_LOG_PATH,
                                  "orders.log write failed\n");
    }
    (void)ctx;
}

static void append_sales_meta(ServerContext *ctx, const char *text) {
    char err[128];
    if (storage_append_sales_log(SALES_LOG_PATH, text, err, sizeof(err)) !=
        0) {
        storage_append_server_log(SERVER_LOG_PATH,
                                  "sales.log write failed\n");
    }
    (void)ctx;
}

static int finalize_cart_lines(ServerContext *ctx, CartItem *lines, int n,
                               char *err, size_t errsz) {
    for (int i = 0; i < n; ++i) {
        const MenuItem *mi = menu_find_by_id(&ctx->menu, lines[i].menu_id);
        if (!mi) {
            snprintf(err, errsz, "unknown menu id %d", lines[i].menu_id);
            return -1;
        }
        if (mi->sold_out) {
            snprintf(err, errsz, "sold out: %s", mi->name);
            return -1;
        }
        strncpy(lines[i].name, mi->name, sizeof(lines[i].name) - 1);
        lines[i].name[sizeof(lines[i].name) - 1] = '\0';
        lines[i].price = mi->price;
    }
    return 0;
}

void server_touch_orders(ServerContext *ctx) {
    ctx->orders_revision++;
}

static void broadcast_raw(ServerContext *ctx, const char *msg, size_t len) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!ctx->clients[i].in_use) {
            continue;
        }
        if (write_all(ctx->clients[i].fd, msg, len) != 0) {
            continue;
        }
    }
}

void server_broadcast_line(ServerContext *ctx, const char *line) {
    char buf[MAX_PROTO_LINE + 4];
    size_t len = strlen(line);
    if (len + 2 > sizeof(buf)) {
        return;
    }
    memcpy(buf, line, len);
    if (len == 0 || buf[len - 1] != '\n') {
        buf[len++] = '\n';
    }
    buf[len] = '\0';
    pthread_mutex_lock(&ctx->lock);
    broadcast_raw(ctx, buf, len);
    pthread_mutex_unlock(&ctx->lock);
}

static void send_snapshot_locked(ServerContext *ctx, int fd, ClientKind kind,
                                 int table_id) {
    for (int i = 0; i < ctx->order_count; ++i) {
        Order *o = &ctx->orders[i];
        if (o->status == STATUS_PAID) {
            continue;
        }
        if (kind == CLIENT_TABLE && o->table_id != table_id) {
            continue;
        }
        char evt[MAX_PROTO_LINE];
        proto_build_order_broadcast(o, evt, sizeof(evt));
        size_t el = strlen(evt);
        evt[el++] = '\n';
        evt[el] = '\0';
        write_all(fd, evt, el);
    }
}

void server_register_client(ServerContext *ctx, int fd, ClientKind kind,
                            int table_id) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!ctx->clients[i].in_use) {
            ctx->clients[i].fd = fd;
            ctx->clients[i].in_use = 1;
            ctx->clients[i].kind = kind;
            ctx->clients[i].table_id = table_id;
            return;
        }
    }
}

void server_unregister_fd(ServerContext *ctx, int fd) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (ctx->clients[i].in_use && ctx->clients[i].fd == fd) {
            ctx->clients[i].in_use = 0;
            ctx->clients[i].fd = -1;
            ctx->clients[i].kind = CLIENT_NONE;
            ctx->clients[i].table_id = 0;
            break;
        }
    }
}

int server_init(ServerContext *ctx, uint16_t port, char *err, size_t errsz) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->cfg.max_tables = 8;
    ctx->cfg.next_order_id = 1;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        snprintf(err, errsz, "socket: %s", strerror(errno));
        pthread_mutex_destroy(&ctx->lock);
        return -1;
    }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(err, errsz, "bind: %s", strerror(errno));
        close(ls);
        pthread_mutex_destroy(&ctx->lock);
        return -1;
    }
    if (listen(ls, 32) != 0) {
        snprintf(err, errsz, "listen: %s", strerror(errno));
        close(ls);
        pthread_mutex_destroy(&ctx->lock);
        return -1;
    }
    ctx->listen_fd = ls;
    return 0;
}

void server_shutdown(ServerContext *ctx) {
    ctx->shutting_down = 1;
    if (ctx->listen_fd >= 0) {
        shutdown(ctx->listen_fd, SHUT_RDWR);
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->accept_thread_started) {
        pthread_join(ctx->accept_thread, NULL);
        ctx->accept_thread_started = 0;
    }

    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (ctx->clients[i].in_use && ctx->clients[i].fd >= 0) {
            shutdown(ctx->clients[i].fd, SHUT_RDWR);
            close(ctx->clients[i].fd);
            ctx->clients[i].in_use = 0;
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    char e2[128];
    storage_save_config(CONFIG_PATH, &ctx->cfg, e2, sizeof(e2));
    menu_save_file(&ctx->menu, MENU_PATH, e2, sizeof(e2));

    pthread_mutex_destroy(&ctx->lock);
}

int server_load_persistent(ServerContext *ctx, char *err, size_t errsz) {
    if (storage_load_config(CONFIG_PATH, &ctx->cfg, err, errsz) != 0) {
        return -1;
    }
    if (menu_load_file(&ctx->menu, MENU_PATH, err, errsz) != 0) {
        return -1;
    }
    ctx->order_count = 0;
    return 0;
}

int server_save_config(ServerContext *ctx, char *err, size_t errsz) {
    return storage_save_config(CONFIG_PATH, &ctx->cfg, err, errsz);
}

int server_find_order_index(ServerContext *ctx, int order_id) {
    return orders_find_index(ctx->orders, ctx->order_count, order_id);
}

Order *server_get_order(ServerContext *ctx, int order_id) {
    return orders_find(ctx->orders, ctx->order_count, order_id);
}

static int push_order_locked(ServerContext *ctx, const Order *o) {
    if (ctx->order_count >= MAX_ORDERS) {
        return -1;
    }
    ctx->orders[ctx->order_count++] = *o;
    return 0;
}

int server_create_order(ServerContext *ctx, int table_id, const Cart *cart,
                        char *err, size_t errsz) {
    pthread_mutex_lock(&ctx->lock);
    if (table_id <= 0 || table_id > ctx->cfg.max_tables) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "bad table range");
        return -1;
    }
    Order ord;
    if (order_build_from_cart(&ord, ctx->cfg.next_order_id, table_id, cart, err,
                              errsz) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }
    if (push_order_locked(ctx, &ord) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "order capacity");
        return -1;
    }
    ctx->cfg.next_order_id++;
    char logln[MAX_PROTO_LINE];
    snprintf(logln, sizeof(logln),
             "ORDER_CREATED id=%d table=%d total=%d ts=%lld\n", ord.order_id,
             ord.table_id, ord.total_price, (long long)time(NULL));
    append_orders_meta(ctx, logln);
    server_save_config(ctx, err, errsz);

    char evt[MAX_PROTO_LINE];
    proto_build_order_broadcast(&ord, evt, sizeof(evt));
    size_t el = strlen(evt);
    evt[el++] = '\n';
    evt[el] = '\0';
    broadcast_raw(ctx, evt, el);

    server_touch_orders(ctx);
    pthread_mutex_unlock(&ctx->lock);
    return ord.order_id;
}

static int valid_kitchen_transition(OrderStatus cur, OrderStatus nw) {
    if (cur == STATUS_WAITING && nw == STATUS_COOKING) {
        return 1;
    }
    if (cur == STATUS_COOKING && nw == STATUS_DONE) {
        return 1;
    }
    if (cur == STATUS_WAITING && nw == STATUS_DONE) {
        return 1;
    }
    if (cur == STATUS_DONE && nw == STATUS_COOKING) {
        return 1;
    }
    if (cur == STATUS_COOKING && nw == STATUS_WAITING) {
        return 1;
    }

    return 0;
}

int server_update_order_status(ServerContext *ctx, int order_id,
                               OrderStatus new_status, char *err, size_t errsz) {
    pthread_mutex_lock(&ctx->lock);
    Order *o = server_get_order(ctx, order_id);
    if (!o) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "order not found");
        return -1;
    }
    if (o->status == STATUS_PAID) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "already paid");
        return -1;
    }
    if (!valid_kitchen_transition(o->status, new_status)) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "invalid transition");
        return -1;
    }
    o->status = new_status;
    char logln[256];
    snprintf(logln, sizeof(logln),
             "ORDER_STATUS id=%d status=%s ts=%lld\n", order_id,
             status_to_string(new_status), (long long)time(NULL));
    append_orders_meta(ctx, logln);

    char evt[MAX_PROTO_LINE];
    proto_build_order_broadcast(o, evt, sizeof(evt));
    size_t el = strlen(evt);
    evt[el++] = '\n';
    evt[el] = '\0';
    broadcast_raw(ctx, evt, el);

    server_touch_orders(ctx);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

int server_pay_order(ServerContext *ctx, int order_id, char *err,
                     size_t errsz) {
    pthread_mutex_lock(&ctx->lock);
    Order *o = server_get_order(ctx, order_id);
    if (!o) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "order not found");
        return -1;
    }
    if (o->status != STATUS_DONE) {
        pthread_mutex_unlock(&ctx->lock);
        snprintf(err, errsz, "not DONE");
        return -1;
    }
    o->status = STATUS_PAID;
    char slog[MAX_PROTO_LINE];
    snprintf(slog, sizeof(slog),
             "SALE id=%d table=%d total=%d ts=%lld detail=", order_id,
             o->table_id, o->total_price, (long long)time(NULL));
    size_t sl = strlen(slog);
    for (int i = 0; i < o->item_count && sl + 40 < sizeof(slog); ++i) {
        int n =
            snprintf(slog + sl, sizeof(slog) - sl, "%s%d:%d",
                     i ? "," : "", o->items[i].menu_id, o->items[i].qty);
        if (n > 0) {
            sl += (size_t)n;
        }
    }
    strncat(slog, "\n", sizeof(slog) - strlen(slog) - 1);
    append_sales_meta(ctx, slog);

    char evt[MAX_PROTO_LINE];
    proto_build_order_broadcast(o, evt, sizeof(evt));
    size_t el = strlen(evt);
    evt[el++] = '\n';
    evt[el] = '\0';
    broadcast_raw(ctx, evt, el);

    server_touch_orders(ctx);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static void handle_menu_request(ServerContext *ctx, int fd) {
    char resp[MAX_PROTO_LINE];
    pthread_mutex_lock(&ctx->lock);
    if (proto_build_menu_response(&ctx->menu, resp, sizeof(resp)) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        write_all(fd, "ERROR|menu encode\n", 18);
        return;
    }
    pthread_mutex_unlock(&ctx->lock);
    strncat(resp, "\n", sizeof(resp) - strlen(resp) - 1);
    write_all(fd, resp, strlen(resp));
}

static void handle_order_create_client(ServerContext *ctx, int fd,
                                       const char *line, int table_id) {
    int tbl = 0;
    CartItem lines[MAX_ORDER_ITEMS];
    int nlines = 0;
    char err[160];
    if (proto_parse_order_create(line, &tbl, lines, MAX_ORDER_ITEMS, &nlines,
                                 err, sizeof(err)) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "ERROR|%s\n", err);
        write_all(fd, buf, strlen(buf));
        return;
    }
    if (tbl != table_id) {
        write_all(fd, "ERROR|table mismatch\n", 21);
        return;
    }
    pthread_mutex_lock(&ctx->lock);
    if (finalize_cart_lines(ctx, lines, nlines, err, sizeof(err)) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        char buf[256];
        snprintf(buf, sizeof(buf), "ERROR|%s\n", err);
        write_all(fd, buf, strlen(buf));
        return;
    }
    Cart cart;
    cart_init(&cart);
    for (int i = 0; i < nlines; ++i) {
        if (cart_add(&cart, &lines[i], err, sizeof(err)) != 0) {
            pthread_mutex_unlock(&ctx->lock);
            char buf[256];
            snprintf(buf, sizeof(buf), "ERROR|%s\n", err);
            write_all(fd, buf, strlen(buf));
            return;
        }
    }
    Order ord;
    if (order_build_from_cart(&ord, ctx->cfg.next_order_id, tbl, &cart, err,
                              sizeof(err)) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        char buf[256];
        snprintf(buf, sizeof(buf), "ERROR|%s\n", err);
        write_all(fd, buf, strlen(buf));
        return;
    }
    if (push_order_locked(ctx, &ord) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        write_all(fd, "ERROR|capacity\n", 15);
        return;
    }
    ctx->cfg.next_order_id++;
    char logln[MAX_PROTO_LINE];
    snprintf(logln, sizeof(logln),
             "ORDER_CREATED id=%d table=%d total=%d ts=%lld\n", ord.order_id,
             ord.table_id, ord.total_price, (long long)time(NULL));
    append_orders_meta(ctx, logln);
    server_save_config(ctx, err, sizeof(err));

    char evt[MAX_PROTO_LINE];
    proto_build_order_broadcast(&ord, evt, sizeof(evt));
    size_t el = strlen(evt);
    evt[el++] = '\n';
    evt[el] = '\0';
    broadcast_raw(ctx, evt, el);
    server_touch_orders(ctx);
    pthread_mutex_unlock(&ctx->lock);

    char reply[128];
    snprintf(reply, sizeof(reply),
             "ORDER_CREATED|order_id=%d|total=%d|status=%s\n", ord.order_id,
             ord.total_price, status_to_string(ord.status));
    write_all(fd, reply, strlen(reply));
}

static void handle_order_update_client(ServerContext *ctx, const char *line) {
    int oid = 0;
    OrderStatus st = STATUS_WAITING;
    char err[160];
    if (proto_parse_order_update(line, &oid, &st, err, sizeof(err)) != 0) {
        return;
    }
    server_update_order_status(ctx, oid, st, err, sizeof(err));
}

static void handle_call_staff(ServerContext *ctx, const char *line) {
    int tbl = 0;
    char err[160];
    if (proto_parse_call_staff(line, &tbl, err, sizeof(err)) != 0) {
        return;
    }
    pthread_mutex_lock(&ctx->lock);
    snprintf(ctx->last_staff_message, sizeof(ctx->last_staff_message),
             "직원 호출: 테이블 %d", tbl);
    ctx->last_staff_time = time(NULL);
    if (tbl > 0 && tbl <= MAX_TABLES) {
        ctx->staff_call_at[tbl] = ctx->last_staff_time;
    }
    pthread_mutex_unlock(&ctx->lock);

    char evt[MAX_PROTO_LINE];
    proto_build_staff_broadcast(tbl, evt, sizeof(evt));
    strncat(evt, "\n", sizeof(evt) - strlen(evt) - 1);
    server_broadcast_line(ctx, evt);

    char slog[128];
    snprintf(slog, sizeof(slog), "STAFF_CALL table=%d ts=%lld\n", tbl,
             (long long)time(NULL));
    append_orders_meta(ctx, slog);
}

static void *client_thread_main(void *arg) {
    ClientThreadArg *cta = (ClientThreadArg *)arg;
    ServerContext *ctx = cta->ctx;
    int fd = cta->client_fd;
    free(cta);

    SockReader sr;
    memset(&sr, 0, sizeof(sr));
    sr.fd = fd;

    char line[MAX_PROTO_LINE];
    if (sock_read_line(&sr, line, sizeof(line)) <= 0) {
        close(fd);
        return NULL;
    }

    ClientKind kind = CLIENT_NONE;
    int table_id = 0;
    char err[128];

    if (strncmp(line, "HELLO TABLE", 11) == 0) {
        if (proto_parse_hello_table(line, &table_id, err, sizeof(err)) != 0) {
            write_all(fd, "ERROR|bad hello table\n", 22);
            close(fd);
            return NULL;
        }
        kind = CLIENT_TABLE;
    } else if (strcmp(line, "HELLO KITCHEN") == 0) {
        kind = CLIENT_KITCHEN;
    } else {
        write_all(fd, "ERROR|handshake required\n", 23);
        close(fd);
        return NULL;
    }

    if (write_all(fd, "OK|hello\n", strlen("OK|hello\n")) != 0) { 
        pthread_mutex_lock(&ctx->lock);
        server_unregister_fd(ctx, fd);
        pthread_mutex_unlock(&ctx->lock);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        return NULL;
    }

    pthread_mutex_lock(&ctx->lock);
    server_register_client(ctx, fd, kind, table_id);
    send_snapshot_locked(ctx, fd, kind, table_id);
    pthread_mutex_unlock(&ctx->lock);

    while (!ctx->shutting_down &&
           sock_read_line(&sr, line, sizeof(line)) > 0) {
        if (strcmp(line, "MENU_REQUEST") == 0) {
            handle_menu_request(ctx, fd);
        } else if (strncmp(line, "ORDER_CREATE|", 13) == 0) {
            if (kind != CLIENT_TABLE) {
                write_all(fd, "ERROR|forbidden\n", 16);
                continue;
            }
            handle_order_create_client(ctx, fd, line, table_id);
        } else if (strncmp(line, "ORDER_UPDATE|", 13) == 0) {
            if (kind != CLIENT_KITCHEN) {
                write_all(fd, "ERROR|forbidden\n", 16);
                continue;
            }
            handle_order_update_client(ctx, line);
        } else if (strncmp(line, "CALL_STAFF|", 11) == 0) {
            if (kind != CLIENT_TABLE) {
                write_all(fd, "ERROR|forbidden\n", 16);
                continue;
            }
            handle_call_staff(ctx, line);
            write_all(fd, "OK|staff\n", 9);
        } else if (strcmp(line, "PING") == 0) {
            write_all(fd, "PONG\n", 5);
        } else {
            write_all(fd, "ERROR|unknown\n", 14);
        }
    }

    pthread_mutex_lock(&ctx->lock);
    server_unregister_fd(ctx, fd);
    pthread_mutex_unlock(&ctx->lock);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

void *server_accept_loop(void *arg) {
    ServerContext *ctx = arg;
    while (!ctx->shutting_down) {
        int cfd = accept(ctx->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        ClientThreadArg *cta = malloc(sizeof(ClientThreadArg));
        if (!cta) {
            close(cfd);
            continue;
        }
        cta->ctx = ctx;
        cta->client_fd = cfd;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, client_thread_main, cta);
        pthread_attr_destroy(&attr);
    }
    return NULL;
}
