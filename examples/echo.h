#include "mach_ipc.h"

#ifndef ECHO_H
#define ECHO_H

enum {
    MSG_TYPE_SET_ECHO_SHM = 1U,
    MSG_TYPE_ECHO,
    MSG_TYPE_SILENT
};

#define MSG_ID_SET_ECHO_SHM (SET_FEATURE(MSG_ID_USER(MSG_TYPE_SET_ECHO_SHM), INTERNAL_FEATURE_LPCY))
#define MSG_ID_ECHO (MSG_ID_USER(MSG_TYPE_ECHO))
#define MSG_ID_SILENT (MSG_ID_USER(MSG_TYPE_SILENT))

enum {
    ECHO_CUSTOM_STATUS = (IPC_USER_BASE + 1)
};

const char *echo_status_string(ipc_status_t status);

#endif // ECHO_H