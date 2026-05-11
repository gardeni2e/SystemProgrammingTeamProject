#include <stdio.h>
#include <string.h>
#include <time.h>

#include "menu.h"
#include "order.h"

void cart_init(Cart *cart) {
    memset(cart, 0, sizeof(*cart));
}

int cart_add(Cart *cart, const CartItem *line, char *err, size_t errsz) {
    if (line->qty <= 0) {
        snprintf(err, errsz, "bad qty");
        return -1;
    }
    for (int i = 0; i < cart->count; ++i) {
        if (cart->items[i].menu_id == line->menu_id) {
            cart->items[i].qty += line->qty;
            return 0;
        }
    }
    if (cart->count >= MAX_ORDER_ITEMS) {
        snprintf(err, errsz, "cart full");
        return -1;
    }
    cart->items[cart->count++] = *line;
    return 0;
}

int cart_remove_line(Cart *cart, int menu_id) {
    for (int i = 0; i < cart->count; ++i) {
        if (cart->items[i].menu_id == menu_id) {
            for (int j = i; j < cart->count - 1; ++j) {
                cart->items[j] = cart->items[j + 1];
            }
            cart->count--;
            return 0;
        }
    }
    return -1;
}

int cart_set_qty(Cart *cart, int menu_id, int qty, char *err, size_t errsz) {
    if (qty <= 0) {
        return cart_remove_line(cart, menu_id);
    }
    for (int i = 0; i < cart->count; ++i) {
        if (cart->items[i].menu_id == menu_id) {
            cart->items[i].qty = qty;
            return 0;
        }
    }
    snprintf(err, errsz, "line not in cart");
    return -1;
}

int cart_total(const Cart *cart) {
    int t = 0;
    for (int i = 0; i < cart->count; ++i) {
        t += cart->items[i].price * cart->items[i].qty;
    }
    return t;
}

int order_build_from_cart(Order *out, int order_id, int table_id,
                          const Cart *cart, char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    out->order_id = order_id;
    out->table_id = table_id;
    out->status = STATUS_WAITING;
    out->created_at = time(NULL);
    out->item_count = 0;
    out->total_price = 0;
    if (cart->count <= 0) {
        snprintf(err, errsz, "empty cart");
        return -1;
    }
    for (int i = 0; i < cart->count; ++i) {
        if (out->item_count >= MAX_ORDER_ITEMS) {
            snprintf(err, errsz, "too many lines");
            return -1;
        }
        OrderLineItem *li = &out->items[out->item_count];
        li->menu_id = cart->items[i].menu_id;
        li->qty = cart->items[i].qty;
        strncpy(li->name, cart->items[i].name, sizeof(li->name) - 1);
        li->name[sizeof(li->name) - 1] = '\0';
        li->unit_price = cart->items[i].price;
        out->total_price += li->unit_price * li->qty;
        out->item_count++;
    }
    return 0;
}

void orders_reset(Order *orders, int *count) {
    memset(orders, 0, sizeof(Order) * (size_t)(*count));
    *count = 0;
}

int orders_find_index(const Order *orders, int count, int order_id) {
    for (int i = 0; i < count; ++i) {
        if (orders[i].order_id == order_id) {
            return i;
        }
    }
    return -1;
}

Order *orders_find(Order *orders, int count, int order_id) {
    int idx = orders_find_index(orders, count, order_id);
    if (idx < 0) {
        return NULL;
    }
    return &orders[idx];
}
