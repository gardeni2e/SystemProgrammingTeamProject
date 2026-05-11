#include <stdio.h>
#include <string.h>

#include "order.h"

static int fail(const char *msg) {
    fprintf(stderr, "[test_order] FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    Cart cart;
    cart_init(&cart);

    char err[128];
    CartItem a = {.menu_id = 1};
    strncpy(a.name, "Americano", sizeof(a.name) - 1);
    a.price = 4000;
    a.qty = 2;
    if (cart_add(&cart, &a, err, sizeof(err)) != 0) {
        return fail("add");
    }

    CartItem b = {.menu_id = 2};
    strncpy(b.name, "Latte", sizeof(b.name) - 1);
    b.price = 4500;
    b.qty = 1;
    if (cart_add(&cart, &b, err, sizeof(err)) != 0) {
        return fail("add2");
    }

    if (cart_total(&cart) != 12500) {
        return fail("total mismatch");
    }

    if (cart_set_qty(&cart, 1, 3, err, sizeof(err)) != 0) {
        return fail("set qty");
    }
    if (cart_total(&cart) != 16500) {
        return fail("total after qty");
    }

    Order ord;
    if (order_build_from_cart(&ord, 42, 3, &cart, err, sizeof(err)) != 0) {
        return fail("build order");
    }
    if (ord.order_id != 42 || ord.table_id != 3 || ord.item_count != 2) {
        return fail("order meta");
    }

    printf("[test_order] PASS\n");
    return 0;
}
