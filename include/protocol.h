#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

#define PROTO_TERM "\n"

int proto_append_esc_name(char *dst, size_t dstsz, const char *name);
int proto_build_menu_response(const MenuCatalog *cat, char *out, size_t outsz);
int proto_parse_menu_csv_segment(const char *segment, MenuItem *out_item);

int proto_parse_order_create(const char *msg, int *table_out, CartItem *cart,
                             int max_cart, int *cart_count_out, char *err,
                             size_t errsz);

int proto_parse_order_update(const char *msg, int *order_id_out,
                             OrderStatus *status_out, char *err, size_t errsz);

int proto_parse_payment_request(const char *msg, int *order_id_out,
                                  char *err, size_t errsz);

int proto_parse_call_staff(const char *msg, int *table_out, char *err,
                           size_t errsz);

int proto_parse_hello_table(const char *msg, int *table_out, char *err,
                            size_t errsz);

void proto_build_order_broadcast(const Order *ord, char *out, size_t outsz);
void proto_build_staff_broadcast(int table_id, char *out, size_t outsz);

int proto_parse_order_broadcast(const char *msg, Order *out_order,
                                char *err, size_t errsz);

#endif
