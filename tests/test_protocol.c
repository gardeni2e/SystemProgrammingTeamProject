#include <stdio.h>
#include <string.h>

#include "protocol.h"

static int fail(const char *msg) {
    fprintf(stderr, "[test_protocol] FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    char err[128];
    int tbl = 0;
    CartItem lines[MAX_ORDER_ITEMS];
    int n = 0;

    const char *msg =
        "ORDER_CREATE|table=2|items=1:2,3:1";

    if (proto_parse_order_create(msg, &tbl, lines, MAX_ORDER_ITEMS, &n, err,
                                 sizeof(err)) != 0) {
        return fail("parse create");
    }
    if (tbl != 2 || n != 2 || lines[0].menu_id != 1 || lines[0].qty != 2 ||
        lines[1].menu_id != 3 || lines[1].qty != 1) {
        return fail("create fields");
    }

    int oid = 0;
    OrderStatus st = STATUS_WAITING;
    const char *upd = "ORDER_UPDATE|order_id=7|status=COOKING";
    if (proto_parse_order_update(upd, &oid, &st, err, sizeof(err)) != 0) {
        return fail("parse update");
    }
    if (oid != 7 || st != STATUS_COOKING) {
        return fail("update fields");
    }

    Order ord;
    memset(&ord, 0, sizeof(ord));
    ord.order_id = 9;
    ord.table_id = 4;
    ord.status = STATUS_WAITING;
    ord.total_price = 12000;
    ord.created_at = 1700000000;
    ord.item_count = 1;
    ord.items[0].menu_id = 5;
    ord.items[0].qty = 2;
    strncpy(ord.items[0].name, "Soup", sizeof(ord.items[0].name) - 1);
    ord.items[0].unit_price = 6000;

    char buf[MAX_PROTO_LINE];
    proto_build_order_broadcast(&ord, buf, sizeof(buf));

    Order parsed;
    if (proto_parse_order_broadcast(buf, &parsed, err, sizeof(err)) != 0) {
        return fail("roundtrip parse");
    }
    if (parsed.order_id != ord.order_id || parsed.table_id != ord.table_id ||
        parsed.total_price != ord.total_price ||
        parsed.item_count != ord.item_count ||
        strcmp(parsed.items[0].name, ord.items[0].name) != 0) {
        return fail("roundtrip fields");
    }

    printf("[test_protocol] PASS\n");
    return 0;
}
