#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static void sanitize_field(char *s) {
    for (; *s; ++s) {
        if (*s == ',' || *s == ';' || *s == '|' || *s == ':' ||
            *s == '\n' || *s == '\r') {
            *s = '_';
        }
    }
}

int proto_append_esc_name(char *dst, size_t dstsz, const char *name) {
    size_t pos = strnlen(dst, dstsz);
    char tmp[MAX_NAME];
    strncpy(tmp, name, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    sanitize_field(tmp);
    int n = snprintf(dst + pos, dstsz > pos ? dstsz - pos : 0, "%s", tmp);
    return n >= 0 ? 0 : -1;
}

int proto_build_menu_response(const MenuCatalog *cat, char *out, size_t outsz) {
    size_t pos = 0;
    int w =
        snprintf(out + pos, outsz > pos ? outsz - pos : 0, "MENU_RESPONSE|");
    if (w < 0) {
        return -1;
    }
    pos += (size_t)w;
    for (int i = 0; i < cat->count; ++i) {
        char name[MAX_NAME];
        char category[MAX_CATEGORY];

        strncpy(name, cat->items[i].name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        sanitize_field(name);

        strncpy(category,
                cat->items[i].category[0] ? cat->items[i].category : "기타",
                sizeof(category) - 1);
        category[sizeof(category) - 1] = '\0';
        sanitize_field(category);

        w = snprintf(out + pos, outsz > pos ? outsz - pos : 0,
                    "%s%d,%s,%d,%d,%s,%d",
                    i > 0 ? ";" : "",
                    cat->items[i].id,
                    name,
                    cat->items[i].price,
                    cat->items[i].sold_out ? 1 : 0,
                    category,
                    cat->items[i].popular ? 1 : 0);

        if (w < 0 || (size_t)w >= outsz - pos) {
            return -1;
        }

        pos += (size_t)w;
    }
    return 0;
}

int proto_parse_menu_csv_segment(const char *segment, MenuItem *out_item) {
    char buf[MAX_NAME + MAX_CATEGORY + 128];

    strncpy(buf, segment, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *idtok = strtok_r(buf, ",", &save);
    char *nametok = strtok_r(NULL, ",", &save);
    char *pricetok = strtok_r(NULL, ",", &save);
    char *sotok = strtok_r(NULL, ",", &save);
    char *categorytok = strtok_r(NULL, ",", &save);
    char *populartok = strtok_r(NULL, ",", &save);

    if (!idtok || !nametok || !pricetok || !sotok) {
        return -1;
    }

    memset(out_item, 0, sizeof(*out_item));

    out_item->id = atoi(idtok);

    strncpy(out_item->name, nametok, sizeof(out_item->name) - 1);
    out_item->name[sizeof(out_item->name) - 1] = '\0';

    out_item->price = atoi(pricetok);
    out_item->sold_out = atoi(sotok) ? 1 : 0;

    if (categorytok && categorytok[0] != '\0') {
        strncpy(out_item->category, categorytok,
                sizeof(out_item->category) - 1);
        out_item->category[sizeof(out_item->category) - 1] = '\0';
    } else {
        strncpy(out_item->category, "기타",
                sizeof(out_item->category) - 1);
        out_item->category[sizeof(out_item->category) - 1] = '\0';
    }

    out_item->popular = populartok ? (atoi(populartok) ? 1 : 0) : 0;

    return 0;
}

static void copy_err(char *err, size_t errsz, const char *msg) {
    if (!err || errsz == 0) {
        return;
    }
    strncpy(err, msg, errsz - 1);
    err[errsz - 1] = '\0';
}

int proto_parse_order_create(const char *msg, int *table_out, CartItem *cart,
                             int max_cart, int *cart_count_out, char *err,
                             size_t errsz) {
    const char *p = strstr(msg, "ORDER_CREATE|");
    if (!p) {
        copy_err(err, errsz, "not ORDER_CREATE");
        return -1;
    }
    p += strlen("ORDER_CREATE|");
    int table_id = -1;
    char items_part[MAX_PROTO_LINE];
    items_part[0] = '\0';

    char buf[MAX_PROTO_LINE];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);
    while (tok) {
        if (strncmp(tok, "table=", 6) == 0) {
            table_id = atoi(tok + 6);
        } else if (strncmp(tok, "items=", 6) == 0) {
            strncpy(items_part, tok + 6, sizeof(items_part) - 1);
            items_part[sizeof(items_part) - 1] = '\0';
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }
    if (table_id <= 0) {
        copy_err(err, errsz, "bad table");
        return -1;
    }
    if (items_part[0] == '\0') {
        copy_err(err, errsz, "missing items");
        return -1;
    }

    int ncart = 0;
    char ip[MAX_PROTO_LINE];
    strncpy(ip, items_part, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';
    char *save2 = NULL;
    char *pair = strtok_r(ip, ",", &save2);
    while (pair && ncart < max_cart) {
        char *colon = strchr(pair, ':');
        if (!colon) {
            copy_err(err, errsz, "bad item pair");
            return -1;
        }
        *colon = '\0';
        int mid = atoi(pair);
        int qty = atoi(colon + 1);
        if (mid <= 0 || qty <= 0) {
            copy_err(err, errsz, "bad menu/qty");
            return -1;
        }
        cart[ncart].menu_id = mid;
        cart[ncart].qty = qty;
        cart[ncart].name[0] = '\0';
        cart[ncart].price = 0;
        ++ncart;
        pair = strtok_r(NULL, ",", &save2);
    }
    *table_out = table_id;
    *cart_count_out = ncart;
    return 0;
}

int proto_parse_order_update(const char *msg, int *order_id_out,
                             OrderStatus *status_out, char *err, size_t errsz) {
    const char *p = strstr(msg, "ORDER_UPDATE|");
    if (!p) {
        copy_err(err, errsz, "not ORDER_UPDATE");
        return -1;
    }
    p += strlen("ORDER_UPDATE|");
    char buf[MAX_PROTO_LINE];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int oid = -1;
    OrderStatus st = STATUS_WAITING;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);
    while (tok) {
        if (strncmp(tok, "order_id=", 9) == 0) {
            oid = atoi(tok + 9);
        } else if (strncmp(tok, "status=", 7) == 0) {
            st = status_from_string(tok + 7);
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }
    if (oid <= 0) {
        copy_err(err, errsz, "bad order_id");
        return -1;
    }
    *order_id_out = oid;
    *status_out = st;
    return 0;
}

int proto_parse_order_cancel(const char *msg, int *order_id_out, char *err, size_t errsz) {
    const char *p = strstr(msg, "ORDER_CANCEL|");
    if (!p) {
        copy_err(err, errsz, "not ORDER_CANCEL");
        return -1;
    }

    p += strlen("ORDER_CANCEL|");

    char buf[256];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int oid = -1;

    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);

    while (tok) {
        if (strncmp(tok, "order_id=", 9) == 0) {
            oid = atoi(tok + 9);
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }

    if (oid <= 0) {
        copy_err(err, errsz, "bad order_id");
        return -1;
    }

    *order_id_out = oid;
    return 0;
}

int proto_parse_payment_request(const char *msg, int *order_id_out,
                                char *err, size_t errsz) {
    const char *p = strstr(msg, "PAYMENT_REQUEST|");
    if (!p) {
        copy_err(err, errsz, "not PAYMENT_REQUEST");
        return -1;
    }
    p += strlen("PAYMENT_REQUEST|");
    char buf[256];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int oid = -1;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);
    while (tok) {
        if (strncmp(tok, "order_id=", 9) == 0) {
            oid = atoi(tok + 9);
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }
    if (oid <= 0) {
        copy_err(err, errsz, "bad order_id");
        return -1;
    }
    *order_id_out = oid;
    return 0;
}

int proto_parse_call_staff(const char *msg, int *table_out, char *err,
                           size_t errsz) {
    const char *p = strstr(msg, "CALL_STAFF|");
    if (!p) {
        copy_err(err, errsz, "not CALL_STAFF");
        return -1;
    }
    p += strlen("CALL_STAFF|");
    char buf[256];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int tid = -1;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);
    while (tok) {
        if (strncmp(tok, "table=", 6) == 0) {
            tid = atoi(tok + 6);
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }
    if (tid <= 0) {
        copy_err(err, errsz, "bad table");
        return -1;
    }
    *table_out = tid;
    return 0;
}

int proto_parse_hello_table(const char *msg, int *table_out, char *err,
                            size_t errsz) {
    if (strncmp(msg, "HELLO TABLE", 11) != 0) {
        copy_err(err, errsz, "expected HELLO TABLE");
        return -1;
    }
    const char *p = msg + 11;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    int tid = atoi(p);
    if (tid <= 0) {
        copy_err(err, errsz, "bad table id");
        return -1;
    }
    *table_out = tid;
    return 0;
}

static void append_item_payload(char *buf, size_t bufsz, size_t *pos,
                                const OrderLineItem *it) {
    char name[MAX_NAME];
    strncpy(name, it->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    sanitize_field(name);
    int w = snprintf(buf + *pos, bufsz > *pos ? bufsz - *pos : 0,
                     "%s%d:%d:%s:%d", *pos > 0 ? ";" : "", it->menu_id, it->qty,
                     name, it->unit_price);
    if (w > 0) {
        *pos += (size_t)w;
    }
}

void proto_build_order_broadcast(const Order *ord, char *out, size_t outsz) {
    size_t pos = 0;
    int w = snprintf(out + pos, outsz > pos ? outsz - pos : 0,
                     "ORDER_EVENT|order_id=%d|table=%d|status=%s|total=%d|"
                     "created=%lld|items=",
                     ord->order_id, ord->table_id, status_to_string(ord->status),
                     ord->total_price, (long long)ord->created_at);
    if (w < 0) {
        out[0] = '\0';
        return;
    }
    pos += (size_t)w;
    for (int i = 0; i < ord->item_count; ++i) {
        append_item_payload(out, outsz, &pos, &ord->items[i]);
    }
}

void proto_build_staff_broadcast(int table_id, char *out, size_t outsz) {
    snprintf(out, outsz, "STAFF_CALL|table=%d", table_id);
}

static const char *find_items_payload(const char *msg) {
    const char *p = strstr(msg, "|items=");
    if (!p) {
        return NULL;
    }
    return p + strlen("|items=");
}

int proto_parse_order_broadcast(const char *msg, Order *out_order, char *err,
                                size_t errsz) {
    memset(out_order, 0, sizeof(*out_order));
    if (strncmp(msg, "ORDER_EVENT|", 12) != 0) {
        copy_err(err, errsz, "not ORDER_EVENT");
        return -1;
    }
    char buf[MAX_PROTO_LINE];
    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *first = strtok_r(buf, "|", &saveptr);
    (void)first;
    char *tok = strtok_r(NULL, "|", &saveptr);
    while (tok) {
        if (strncmp(tok, "order_id=", 9) == 0) {
            out_order->order_id = atoi(tok + 9);
        } else if (strncmp(tok, "table=", 6) == 0) {
            out_order->table_id = atoi(tok + 6);
        } else if (strncmp(tok, "status=", 7) == 0) {
            out_order->status = status_from_string(tok + 7);
        } else if (strncmp(tok, "total=", 6) == 0) {
            out_order->total_price = atoi(tok + 6);
        } else if (strncmp(tok, "created=", 8) == 0) {
            out_order->created_at = (time_t)atoll(tok + 8);
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }

    const char *raw_items = find_items_payload(msg);
    if (!raw_items) {
        copy_err(err, errsz, "missing items payload");
        return -1;
    }
    char ip[MAX_PROTO_LINE];
    strncpy(ip, raw_items, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';

    int ic = 0;
    char *save2 = NULL;
    char *seg = strtok_r(ip, ";", &save2);
    while (seg && ic < MAX_ORDER_ITEMS) {
        char segbuf[512];
        strncpy(segbuf, seg, sizeof(segbuf) - 1);
        segbuf[sizeof(segbuf) - 1] = '\0';
        char *s3 = NULL;
        char *f1 = strtok_r(segbuf, ":", &s3);
        char *f2 = strtok_r(NULL, ":", &s3);
        char *f3 = strtok_r(NULL, ":", &s3);
        char *f4 = strtok_r(NULL, ":", &s3);
        if (!f1 || !f2 || !f3 || !f4) {
            break;
        }
        out_order->items[ic].menu_id = atoi(f1);
        out_order->items[ic].qty = atoi(f2);
        strncpy(out_order->items[ic].name, f3, MAX_NAME - 1);
        out_order->items[ic].name[MAX_NAME - 1] = '\0';
        out_order->items[ic].unit_price = atoi(f4);
        ++ic;
        seg = strtok_r(NULL, ";", &save2);
    }
    out_order->item_count = ic;
    return 0;
}
