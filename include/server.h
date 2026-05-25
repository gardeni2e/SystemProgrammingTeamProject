#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

#include "common.h"
#include "order.h"
#include "storage.h"

typedef struct {
    int fd;
    int in_use;
    ClientKind kind;
    int table_id;
} ClientSlot;

typedef struct ServerContext {
    MenuCatalog menu;
    Order orders[MAX_ORDERS];
    int order_count;
    StoreConfig cfg;
    int listen_fd;
    volatile sig_atomic_t shutting_down;
    volatile sig_atomic_t orders_revision;
    pthread_mutex_t lock;
    ClientSlot clients[MAX_CLIENTS];
    pthread_t accept_thread;
    int accept_thread_started;
    char last_staff_message[160];
    time_t last_staff_time;
    time_t staff_call_at[MAX_TABLES + 1];
} ServerContext;

void server_install_sigint_handler(ServerContext *ctx);

int server_init(ServerContext *ctx, uint16_t port, char *err, size_t errsz);
void server_shutdown(ServerContext *ctx);

int server_load_persistent(ServerContext *ctx, char *err, size_t errsz);
int server_save_config(ServerContext *ctx, char *err, size_t errsz);

void server_touch_orders(ServerContext *ctx);

int server_find_order_index(ServerContext *ctx, int order_id);
Order *server_get_order(ServerContext *ctx, int order_id);

int server_create_order(ServerContext *ctx, int table_id, const Cart *cart,
                        char *err, size_t errsz);
int server_update_order_status(ServerContext *ctx, int order_id,
                               OrderStatus new_status, char *err, size_t errsz);
int server_pay_order(ServerContext *ctx, int order_id, char *err, size_t errsz);

void server_broadcast_line(ServerContext *ctx, const char *line);
void server_register_client(ServerContext *ctx, int fd, ClientKind kind,
                            int table_id);
void server_unregister_fd(ServerContext *ctx, int fd);

void *server_accept_loop(void *arg);

#endif
