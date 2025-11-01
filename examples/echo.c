#include "echo.h"

const char *echo_status_string(ipc_status_t status) {
    switch ((int)status) {
        case ECHO_CUSTOM_STATUS:
            return "Custom Echo Status";
        default:
            return NULL;
    }
}
