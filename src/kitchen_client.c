#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ui.h"

static void usage(const char *prog) {
    fprintf(stderr, "사용법: %s <host> <port>\n예: %s 127.0.0.1 9090\n", prog,
            prog);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    KitchenUiArgs args;
    memset(&args, 0, sizeof(args));
    strncpy(args.host, argv[1], sizeof(args.host) - 1);
    args.port = (uint16_t)atoi(argv[2]);

    if (args.port == 0) {
        usage(argv[0]);
        return 1;
    }

    ui_run_kitchen(&args);
    return 0;
}
