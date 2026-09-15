#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "pd_status.h"
#include "pd_proto.h"

#define PD_STATUS_REQ_TIMEOUT_US (2500000ULL)
#define PD_STATUS_PPS_POLL_PERIOD_US (1000000ULL)
#define PD_STATUS_PPS_POLL_TIMEOUT_US (1500000ULL)

static SemaphoreHandle_t s_mutex;
static pd_status_data_t s_status;

void pd_status_init(void)
{
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutex();
        memset(&s_status, 0, sizeof(s_status));
    }
}

static const char *msg_to_string(const pd_msg_header *hdr)
{
    if (hdr->num_data_objects > 0)
    {
        switch (hdr->message_type)
        {
        case PD_DATA_SOURCE_CAPABILITIES:
            return "Source Capabilities";
        case PD_DATA_REQUEST:
            return "Request";
        case PD_DATA_SINK_CAPABILITIES:
            return "Sink Capabilities";
        case PD_VENDOR_MESSAGE:
            return "VDM";
        default:
            return "Messaggio dati";
        }
    }

    switch (hdr->message_type)
    {
    case PD_CONTROL_GOOD_CRC:
        return "GoodCRC";
    case PD_CONTROL_ACCEPT:
        return "Accept";
    case PD_CONTROL_REJECT:
        return "Reject";
    case PD_CONTROL_PS_RDY:
        return "PS_RDY";
    case PD_CONTROL_GET_SOURCE_CAP:
        return "Get_Source_Cap";
    case PD_CONTROL_SOFT_RESET:
        return "Soft Reset";
    case PD_CONTROL_WAIT:
        return "Wait";
    default:
        return "Messaggio di controllo";
    }
}

static void parse_pdos(const uint32_t *pdo, uint8_t pdo_count)
{
    s_status.pdo_count = 0;

    for (uint8_t index = 0; index < pdo_count && s_status.pdo_count < PD_STATUS_MAX_PDO; index++)
    {
        uint32_t value = pdo[index];
        uint32_t type = (value >> 30) & 0x03;
        pd_status_pdo_t *out = &s_status.pdos[s_status.pdo_count];

        if (type == 0)
        {
            out->type = PD_STATUS_PDO_FIXED;
            out->mv = ((value >> 10) & 0x3FF) * 50;
            out->ma = (value & 0x3FF) * 10;
            s_status.pdo_count++;
        }
        else if (type == 3 && ((value >> 28) & 0x03) == 0)
        {
            out->type = PD_STATUS_PDO_PPS;
            out->min_mv = ((value >> 8) & 0xFF) * 100;
            out->max_mv = ((value >> 17) & 0xFF) * 100;
            out->ma = (value & 0x7F) * 50;
            s_status.pdo_count++;
        }
        else
        {
            out->type = PD_STATUS_PDO_OTHER;
            out->mv = out->ma = out->min_mv = out->max_mv = 0;
            s_status.pdo_count++;
        }
    }
}

void pd_status_on_packet(uint8_t dir, const pd_msg_header *hdr, const uint32_t *pdo, uint8_t pdo_count)
{
    pd_status_init();

    if (hdr == NULL)
    {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool received = (dir == PD_PACKET_RECEIVED || dir == PD_PACKET_RECEIVED_ACKNOWLEDGED);

    s_status.last_rx_us = esp_timer_get_time();
    strlcpy(s_status.last_message, msg_to_string(hdr), sizeof(s_status.last_message));

    if (received)
    {
        s_status.charger_seen = true;

        if (pdo_count > 0 && hdr->message_type == PD_DATA_SOURCE_CAPABILITIES)
        {
            parse_pdos(pdo, pdo_count);
        }
        else if (pdo_count > 0 && hdr->message_type == PD_DATA_PPS_STATUS)
        {
            uint32_t value = pdo[0];
            s_status.pps_status_seen = true;
            s_status.pps_status_unsupported = false;
            s_status.pps_status_pending = false;
            s_status.pps_status_code = (value >> 18) & 0x03;
            s_status.pps_status_mv = (value & 0x1FF) * 100;
            s_status.pps_status_ma = ((value >> 10) & 0x7F) * 50;
            s_status.pps_status_us = esp_timer_get_time();
        }
        else if (pdo_count == 0)
        {
            switch (hdr->message_type)
            {
            case PD_CONTROL_NOT_SUPPORTED:
                if (s_status.pps_status_pending)
                {
                    s_status.pps_status_pending = false;
                    s_status.pps_status_unsupported = true;
                }
                break;

            case PD_CONTROL_ACCEPT:
                if (s_status.req_state == PD_STATUS_REQ_PENDING || s_status.req_state == PD_STATUS_REQ_ACCEPTED)
                {
                    s_status.req_state = PD_STATUS_REQ_ACCEPTED;
                }
                break;

            case PD_CONTROL_REJECT:
                if (s_status.req_state == PD_STATUS_REQ_PENDING || s_status.req_state == PD_STATUS_REQ_ACCEPTED)
                {
                    s_status.req_state = PD_STATUS_REQ_REJECTED;
                }
                break;

            case PD_CONTROL_PS_RDY:
                if (s_status.req_state == PD_STATUS_REQ_PENDING || s_status.req_state == PD_STATUS_REQ_ACCEPTED)
                {
                    s_status.req_state = PD_STATUS_REQ_READY;
                }
                break;

            case PD_CONTROL_SOFT_RESET:
                s_status.req_state = PD_STATUS_REQ_NONE;
                break;

            default:
                break;
            }
        }
    }

    xSemaphoreGive(s_mutex);
}

void pd_status_on_reset(void)
{
    pd_status_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.req_state = PD_STATUS_REQ_NONE;
    xSemaphoreGive(s_mutex);
}

void pd_status_begin_request(bool pps, uint8_t object, uint32_t mv, uint32_t ma)
{
    pd_status_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.req_state = PD_STATUS_REQ_PENDING;
    s_status.req_pps = pps;
    s_status.req_object = object;
    s_status.req_mv = mv;
    s_status.req_ma = ma;
    s_status.req_time_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

bool pd_status_get(pd_status_data_t *out)
{
    if (s_mutex == NULL || out == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_status.req_state == PD_STATUS_REQ_PENDING &&
        esp_timer_get_time() - s_status.req_time_us > PD_STATUS_REQ_TIMEOUT_US)
    {
        s_status.req_state = PD_STATUS_REQ_TIMEOUT;
    }

    *out = s_status;
    xSemaphoreGive(s_mutex);
    return true;
}

void pd_status_pps_poll_set(bool enabled)
{
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutex();
        memset(&s_status, 0, sizeof(s_status));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.pps_poll_enabled = enabled;
    if (!enabled)
    {
        s_status.pps_status_pending = false;
    }
    xSemaphoreGive(s_mutex);
}

bool pd_status_pps_poll_due(void)
{
    bool due = false;

    pd_status_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint64_t now = esp_timer_get_time();

    if (s_status.pps_status_pending &&
        now - s_status.pps_poll_last_us > PD_STATUS_PPS_POLL_TIMEOUT_US)
    {
        s_status.pps_status_pending = false;
        s_status.pps_status_unsupported = true;
    }

    if (s_status.pps_poll_enabled &&
        s_status.req_pps &&
        s_status.req_state == PD_STATUS_REQ_READY &&
        !s_status.pps_status_pending &&
        now - s_status.pps_poll_last_us > PD_STATUS_PPS_POLL_PERIOD_US)
    {
        s_status.pps_status_pending = true;
        s_status.pps_poll_last_us = now;
        due = true;
    }

    xSemaphoreGive(s_mutex);
    return due;
}
