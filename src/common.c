#include <string.h>

#include "common.h"

const char *status_to_string(OrderStatus st) {
    switch (st) {
    case STATUS_WAITING:
        return "WAITING";
    case STATUS_COOKING:
        return "COOKING";
    case STATUS_DONE:
        return "DONE";
    case STATUS_PAID:
        return "PAID";
    default:
        return "WAITING";
    }
}

OrderStatus status_from_string(const char *s) {
    if (!s) {
        return STATUS_WAITING;
    }
    if (strcmp(s, "WAITING") == 0) {
        return STATUS_WAITING;
    }
    if (strcmp(s, "COOKING") == 0) {
        return STATUS_COOKING;
    }
    if (strcmp(s, "DONE") == 0) {
        return STATUS_DONE;
    }
    if (strcmp(s, "PAID") == 0) {
        return STATUS_PAID;
    }
    return STATUS_WAITING;
}
