#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "menu.h"

void menu_init_catalog(MenuCatalog *cat) {
    memset(cat, 0, sizeof(*cat));
}

static int write_default_menu_file(const char *path, char *err, size_t errsz) {
    const char *csv =
        "id,name,price,soldout\n"
        "1,Kimchi Fried Rice,9000,0\n"
        "2,Soy Sauce Egg Rice,7500,0\n"
        "3,Miso Soup,3000,0\n"
        "4,Ice Americano,4500,0\n"
        "5,Latte,5000,0\n";
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "open default menu: %s", strerror(errno));
        return -1;
    }
    ssize_t wr = write(fd, csv, strlen(csv));
    close(fd);
    if (wr != (ssize_t)strlen(csv)) {
        snprintf(err, errsz, "write default menu incomplete");
        return -1;
    }
    return 0;
}

static int parse_header_ok(const char *line) {
    return strstr(line, "id") && strstr(line, "name") && strstr(line, "price");
}

int menu_load_file(MenuCatalog *cat, const char *path, char *err, size_t errsz) {
    menu_init_catalog(cat);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (write_default_menu_file(path, err, errsz) != 0) {
            return -1;
        }
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "open menu: %s", strerror(errno));
        return -1;
    }
    char buf[65536];
    ssize_t rd = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (rd < 0) {
        snprintf(err, errsz, "read menu: %s", strerror(errno));
        return -1;
    }
    buf[rd] = '\0';

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    int first = 1;
    while (line) {
        char work[512];
        strncpy(work, line, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
        if (first) {
            first = 0;
            if (!parse_header_ok(work)) {
                snprintf(err, errsz, "bad csv header");
                return -1;
            }
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        char *s2 = NULL;
        char *idtok = strtok_r(work, ",", &s2);
        char *nametok = strtok_r(NULL, ",", &s2);
        char *pricetok = strtok_r(NULL, ",", &s2);
        char *sotok = strtok_r(NULL, ",", &s2);
        if (!idtok || !nametok || !pricetok || !sotok) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (cat->count >= MAX_MENU_ITEMS) {
            break;
        }
        MenuItem *m = &cat->items[cat->count];
        m->id = atoi(idtok);
        strncpy(m->name, nametok, sizeof(m->name) - 1);
        m->name[sizeof(m->name) - 1] = '\0';
        m->price = atoi(pricetok);
        m->sold_out = atoi(sotok) ? 1 : 0;
        if (m->id > 0 && m->name[0] != '\0') {
            cat->count++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (cat->count == 0) {
        if (write_default_menu_file(path, err, errsz) != 0) {
            return -1;
        }
        return menu_load_file(cat, path, err, errsz);
    }
    return 0;
}

int menu_save_file(const MenuCatalog *cat, const char *path, char *err,
                   size_t errsz) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "open tmp menu: %s", strerror(errno));
        return -1;
    }
    char line[512];
    int n = snprintf(line, sizeof(line), "id,name,price,soldout\n");
    if (write(fd, line, (size_t)n) != n) {
        close(fd);
        snprintf(err, errsz, "write header failed");
        return -1;
    }
    for (int i = 0; i < cat->count; ++i) {
        const MenuItem *m = &cat->items[i];
        n = snprintf(line, sizeof(line), "%d,%s,%d,%d\n", m->id, m->name,
                     m->price, m->sold_out ? 1 : 0);
        if (write(fd, line, (size_t)n) != n) {
            close(fd);
            snprintf(err, errsz, "write row failed");
            return -1;
        }
    }
    if (fsync(fd) != 0) {
        close(fd);
        snprintf(err, errsz, "fsync menu tmp failed");
        return -1;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        snprintf(err, errsz, "rename menu: %s", strerror(errno));
        return -1;
    }
    return 0;
}

const MenuItem *menu_find_by_id(const MenuCatalog *cat, int id) {
    int idx = menu_index_by_id(cat, id);
    if (idx < 0) {
        return NULL;
    }
    return &cat->items[idx];
}

int menu_index_by_id(const MenuCatalog *cat, int id) {
    for (int i = 0; i < cat->count; ++i) {
        if (cat->items[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int next_menu_id(const MenuCatalog *cat) {
    int max_id = 0;
    for (int i = 0; i < cat->count; ++i) {
        if (cat->items[i].id > max_id) {
            max_id = cat->items[i].id;
        }
    }
    return max_id + 1;
}

int menu_add_item(MenuCatalog *cat, const MenuItem *item, char *err,
                  size_t errsz) {
    if (cat->count >= MAX_MENU_ITEMS) {
        snprintf(err, errsz, "menu full");
        return -1;
    }
    MenuItem copy = *item;
    if (copy.id <= 0) {
        copy.id = next_menu_id(cat);
    }
    if (menu_index_by_id(cat, copy.id) >= 0) {
        snprintf(err, errsz, "duplicate id");
        return -1;
    }
    cat->items[cat->count++] = copy;
    return 0;
}

int menu_update_item(MenuCatalog *cat, int id, const MenuItem *item, char *err,
                     size_t errsz) {
    int idx = menu_index_by_id(cat, id);
    if (idx < 0) {
        snprintf(err, errsz, "menu id not found");
        return -1;
    }
    MenuItem copy = *item;
    copy.id = id;
    cat->items[idx] = copy;
    return 0;
}

int menu_delete_item(MenuCatalog *cat, int id, char *err, size_t errsz) {
    int idx = menu_index_by_id(cat, id);
    if (idx < 0) {
        snprintf(err, errsz, "menu id not found");
        return -1;
    }
    for (int i = idx; i < cat->count - 1; ++i) {
        cat->items[i] = cat->items[i + 1];
    }
    cat->count--;
    return 0;
}

int menu_set_soldout(MenuCatalog *cat, int id, int sold_out, char *err,
                     size_t errsz) {
    int idx = menu_index_by_id(cat, id);
    if (idx < 0) {
        snprintf(err, errsz, "menu id not found");
        return -1;
    }
    cat->items[idx].sold_out = sold_out ? 1 : 0;
    return 0;
}
