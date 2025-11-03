#include "stress_test.h"

const char* stress_status_string(ipc_status_t status) {
    switch ((int)status) {
        case STRESS_STATUS_PING_OK:
            return "Ping successful";
        case STRESS_STATUS_HEAVY_OK:
            return "Heavy payload processed";
        case STRESS_STATUS_BURST_OK:
            return "Burst complete";
        case STRESS_STATUS_TIMEOUT_OK:
            return "Timeout test passed";
        case STRESS_STATUS_SHARE_OK:
            return "Shared memory processed";
        default:
            return NULL;
    }
}