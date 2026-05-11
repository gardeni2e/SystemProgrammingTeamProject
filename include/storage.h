#ifndef STORAGE_H
#define STORAGE_H

#include <sys/types.h>

#include "common.h"

typedef struct {
    int max_tables;
    int next_order_id;
} StoreConfig;

int storage_load_config(const char *path, StoreConfig *cfg, char *err,
                        size_t errsz);
int storage_save_config(const char *path, const StoreConfig *cfg, char *err,
                        size_t errsz);

int storage_append_orders_log(const char *path, const char *line, char *err,
                              size_t errsz);
int storage_append_sales_log(const char *path, const char *line, char *err,
                               size_t errsz);

int storage_append_server_log(const char *path, const char *line);

ssize_t storage_read_tail(const char *path, char *out, size_t outsiz,
                          char *err, size_t errsz);

#endif
