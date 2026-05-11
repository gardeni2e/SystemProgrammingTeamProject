#ifndef ORDER_H
#define ORDER_H

#include "common.h"

typedef struct {
    CartItem items[MAX_ORDER_ITEMS];
    int count;
} Cart;

void cart_init(Cart *cart);
int cart_add(Cart *cart, const CartItem *line, char *err, size_t errsz);
int cart_remove_line(Cart *cart, int menu_id);
int cart_set_qty(Cart *cart, int menu_id, int qty, char *err, size_t errsz);
int cart_total(const Cart *cart);

int order_build_from_cart(Order *out, int order_id, int table_id,
                          const Cart *cart, char *err, size_t errsz);

void orders_reset(Order *orders, int *count);
int orders_find_index(const Order *orders, int count, int order_id);
Order *orders_find(Order *orders, int count, int order_id);

#endif
