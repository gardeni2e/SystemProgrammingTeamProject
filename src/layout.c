#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "layout.h"

void layout_init(StoreLayout *layout) {
    memset(layout, 0, sizeof(*layout));
    layout->grid_rows = 5;
    layout->grid_cols = 7;
}

const char *layout_zone_name(TableZone zone) {
    return zone == TABLE_ZONE_INSIDE ? "inside" : "outside";
}

static TableZone layout_zone_from_string(const char *s) {
    if (!s) {
        return TABLE_ZONE_OUTSIDE;
    }
    if (strcmp(s, "inside") == 0 || strcmp(s, "안") == 0) {
        return TABLE_ZONE_INSIDE;
    }
    return TABLE_ZONE_OUTSIDE;
}

static void layout_add_slot(StoreLayout *layout, int table_id, TableZone zone,
                            int row, int col, const char *label) {
    if (layout->count >= MAX_LAYOUT_TABLES) {
        return;
    }
    TablePlacement *tp = &layout->tables[layout->count++];
    tp->table_id = table_id;
    tp->zone = zone;
    tp->row = row;
    tp->col = col;
    tp->active = 1;
    snprintf(tp->label, sizeof(tp->label), "%s", label);
}

void layout_init_default(StoreLayout *layout) {
    layout_init(layout);
    layout->grid_rows = 5;
    layout->grid_cols = 7;

    
    layout->count = 0;

    
    layout_add_slot(layout, 1, TABLE_ZONE_OUTSIDE, 0, 0, "밖1");
    layout_add_slot(layout, 2, TABLE_ZONE_OUTSIDE, 0, 1, "밖2");
    layout_add_slot(layout, 3, TABLE_ZONE_OUTSIDE, 1, 0, "밖3");
    layout_add_slot(layout, 4, TABLE_ZONE_OUTSIDE, 1, 1, "밖4");
    layout_add_slot(layout, 5, TABLE_ZONE_OUTSIDE, 2, 0, "밖5");
    layout_add_slot(layout, 6, TABLE_ZONE_OUTSIDE, 2, 1, "밖6");
    layout_add_slot(layout, 7, TABLE_ZONE_OUTSIDE, 3, 0, "밖7");
    layout_add_slot(layout, 8, TABLE_ZONE_OUTSIDE, 3, 1, "밖8");

    
    layout_add_slot(layout, 9, TABLE_ZONE_INSIDE, 1, 3, "안1");
    layout_add_slot(layout, 10, TABLE_ZONE_INSIDE, 1, 4, "안2");
    layout_add_slot(layout, 11, TABLE_ZONE_INSIDE, 1, 5, "안3");
    layout_add_slot(layout, 12, TABLE_ZONE_INSIDE, 2, 3, "안4");
    layout_add_slot(layout, 13, TABLE_ZONE_INSIDE, 2, 4, "안5");
    layout_add_slot(layout, 14, TABLE_ZONE_INSIDE, 2, 5, "안6");
}

TablePlacement *layout_find_by_id(StoreLayout *layout, int table_id) {
    for (int i = 0; i < layout->count; ++i) {
        if (layout->tables[i].active && layout->tables[i].table_id == table_id) {
            return &layout->tables[i];
        }
    }
    return NULL;
}

TablePlacement *layout_find_at(StoreLayout *layout, int row, int col) {
    for (int i = 0; i < layout->count; ++i) {
        TablePlacement *tp = &layout->tables[i];
        if (tp->active && tp->row == row && tp->col == col) {
            return tp;
        }
    }
    return NULL;
}

int layout_max_table_id(const StoreLayout *layout) {
    int max_id = 0;
    for (int i = 0; i < layout->count; ++i) {
        if (layout->tables[i].active && layout->tables[i].table_id > max_id) {
            max_id = layout->tables[i].table_id;
        }
    }
    return max_id;
}

static void ensure_layout_file(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return;
    }
    StoreLayout def;
    char err[128];
    layout_init_default(&def);
    layout_save(path, &def, err, sizeof(err));
}

