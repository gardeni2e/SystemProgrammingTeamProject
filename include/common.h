#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <time.h>

#define MAX_MENU_ITEMS 128
#define MAX_ORDER_ITEMS 32
#define MAX_ORDERS 256
#define MAX_CLIENTS 64
#define MAX_PROTO_LINE 16384
#define MAX_NAME 96
#define MAX_CATEGORY 32
#define MAX_TABLES 64
#define DEFAULT_PORT 9090

typedef enum {
    CLIENT_NONE = 0,
    CLIENT_TABLE,
    CLIENT_KITCHEN
} ClientKind;

typedef enum {
    STATUS_WAITING = 0,
    STATUS_COOKING,
    STATUS_DONE,
    STATUS_PAID,
    STATUS_CANCELLED
} OrderStatus;

typedef struct {
    int menu_id;
    char name[MAX_NAME];
    int unit_price;
    int qty;
} OrderLineItem;

typedef struct {
    int order_id;
    int table_id;
    OrderStatus status;
    int item_count;
    OrderLineItem items[MAX_ORDER_ITEMS];
    int total_price;
    time_t created_at;
} Order;

typedef struct {
    int menu_id;
    char name[MAX_NAME];
    int price;
    int qty;
} CartItem;

typedef struct {
    int id;
    char name[MAX_NAME];
    char category[MAX_CATEGORY];
    int price;
    int sold_out;
    int popular;
} MenuItem;

typedef struct {
    MenuItem items[MAX_MENU_ITEMS];
    int count;
} MenuCatalog;

const char *status_to_string(OrderStatus st);
const char *status_to_label(OrderStatus st);
OrderStatus status_from_string(const char *s);

#endif
