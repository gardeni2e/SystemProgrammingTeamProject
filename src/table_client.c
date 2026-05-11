#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ui.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "사용법: %s <host> <port> <table_id>\n"
            "예: %s 127.0.0.1 9090 1\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    TableUiArgs args;
    memset(&args, 0, sizeof(args));
    strncpy(args.host, argv[1], sizeof(args.host) - 1);
    args.port = (uint16_t)atoi(argv[2]);
    args.table_id = atoi(argv[3]);

    if (args.port == 0 || args.table_id <= 0) {
        usage(argv[0]);
        return 1;
    }

    ui_run_table(&args);
    return 0;
}
