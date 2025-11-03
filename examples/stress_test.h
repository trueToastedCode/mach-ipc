#ifndef STRESS_TEST_H
#define STRESS_TEST_H

#include "mach_ipc.h"

// Message types for stress testing
#define MSG_TYPE_PING           (1UL)
#define MSG_TYPE_HEAVY_PAYLOAD  (2UL)
#define MSG_TYPE_BURST          (3UL)
#define MSG_TYPE_ECHO_BACK      (4UL)
#define MSG_TYPE_BROADCAST_REQ  (5UL)
#define MSG_TYPE_BROADCAST_MSG  (6UL)
#define MSG_TYPE_TIMEOUT_TEST   (7UL)
#define MSG_TYPE_SHARE_MEMORY   (8UL)
#define MSG_TYPE_STATS_REQ      (9UL)
#define MSG_TYPE_STATS_RESP     (10UL)

// Message IDs
#define MSG_ID_PING             (MSG_ID_USER(MSG_TYPE_PING))
#define MSG_ID_HEAVY_PAYLOAD    (SET_FEATURE(MSG_ID_USER(MSG_TYPE_HEAVY_PAYLOAD), INTERNAL_FEATURE_UPSH))
#define MSG_ID_BURST            (MSG_ID_USER(MSG_TYPE_BURST))
#define MSG_ID_ECHO_BACK        (MSG_ID_USER(MSG_TYPE_ECHO_BACK))
#define MSG_ID_BROADCAST_REQ    (MSG_ID_USER(MSG_TYPE_BROADCAST_REQ))
#define MSG_ID_BROADCAST_MSG    (MSG_ID_USER(MSG_TYPE_BROADCAST_MSG))
#define MSG_ID_TIMEOUT_TEST     (MSG_ID_USER(MSG_TYPE_TIMEOUT_TEST))
#define MSG_ID_SHARE_MEMORY     (SET_FEATURE(MSG_ID_USER(MSG_TYPE_SHARE_MEMORY), INTERNAL_FEATURE_UPSH))
#define MSG_ID_STATS_REQ        (MSG_ID_USER(MSG_TYPE_STATS_REQ))
#define MSG_ID_STATS_RESP       (MSG_ID_USER(MSG_TYPE_STATS_RESP))

// Custom status codes
#define STRESS_STATUS_PING_OK       (IPC_USER_BASE + 1)
#define STRESS_STATUS_HEAVY_OK      (IPC_USER_BASE + 2)
#define STRESS_STATUS_BURST_OK      (IPC_USER_BASE + 3)
#define STRESS_STATUS_TIMEOUT_OK    (IPC_USER_BASE + 4)
#define STRESS_STATUS_SHARE_OK      (IPC_USER_BASE + 5)

// Payload structures
typedef struct {
    uint32_t sequence;
    uint64_t timestamp;
    uint32_t client_id;
} ping_payload_t;

typedef struct {
    uint32_t total_messages;
    uint32_t total_bytes;
    uint32_t broadcasts;
    uint32_t timeouts;
    uint32_t errors;
} stats_payload_t;

const char* stress_status_string(ipc_status_t status);

#endif // STRESS_TEST_H