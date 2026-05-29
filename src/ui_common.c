#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <locale.h>

#include "protocol.h"
#include "ui_common.h"

void ui_locale_utf8(void) {
    setlocale(LC_ALL, "");
}

int ui_write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

int ui_sock_read_line(UiSockReader *sr, char *out, size_t outsz) {
    if (outsz == 0) {
        return -1;
    }
    for (;;) {
        for (size_t i = 0; i < sr->len; ++i) {
            if (sr->buf[i] == '\n') {
                size_t take = i + 1;
                size_t copy = take - 1;
                if (copy >= outsz) {
                    copy = outsz - 1;
                }
                memcpy(out, sr->buf, copy);
                out[copy] = '\0';
                while (copy > 0 &&
                       (out[copy - 1] == '\r' || out[copy - 1] == '\n')) {
                    out[--copy] = '\0';
                }
                memmove(sr->buf, sr->buf + take, sr->len - take);
                sr->len -= take;
                return 1;
            }
        }
        if (sr->len >= sizeof(sr->buf)) {
            return -1;
        }
        ssize_t rd =
            read(sr->fd, sr->buf + sr->len, sizeof(sr->buf) - sr->len);
        if (rd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rd == 0) {
            return 0;
        }
        sr->len += (size_t)rd;
    }
}

void ui_net_enqueue(UiNetQueue *nq, const char *line) {
    pthread_mutex_lock(&nq->mu);
    if (nq->qn < 128) {
        strncpy(nq->q[nq->qn], line, MAX_PROTO_LINE - 1);
        nq->q[nq->qn][MAX_PROTO_LINE - 1] = '\0';
        nq->qn++;
    }
    pthread_mutex_unlock(&nq->mu);
}

int ui_net_pop(UiNetQueue *nq, char *out, size_t outsz) {
    pthread_mutex_lock(&nq->mu);
    if (nq->qn <= 0) {
        pthread_mutex_unlock(&nq->mu);
        return 0;
    }
    strncpy(out, nq->q[0], outsz - 1);
    out[outsz - 1] = '\0';
    for (int i = 1; i < nq->qn; ++i) {
        memcpy(nq->q[i - 1], nq->q[i], MAX_PROTO_LINE);
    }
    nq->qn--;
    pthread_mutex_unlock(&nq->mu);
    return 1;
}

void *ui_net_reader_main(void *arg) {
    UiNetQueue *nq = arg;
    UiSockReader sr;
    memset(&sr, 0, sizeof(sr));
    sr.fd = nq->sock;
    char line[MAX_PROTO_LINE];
    while (!nq->stop && ui_sock_read_line(&sr, line, sizeof(line)) > 0) {
        ui_net_enqueue(nq, line);
    }
    return NULL;
}

int ui_tcp_connect(const char *host, uint16_t port, char *err,
                          size_t errsz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errsz, "socket %s", strerror(errno));
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        snprintf(err, errsz, "bad host");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        snprintf(err, errsz, "connect %s", strerror(errno));
        return -1;
    }
    return fd;
}

void ui_apply_menu_response(MenuCatalog *cat, const char *line) {
    const char *p = strstr(line, "MENU_RESPONSE|");
    if (!p) {
        return;
    }
    p += strlen("MENU_RESPONSE|");
    menu_init_catalog(cat);
    char buf[MAX_PROTO_LINE];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    char *seg = strtok_r(buf, ";", &save);
    while (seg) {
        MenuItem mi;
        if (proto_parse_menu_csv_segment(seg, &mi) == 0 && mi.id > 0 &&
            cat->count < MAX_MENU_ITEMS) {
            cat->items[cat->count++] = mi;
        }
        seg = strtok_r(NULL, ";", &save);
    }
}

void ui_merge_order_mirror(Order *orders, int *count, const Order *incoming) {
    int idx = orders_find_index(orders, *count, incoming->order_id);
    if (idx >= 0) {
        orders[idx] = *incoming;
        return;
    }
    if (*count < MAX_ORDERS) {
        orders[(*count)++] = *incoming;
    }
}

void ui_send_line(int sock, const char *s) {
    ui_write_all(sock, s, strlen(s));
}
