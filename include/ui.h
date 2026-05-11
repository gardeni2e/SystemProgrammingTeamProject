#ifndef UI_H
#define UI_H

#include <stdint.h>

#include "common.h"

struct ServerContext;

void ui_run_pos(struct ServerContext *ctx);

typedef struct {
    char host[128];
    uint16_t port;
    int table_id;
} TableUiArgs;

void ui_run_table(const TableUiArgs *args);

typedef struct {
    char host[128];
    uint16_t port;
} KitchenUiArgs;

void ui_run_kitchen(const KitchenUiArgs *args);

#endif
