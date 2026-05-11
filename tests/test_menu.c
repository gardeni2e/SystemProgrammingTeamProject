#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "menu.h"

static int fail(const char *msg) {
    fprintf(stderr, "[test_menu] FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    char path[] = "/tmp/elec462_menu_test.csv";
    unlink(path);

    MenuCatalog cat;
    char err[160];
    if (menu_load_file(&cat, path, err, sizeof(err)) != 0) {
        return fail("load default menu");
    }
    if (cat.count <= 0) {
        return fail("menu empty");
    }

    MenuItem mi;
    memset(&mi, 0, sizeof(mi));
    strncpy(mi.name, "Unit Burger", sizeof(mi.name) - 1);
    mi.price = 6500;
    mi.sold_out = 0;
    if (menu_add_item(&cat, &mi, err, sizeof(err)) != 0) {
        return fail("add item");
    }

    int nid = cat.items[cat.count - 1].id;
    if (menu_set_soldout(&cat, nid, 1, err, sizeof(err)) != 0) {
        return fail("soldout");
    }

    if (menu_save_file(&cat, path, err, sizeof(err)) != 0) {
        return fail("save");
    }

    MenuCatalog cat2;
    if (menu_load_file(&cat2, path, err, sizeof(err)) != 0) {
        return fail("reload");
    }

    const MenuItem *found = menu_find_by_id(&cat2, nid);
    if (!found || !found->sold_out) {
        return fail("persist soldout");
    }

    printf("[test_menu] PASS\n");
    unlink(path);
    return 0;
}
