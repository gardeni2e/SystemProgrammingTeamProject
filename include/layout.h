#ifndef LAYOUT_H
#define LAYOUT_H

#include "common.h"

#define MAX_GRID_ROWS 24
#define MAX_GRID_COLS 40
#define MAX_LAYOUT_TABLES MAX_TABLES
#define MAX_TABLE_LABEL 16
#define LAYOUT_PATH "data/layout.conf"

typedef enum {
    TABLE_ZONE_OUTSIDE = 0,
    TABLE_ZONE_INSIDE = 1
} TableZone;

typedef struct {
    int table_id;
    TableZone zone;
    int row;
    int col;
    char label[MAX_TABLE_LABEL];
    int active;
} TablePlacement;

typedef struct {
    int grid_rows;
    int grid_cols;
    int count;
    TablePlacement tables[MAX_LAYOUT_TABLES];
} StoreLayout;

void layout_init(StoreLayout *layout);
void layout_init_default(StoreLayout *layout);
int layout_load(const char *path, StoreLayout *layout, char *err, size_t errsz);
int layout_save(const char *path, const StoreLayout *layout, char *err,
               size_t errsz);

TablePlacement *layout_find_by_id(StoreLayout *layout, int table_id);
TablePlacement *layout_find_at(StoreLayout *layout, int row, int col);
int layout_max_table_id(const StoreLayout *layout);

const char *layout_zone_name(TableZone zone);

#endif
