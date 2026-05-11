#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "storage.h"

ssize_t storage_read_tail(const char *path, char *out, size_t outsiz,
                          char *err, size_t errsz) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (err && errsz) {
            snprintf(err, errsz, "stat tail: %s", strerror(errno));
        }
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (err && errsz) {
            snprintf(err, errsz, "open tail");
        }
        return -1;
    }
    off_t want = (off_t)outsiz - 1;
    if (want < 0) {
        want = 0;
    }
    off_t start = 0;
    if (st.st_size > want) {
        start = st.st_size - want;
    }
    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        if (err && errsz) {
            snprintf(err, errsz, "lseek tail failed");
        }
        return -1;
    }
    ssize_t rd = read(fd, out, outsiz - 1);
    close(fd);
    if (rd < 0) {
        if (err && errsz) {
            snprintf(err, errsz, "read tail failed");
        }
        return -1;
    }
    out[rd] = '\0';
    return rd;
}

static void write_entire_file(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return;
    }
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    (void)w;
    fsync(fd);
    close(fd);
}

static void ensure_defaults(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return;
    }
    write_entire_file(path,
                      "# tables=N sets seat count\n"
                      "# next_order_id starts from 1\n"
                      "tables=8\n"
                      "next_order_id=1\n");
}

int storage_load_config(const char *path, StoreConfig *cfg, char *err,
                        size_t errsz) {
    ensure_defaults(path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "open config: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        close(fd);
        snprintf(err, errsz, "stat config failed");
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
        snprintf(err, errsz, "read config");
        return -1;
    }
    buf[rd] = '\0';

    cfg->max_tables = 8;
    cfg->next_order_id = 1;
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        if (line[0] == '#' || line[0] == '\0') {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (strncmp(line, "tables=", 7) == 0) {
            cfg->max_tables = atoi(line + 7);
        } else if (strncmp(line, "next_order_id=", 14) == 0) {
            cfg->next_order_id = atoi(line + 14);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(buf);
    if (cfg->max_tables <= 0 || cfg->max_tables > MAX_TABLES) {
        cfg->max_tables = 8;
    }
    if (cfg->next_order_id <= 0) {
        cfg->next_order_id = 1;
    }
    return 0;
}

static int rewrite_config_lines(const char *path, const StoreConfig *cfg,
                                char *err, size_t errsz) {
    struct stat st;
    char stackbuf[4096];
    char *heapbuf = NULL;
    char *buf = stackbuf;
    size_t cap = sizeof(stackbuf);

    if (stat(path, &st) == 0 && st.st_size > 0) {
        size_t need = (size_t)st.st_size + 1;
        if (need > sizeof(stackbuf)) {
            heapbuf = malloc(need);
            if (!heapbuf) {
                snprintf(err, errsz, "oom");
                return -1;
            }
            buf = heapbuf;
            cap = need;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            free(heapbuf);
            snprintf(err, errsz, "open config ro");
            return -1;
        }
        ssize_t rd = read(fd, buf, cap - 1);
        close(fd);
        if (rd < 0) {
            free(heapbuf);
            snprintf(err, errsz, "read config");
            return -1;
        }
        buf[rd] = '\0';
    } else {
        snprintf(buf, cap,
                 "# tables=N sets seat count\n"
                 "# next_order_id persists counters\n"
                 "tables=8\n"
                 "next_order_id=1\n");
    }

    char outbuf[8192];
    size_t opos = 0;
    char *save = NULL;
    char localcopy[8192];
    strncpy(localcopy, buf, sizeof(localcopy) - 1);
    localcopy[sizeof(localcopy) - 1] = '\0';
    char *line = strtok_r(localcopy, "\n", &save);
    int saw_tables = 0;
    int saw_next = 0;
    while (line) {
        char tmp[512];
        if (line[0] == '#' || line[0] == '\0') {
            int n = snprintf(tmp, sizeof(tmp), "%s\n", line);
            if (opos + (size_t)n >= sizeof(outbuf)) {
                break;
            }
            memcpy(outbuf + opos, tmp, (size_t)n);
            opos += (size_t)n;
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (strncmp(line, "tables=", 7) == 0) {
            saw_tables = 1;
            int n = snprintf(tmp, sizeof(tmp), "tables=%d\n", cfg->max_tables);
            if (opos + (size_t)n >= sizeof(outbuf)) {
                break;
            }
            memcpy(outbuf + opos, tmp, (size_t)n);
            opos += (size_t)n;
        } else if (strncmp(line, "next_order_id=", 14) == 0) {
            saw_next = 1;
            int n = snprintf(tmp, sizeof(tmp), "next_order_id=%d\n",
                             cfg->next_order_id);
            if (opos + (size_t)n >= sizeof(outbuf)) {
                break;
            }
            memcpy(outbuf + opos, tmp, (size_t)n);
            opos += (size_t)n;
        } else {
            int n = snprintf(tmp, sizeof(tmp), "%s\n", line);
            if (opos + (size_t)n >= sizeof(outbuf)) {
                break;
            }
            memcpy(outbuf + opos, tmp, (size_t)n);
            opos += (size_t)n;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (!saw_tables) {
        int n = snprintf(outbuf + opos, sizeof(outbuf) - opos, "tables=%d\n",
                         cfg->max_tables);
        if (n > 0) {
            opos += (size_t)n;
        }
    }
    if (!saw_next) {
        int n =
            snprintf(outbuf + opos, sizeof(outbuf) - opos, "next_order_id=%d\n",
                     cfg->next_order_id);
        if (n > 0) {
            opos += (size_t)n;
        }
    }

    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
    int fdw = open(tmppath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fdw < 0) {
        free(heapbuf);
        snprintf(err, errsz, "open tmp config");
        return -1;
    }
    ssize_t w = write(fdw, outbuf, opos);
    fsync(fdw);
    close(fdw);
    free(heapbuf);
    if (w != (ssize_t)opos) {
        snprintf(err, errsz, "write tmp config");
        return -1;
    }
    if (rename(tmppath, path) != 0) {
        snprintf(err, errsz, "rename config");
        return -1;
    }
    return 0;
}

int storage_save_config(const char *path, const StoreConfig *cfg, char *err,
                        size_t errsz) {
    return rewrite_config_lines(path, cfg, err, errsz);
}

static int append_file(const char *path, const char *line, char *err,
                       size_t errsz) {
    int fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "open log %s: %s", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(line);
    ssize_t w = write(fd, line, len);
    if (w != (ssize_t)len) {
        close(fd);
        snprintf(err, errsz, "write log incomplete");
        return -1;
    }
    fsync(fd);
    close(fd);
    return 0;
}

int storage_append_orders_log(const char *path, const char *line, char *err,
                              size_t errsz) {
    return append_file(path, line, err, errsz);
}

int storage_append_sales_log(const char *path, const char *line, char *err,
                               size_t errsz) {
    return append_file(path, line, err, errsz);
}

int storage_append_server_log(const char *path, const char *line) {
    char err[64];
    return append_file(path, line, err, sizeof(err));
}
