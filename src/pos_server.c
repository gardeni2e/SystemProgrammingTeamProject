#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "server.h"
#include "ui.h"

int main(int argc, char **argv) {
    uint16_t port = DEFAULT_PORT;
    if (argc >= 2) {
        port = (uint16_t)atoi(argv[1]);
    }

    ServerContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    server_install_sigint_handler(&ctx);

    char err[256];
    if (server_init(&ctx, port, err, sizeof(err)) != 0) {
        fprintf(stderr, "[POS] %s\n", err);
        return 1;
    }

    if (server_load_persistent(&ctx, err, sizeof(err)) != 0) {
        fprintf(stderr, "[POS] %s\n", err);
        server_shutdown(&ctx);
        return 1;
    }

    if (pthread_create(&ctx.accept_thread, NULL, server_accept_loop, &ctx) !=
        0) {
        fprintf(stderr, "[POS] accept thread creation failed\n");
        server_shutdown(&ctx);
        return 1;
    }
    ctx.accept_thread_started = 1;

    ui_run_pos(&ctx);

    ctx.shutting_down = 1;
    server_shutdown(&ctx);
    return 0;
}
