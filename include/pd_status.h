#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pd_types.h"
#include "pd_proto.h"

#define PD_STATUS_MAX_PDO 8

typedef enum
{
    PD_STATUS_PDO_OTHER = 0,
    PD_STATUS_PDO_FIXED,
    PD_STATUS_PDO_PPS,
} pd_status_pdo_type_t;

typedef struct
{
    pd_status_pdo_type_t type;
    uint32_t mv;
    uint32_t ma;
    uint32_t min_mv;
    uint32_t max_mv;
} pd_status_pdo_t;

typedef enum
{
    PD_STATUS_REQ_NONE = 0,
    PD_STATUS_REQ_PENDING,
    PD_STATUS_REQ_ACCEPTED,
    PD_STATUS_REQ_REJECTED,
    PD_STATUS_REQ_READY,
    PD_STATUS_REQ_TIMEOUT,
} pd_status_req_state_t;

typedef struct
{
    bool charger_seen;
    uint64_t last_rx_us;
    char last_message[48];
    uint8_t pdo_count;
    pd_status_pdo_t pdos[PD_STATUS_MAX_PDO];

    pd_status_req_state_t req_state;
    bool req_pps;
    uint8_t req_object;
    uint32_t req_mv;
    uint32_t req_ma;
    uint64_t req_time_us;

    bool pps_poll_enabled;
    bool pps_status_seen;
    bool pps_status_unsupported;
    bool pps_status_pending;
    uint8_t pps_status_code;
    uint32_t pps_status_mv;
    uint32_t pps_status_ma;
    uint64_t pps_status_us;
    uint64_t pps_poll_last_us;
} pd_status_data_t;

void pd_status_init(void);

void pd_status_on_packet(uint8_t dir, const pd_msg_header *hdr, const uint32_t *pdo, uint8_t pdo_count);

void pd_status_on_reset(void);
void pd_status_begin_request(bool pps, uint8_t object, uint32_t mv, uint32_t ma);
bool pd_status_get(pd_status_data_t *out);
void pd_status_pps_poll_set(bool enabled);
bool pd_status_pps_poll_due(void);
