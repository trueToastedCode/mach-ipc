#include "mach_ipc.h"

#ifndef ECHO_H
#define ECHO_H

#define MSG_TYPE_ECHO (1UL)
#define MSG_TYPE_SILENT (2UL)

#define MSG_ID_ECHO (SET_FEATURE(MSG_ID_USER(MSG_TYPE_ECHO), INTERNAL_FEATURE_UPSH))
#define MSG_ID_SILENT (MSG_ID_USER(MSG_TYPE_SILENT))

#define ECHO_CUSTOM_STATUS (IPC_USER_BASE + 1) 

const char *echo_status_string(ipc_status_t status);

#endif // ECHO_H