int layout_load(const char *path, StoreLayout *layout, char *err, size_t errsz) {
    ensure_layout_file(path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "open layout: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        snprintf(err, errsz, "stat layout");
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    if (sz > 65536) {
        sz = 65536;
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        close(fd);
        snprintf(err, errsz, "oom");
        return -1;
    }
    ssize_t rd = read(fd, buf, sz);
    close(fd);
    if (rd < 0) {
        free(buf);
        snprintf(err, errsz, "read layout");
        return -1;
    }
    buf[rd] = '\0';

    layout_init(layout);
    layout->count = 0;

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        if (line[0] == '#' || line[0] == '\0') {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (strncmp(line, "grid_rows=", 10) == 0) {
            layout->grid_rows = atoi(line + 10);
        } else if (strncmp(line, "grid_cols=", 10) == 0) {
            layout->grid_cols = atoi(line + 10);
        } else if (strncmp(line, "table=", 6) == 0) {
            if (layout->count >= MAX_LAYOUT_TABLES) {
                line = strtok_r(NULL, "\n", &save);
                continue;
            }
            TablePlacement tp;
            memset(&tp, 0, sizeof(tp));
            tp.active = 1;
            tp.zone = TABLE_ZONE_OUTSIDE;
            char zonebuf[16];
            zonebuf[0] = '\0';
            char labelbuf[MAX_TABLE_LABEL];
            labelbuf[0] = '\0';
            int id = 0, row = 0, col = 0;
            sscanf(line + 6, "%d", &id);
            const char *p = strchr(line + 6, ' ');
            while (p && *p) {
                while (*p == ' ') {
                    p++;
                }
                if (strncmp(p, "zone=", 5) == 0) {
                    sscanf(p + 5, "%15s", zonebuf);
                } else if (strncmp(p, "row=", 4) == 0) {
                    sscanf(p + 4, "%d", &row);
                } else if (strncmp(p, "col=", 4) == 0) {
                    sscanf(p + 4, "%d", &col);
                } else if (strncmp(p, "label=", 6) == 0) {
                    sscanf(p + 6, "%15s", labelbuf);
                }
                p = strchr(p + 1, ' ');
            }
            tp.table_id = id;
            tp.row = row;
            tp.col = col;
            tp.zone = layout_zone_from_string(zonebuf);
            if (labelbuf[0]) {
                snprintf(tp.label, sizeof(tp.label), "%s", labelbuf);
            } else {
                snprintf(tp.label, sizeof(tp.label), "T%d", id);
            }
            layout->tables[layout->count++] = tp;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(buf);

    if (layout->grid_rows <= 0 || layout->grid_rows > MAX_GRID_ROWS) {
        layout->grid_rows = 9;
    }
    if (layout->grid_cols <= 0 || layout->grid_cols > MAX_GRID_COLS) {
        layout->grid_cols = 14;
    }
    if (layout->count == 0) {
        layout_init_default(layout);
    }
    return 0;
}

int layout_save(const char *path, const StoreLayout *layout, char *err,
               size_t errsz) {
    char body[32768];
    size_t pos = 0;
    int n = snprintf(body + pos, sizeof(body) - pos,
                     "# POS table layout (grid + placements)\n"
                     "grid_rows=%d\n"
                     "grid_cols=%d\n",
                     layout->grid_rows, layout->grid_cols);
    if (n < 0) {
        snprintf(err, errsz, "format layout");
        return -1;
    }
    pos += (size_t)n;

    for (int i = 0; i < layout->count; ++i) {
        const TablePlacement *tp = &layout->tables[i];
        if (!tp->active) {
            continue;
        }
        n = snprintf(body + pos, sizeof(body) - pos,
                     "table=%d zone=%s row=%d col=%d label=%s\n",
                     tp->table_id, layout_zone_name(tp->zone), tp->row, tp->col,
                     tp->label);
        if (n < 0 || pos + (size_t)n >= sizeof(body)) {
            snprintf(err, errsz, "layout buffer full");
            return -1;
        }
        pos += (size_t)n;
    }

    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
    int fd = open(tmppath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "open layout tmp");
        return -1;
    }
    ssize_t w = write(fd, body, pos);
    fsync(fd);
    close(fd);
    if (w != (ssize_t)pos) {
        snprintf(err, errsz, "write layout");
        return -1;
    }
    if (rename(tmppath, path) != 0) {
        snprintf(err, errsz, "rename layout");
        return -1;
    }
    return 0;
}
