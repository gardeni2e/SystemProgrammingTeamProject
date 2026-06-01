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
    case STATUS_CANCELLED:
        return "CANCELLED";
    default:
        return "WAITING";
    }
}

const char *status_to_label(OrderStatus st) {
    switch (st) {
    case STATUS_WAITING:
        return "대기";
    case STATUS_COOKING:
        return "조리중";
    case STATUS_DONE:
        return "조리완료";
    case STATUS_PAID:
        return "결제완료";
    case STATUS_CANCELLED:
        return "취소";
    default:
        return "대기";
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
    if (strcmp(s, "CANCELLED") == 0) {
        return STATUS_CANCELLED;
    }
    return STATUS_WAITING;
}
