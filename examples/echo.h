#include "mach_ipc.h"

#ifndef ECHO_H
#define ECHO_H

#define MSG_ECHO (1)
#define MSG_SILENT (2)

#define ECHO_CUSTOM_STATUS (IPC_USER_BASE + 1)

const char *echo_status_string(ipc_status_t status);

#endif // ECHO_H