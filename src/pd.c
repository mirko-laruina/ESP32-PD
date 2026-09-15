
#include <string.h>
#include <stdint.h>

#include "esp_system.h"
#include "esp_flash.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd_types.h"
#include "pd_proto.h"
#include "pd_config.h"
#include "pd_rx.h"
#include "pd_tx.h"
#include "pd.h"
#include "pd_status.h"
#include "crc32.h"

QueueHandle_t pd_queue_rx_ack;
QueueHandle_t pd_queue_rx_data;
QueueHandle_t pd_queue_rx_data_log;
QueueHandle_t pd_queue_empty;

static pd_state_t state;

#define TAG "PD"

void pd_mode(pd_mode_t mode)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = 0,
        .pull_up_en = 0};

    switch (mode)
    {
    case PD_MODE_IDLE:
    {
#ifdef GPIO_CC1
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << GPIO_CC1);
        gpio_config(&io_conf);
#endif

        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << GPIO_PD);
        gpio_config(&io_conf);

        break;
    }

    case PD_MODE_SINK:
    {
#ifdef GPIO_CC1
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << GPIO_CC1);
        gpio_config(&io_conf);
#endif

        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << GPIO_PD);
        gpio_config(&io_conf);

        io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
        io_conf.pull_down_en = true;
        io_conf.pin_bit_mask = (1ULL << GPIO_CC1_IN);
        gpio_config(&io_conf);

        gpio_set_drive_capability(GPIO_PD, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(GPIO_CC1_IN, GPIO_DRIVE_CAP_0);

        gpio_set_level(GPIO_CC1_IN, 1);
        gpio_set_level(GPIO_PD, 0);

        break;
    }
    }
}

/* Function to initialize the queues and preallocate buffers */
static void pd_init_queues(void)
{
    /* Create queues */
    pd_queue_rx_ack = xQueueCreate(PD_BUFFER_COUNT, sizeof(pd_rx_ack_t));
    if (pd_queue_rx_ack == NULL)
    {
        ESP_LOGE(TAG, "Failed to create buffer_from_isr_queue");
        return;
    }

    pd_queue_rx_data = xQueueCreate(PD_BUFFER_COUNT, sizeof(pd_rx_buf_t *));
    if (pd_queue_rx_data == NULL)
    {
        ESP_LOGE(TAG, "Failed to create buffer_from_isr_queue");
        return;
    }

    pd_queue_rx_data_log = xQueueCreate(PD_BUFFER_COUNT, sizeof(pd_rx_buf_t *));
    if (pd_queue_rx_data_log == NULL)
    {
        ESP_LOGE(TAG, "Failed to create buffer_from_isr_queue");
        return;
    }

    pd_queue_empty = xQueueCreate(PD_BUFFER_COUNT, sizeof(pd_rx_buf_t *));
    if (pd_queue_empty == NULL)
    {
        ESP_LOGE(TAG, "Failed to create pd_queue_empty");
        return;
    }

    /* Preallocate buffers and push them to the pd_queue_empty */
    for (int index = 0; index < PD_BUFFER_COUNT; index++)
    {
        pd_rx_buf_t *buffer = calloc(1, sizeof(pd_rx_buf_t));
        if (buffer == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate buffer %d", index);
            break;
        }

        if (xQueueSend(pd_queue_empty, &buffer, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to add buffer %d to pd_queue_empty", index);
            free(buffer);
        }
    }
}

void pd_protocol_task(void *pvParameters)
{
    pd_rx_buf_t *rx_data;

    while (1)
    {
        if (xQueueReceive(pd_queue_rx_data, &rx_data, 100 / portTICK_PERIOD_MS) != pdTRUE)
        {
            /* call some functions periodically */
            pd_request_timer();
            if (pd_status_pps_poll_due())
            {
                pd_send_control(PD_CONTROL_GET_PPS_STATUS);
            }
            continue;
        }

        switch (rx_data->type)
        {
        case PD_BUF_TYPE_DATA:
        {
            /* if we weren't addressed (we shall not handle), stop here */
            if (rx_data->dir != PD_PACKET_RECEIVED_ACKNOWLEDGED)
            {
                goto skip;
            }

            pd_msg_header hdr;
            pd_parse_msg_header(&hdr, rx_data->payload);

            pd_msg rx_msg;
            for (int i = 0; i < hdr.num_data_objects; i++)
            {
                rx_msg.pdo[i] = BUILD_LE_UINT32(rx_data->payload, 2 + i * 4);
            }

            pd_status_on_packet(rx_data->dir, &hdr, rx_msg.pdo, hdr.num_data_objects);

            /* when no objects, then its a control message */
            if (hdr.num_data_objects == 0)
            {
                switch (hdr.message_type)
                {
                case PD_CONTROL_SOFT_RESET:
                {
                    pd_msg response = {0};

                    response.target = rx_data->target;
                    response.immediate = true;
                    response.header.num_data_objects = 0;
                    response.header.power_role = PD_DATA_ROLE_UFP;
                    response.header.spec_revision = 2;
                    response.header.data_role = PD_DATA_ROLE_UFP;
                    response.header.message_type = PD_CONTROL_ACCEPT;

                    pd_tx_enqueue(&response);
                    break;
                }

                case PD_CONTROL_GOOD_CRC:
                    if (hdr.data_role == PD_DATA_ROLE_DFP)
                    {
                        pd_tx_ack_received(hdr.message_id);
                    }
                    break;

                case PD_CONTROL_REJECT:
                    if (state.requested_object != 0)
                    {
                        state.accepted_object = 0;
                        state.requested_object = 0;
                    }
                    break;

                case PD_CONTROL_ACCEPT:
                    if (state.requested_object != 0)
                    {
                        state.accepted_object = state.requested_object;
                    }
                    break;

                case PD_CONTROL_PS_RDY:
                    break;
                default:
                    break;
                }
            }
            else
            {
                switch (hdr.message_type)
                {
                case PD_VENDOR_MESSAGE:
                {
                    pd_vdm_packet req_vdm;

                    pd_parse_vdm(&req_vdm, &rx_msg);

                    if (rx_data->target == PD_TARGET_SOP_P && req_vdm.vdm_header.command_type == PD_VDM_CMD_TYPE_REQ && req_vdm.vdm_header.command == PD_VDM_CMD_DISCOVER_IDENTIY)
                    {
#ifdef PD_TEST_EMARKER_CABLE
                        /* ToDo: this requires proper pulldown on the other CC as well */
                        pd_msg response = {0};

                        response.target = PD_TARGET_SOP_P;
                        response.header.power_role = PD_DATA_ROLE_CABLE;
                        response.header.spec_revision = 1;
                        response.header.message_type = PD_VENDOR_MESSAGE;

                        pd_vdm_packet resp_vdm = {
                            .vdm_header = {
                                .svid = PD_VDM_SID_PD,
                                .vdm_type = 1,
                                .vdm_version_major = 1,
                                .vdm_version_minor = 0,
                                .object_position = 0,
                                .command_type = PD_VDM_CMD_TYPE_ACK,
                                .command = PD_VDM_CMD_DISCOVER_IDENTIY,
                            },
                            .id_header = {.usb_host = 0, .usb_device = 0, .sop_product_type = 3, .modal_operation = 0, .usb_vendor_id = 0xDEAD},
                            .crt_stat.usb_if_xid = 0,
                            .product.bcd_device = 0xBEEF,
                            .product.usb_product_id = 0xDEAD,
                            .cable_1 = {.hw_version = 1, .fw_version = 2, .vdo_version = 0, .plug_type = 2, .epr_capable = 1, .cable_latency = 1, .cable_termination = 0, .max_vbus_voltage = 3, .sbu_supported = 0, .sbu_type = 0, .vbus_current = 2, .vbus_through = 1, .usb_speed = 4},
                        };

                        pd_build_vdm(&resp_vdm, &response);

#if 0
                        /* hardcode to fake an authentic cable */
                        response.pdo[0] = 0xFF008041;
                        response.pdo[1] = 0x18000000;
                        response.pdo[2] = 0x00000000;
                        response.pdo[3] = 0x00000000;
                        response.pdo[4] = 0x00082052;
#endif

                        response.header.num_data_objects = 5;

                        pd_tx_enqueue(&response);
#endif
                    }

                    break;
                }
                break;

                case PD_DATA_SOURCE_CAPABILITIES:
                {
                    state.requested_object = 0;

                    for (uint32_t index = 0; !state.requested_object && index < hdr.num_data_objects; index++)
                    {
                        uint32_t pdo_value = rx_msg.pdo[index];
                        uint32_t type = (pdo_value >> 30) & 0x03;

                        switch (type)
                        {
                        case 0:
                        {
                            uint32_t voltage = ((pdo_value >> 10) & 0x3FF) * 50; /* in 50mV units */
                            uint32_t current = (pdo_value & 0x3FF) * 10;         /* in 10mA units */

                            if (voltage == state.request_voltage_mv && current >= state.request_current_ma)
                            {
                                state.requested_object = index + 1;
                                state.requested_pps = false;
                            }
                            break;
                        }

                        case 3:
                        {
                            uint32_t subtype = (pdo_value >> 28) & 0x03;

                            switch (subtype)
                            {
                            case 0:
                            {
                                uint32_t max_voltage = ((pdo_value >> 17) & 0xFF) * 100; /* in 100mV units */
                                uint32_t min_voltage = ((pdo_value >> 8) & 0xFF) * 100;  /* in 100mV units */
                                uint32_t max_current = ((pdo_value >> 0) & 0x7F) * 50;   /* in 50mA units */

                                if (min_voltage <= state.request_voltage_mv && max_voltage >= state.request_voltage_mv && max_current >= state.request_current_ma)
                                {
                                    state.requested_object = index + 1;
                                    state.requested_pps = true;
                                }

                                break;
                            }
                            default:
                                break;
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    }

                    if (!state.requested_object)
                    {
                        state.requested_object = 1;
                    }
                    pd_refresh_request(true);

                    break;
                }
                default:
                    break;
                }
            }

            break;
        }
        case PD_BUF_TYPE_SYMBOLS:
        {
            if (rx_data->target == PD_TARGET_HARD_RESET)
            {
                pd_state_reset();
                pd_status_on_reset();
            }
            else if (rx_data->target == PD_TARGET_CABLE_RESET)
            {
                pd_state_reset();
                pd_status_on_reset();
            }
            break;
        }
        default:
        {
            break;
        }
        }

    skip:

        /* Return the buffer to the log queue */
        if (xQueueSend(pd_queue_rx_data_log, &rx_data, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to return buffer to pd_queue_empty");
            free(rx_data);
        }
    }
}

void pd_refresh_request(bool immediate)
{
    if (state.requested_pps)
    {
        pd_request_pps(state.requested_object, state.request_voltage_mv, state.request_current_ma, immediate);
    }
    else
    {
        pd_request(state.requested_object, state.request_current_ma, immediate);
    }
    state.request_last_timestamp = esp_timer_get_time();
}

void pd_request_object(bool pps, uint8_t object, uint32_t voltage_mv, uint32_t current_ma)
{
    state.requested_pps = pps;
    state.requested_object = object;
    state.request_voltage_mv = voltage_mv;
    state.request_current_ma = current_ma;
    pd_refresh_request(true);
}

void pd_vdm(int command, int mode)
{
    pd_msg response = {0};

    response.target = PD_TARGET_SOP;
    response.immediate = false;
    response.header.num_data_objects = 0;
    response.header.power_role = PD_DATA_ROLE_UFP;
    response.header.spec_revision = 2;
    response.header.data_role = PD_DATA_ROLE_UFP;
    response.header.message_type = PD_VENDOR_MESSAGE;

    pd_tx_enqueue(&response);
}

void pd_send_control(pd_message_type_t message_id)
{
    pd_msg response = {0};

    response.target = PD_TARGET_SOP;
    response.immediate = false;
    response.header.num_data_objects = 0;
    response.header.power_role = PD_DATA_ROLE_UFP;
    response.header.spec_revision = 2;
    response.header.data_role = PD_DATA_ROLE_UFP;
    response.header.message_type = message_id;

    pd_tx_enqueue(&response);
}

void pd_request(uint8_t object, uint32_t current_ma, bool immediate)
{
    pd_msg response = {0};

    response.target = PD_TARGET_SOP;
    response.immediate = immediate;
    response.header.num_data_objects = 1;
    response.header.power_role = PD_DATA_ROLE_UFP;
    response.header.spec_revision = 2;
    response.header.data_role = PD_DATA_ROLE_UFP;
    response.header.message_type = PD_DATA_REQUEST;

    response.pdo[0] |= (object) << 28;
    response.pdo[0] |= (current_ma / 10) << 10;
    response.pdo[0] |= (current_ma / 10) << 0;

    pd_tx_enqueue(&response);
}

void pd_request_pps(uint8_t object, uint32_t voltage_mv, uint32_t current_ma, bool immediate)
{
    pd_msg response = {0};

    response.target = PD_TARGET_SOP;
    response.immediate = immediate;
    response.header.num_data_objects = 1;
    response.header.power_role = PD_DATA_ROLE_UFP;
    response.header.spec_revision = 2;
    response.header.data_role = PD_DATA_ROLE_UFP;
    response.header.message_type = PD_DATA_REQUEST;

    response.pdo[0] |= (object) << 28;
    response.pdo[0] |= (voltage_mv / 20) << 9;
    response.pdo[0] |= (current_ma / 50) << 0;

    pd_tx_enqueue(&response);
}

/* re-request the same object again periodically */
void pd_request_timer()
{
    if (!state.requested_object || !state.accepted_object)
    {
        return;
    }

    if (state.requested_object != state.accepted_object)
    {
        return;
    }

    uint64_t timestamp = esp_timer_get_time();
    if (timestamp - state.request_last_timestamp > (PD_REQUEST_REFRESH_MS) * 1000)
    {
        pd_refresh_request(false);
    }
}

void pd_log_task(void *pvParameters)
{
    pd_rx_buf_t *rx_data;

    while (1)
    {
        if (xQueueReceive(pd_queue_rx_data_log, &rx_data, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        ESP_LOGI(TAG, "");

        switch (rx_data->type)
        {
        case PD_BUF_TYPE_DATA:
        {
            switch (rx_data->target)
            {
            case PD_TARGET_SOP:
                ESP_LOGI(TAG, "Target: SOP");
                break;
            case PD_TARGET_SOP_P:
                ESP_LOGI(TAG, "Target: SOP'");
                break;
            case PD_TARGET_SOP_PP:
                ESP_LOGI(TAG, "Target: SOP''");
                break;
            case PD_TARGET_SOP_PD:
                ESP_LOGI(TAG, "Target: SOP' Debug");
                break;
            case PD_TARGET_SOP_PPD:
                ESP_LOGI(TAG, "Target: SOP'' Debug");
                break;
            case PD_TARGET_HARD_RESET:
                ESP_LOGI(TAG, "Target: Hard Reset");
                break;
            case PD_TARGET_CABLE_RESET:
                ESP_LOGI(TAG, "Target: Cable Reset");
                break;
            default:
                ESP_LOGI(TAG, "Target: Unknown (0x%02" PRIX8 ")", rx_data->target);
                break;
            }

            switch (rx_data->dir)
            {
            case PD_PACKET_RECEIVED:
                break;
            case PD_PACKET_RECEIVED_ACKNOWLEDGED:
                ESP_LOGW(TAG, "  Acknowledged");
                break;
            case PD_PACKET_SENT:
                ESP_LOGE(TAG, "  Sent, but no ACK");
                break;
            case PD_PACKET_SENT_ACKNOWLEDGED:
                ESP_LOGW(TAG, "  Sent");
                break;
            }

            pd_msg_header hdr;
            pd_parse_msg_header(&hdr, rx_data->payload);

            pd_msg rx_msg;
            for (int i = 0; i < hdr.num_data_objects; i++)
            {
                rx_msg.pdo[i] = BUILD_LE_UINT32(rx_data->payload, 2 + i * 4);
            }

            /* Print the fields */

            ESP_LOGI(TAG, "  Header Fields%s", (hdr.extended) ? " (extended)" : "");

            ESP_LOGI(TAG, "    DO: %" PRIu8 ", ID: %" PRIu8 ", PPR/CP: %" PRIu8 ", Rev: %" PRIu8 ", PDR: %" PRIu8 ", Type: %" PRIu8 "",
                     hdr.num_data_objects, hdr.message_id, hdr.power_role, hdr.spec_revision, hdr.data_role, hdr.message_type);

            /* when no objects, then its a control message */
            if (hdr.num_data_objects == 0)
            {
                ESP_LOGI(TAG, "  Control:");
                switch (hdr.message_type)
                {
                case PD_CONTROL_SOFT_RESET:
                    ESP_LOGI(TAG, "    Soft Reset");
                    break;

                case PD_CONTROL_GOOD_CRC:
                    ESP_LOGI(TAG, "    Good CRC");
                    break;

                case PD_CONTROL_REJECT:
                    ESP_LOGI(TAG, "    Rejected");
                    break;

                case PD_CONTROL_ACCEPT:
                    ESP_LOGI(TAG, "    Accepted");
                    break;

                case PD_CONTROL_PS_RDY:
                    ESP_LOGI(TAG, "    Power supply ready");
                    break;

                default:
                    ESP_LOGI(TAG, "    Not implemented yet: 0x%02" PRIx8, hdr.message_type);
                    break;
                }
            }
            else
            {
                ESP_LOGI(TAG, "  Data:");
                switch (hdr.message_type)
                {
                case PD_VENDOR_MESSAGE:
                {
                    for (uint32_t index = 0; index < hdr.num_data_objects; index++)
                    {
                        ESP_LOGI(TAG, "      Data #%" PRIu32 ": 0x%08" PRIx32, index, rx_msg.pdo[index]);
                    }

                    pd_vdm_packet vdm;
                    pd_parse_vdm(&vdm, &rx_msg);
                    pd_dump_vdm(&vdm);
                    break;
                }

                case PD_DATA_REQUEST:
                {
                    ESP_LOGI(TAG, "    Request");
                    uint8_t object = ((rx_msg.pdo[0] >> 28) & 0x1F) - 1;
                    uint32_t operating_current = ((rx_msg.pdo[0] >> 10) & 0x3FF) * 10;
                    uint32_t max_operating_current = ((rx_msg.pdo[0] >> 0) & 0x3FF) * 10;
                    ESP_LOGI(TAG, "      Object #%" PRIu8 " (ToDo: dumping in fixed format, will not match when it's PPS)", object);
                    ESP_LOGI(TAG, "      Current     %" PRIu32 "mA", operating_current);
                    ESP_LOGI(TAG, "      Current Max %" PRIu32 "mA", max_operating_current);
                    break;
                }

                case PD_DATA_SOURCE_CAPABILITIES:
                {
                    ESP_LOGI(TAG, "    Source Capabilities:");
                    for (uint32_t index = 0; index < hdr.num_data_objects; index++)
                    {
                        uint32_t pdo_value = rx_msg.pdo[index];
                        uint32_t type = (pdo_value >> 30) & 0x03;

                        switch (type)
                        {
                        case 0:
                        {
                            ESP_LOGI(TAG, "    #%" PRIu32 ": Fixed Supply PDO", index);
                            uint32_t voltage = (pdo_value >> 10) & 0x3FF; /* in 50mV units */
                            uint32_t current = pdo_value & 0x3FF;         /* in 10mA units */
                            uint32_t epr_capable = (pdo_value >> 23) & 0x01;
                            float watts = (voltage * 50 * current * 10) / 1000000.0;

                            ESP_LOGI(TAG, "        %" PRIu32 " mV, %" PRIu32 " mA (%2.2f W%s)",
                                     voltage * 50, current * 10, watts,
                                     epr_capable ? ", EPR" : "");

                            break;
                        }
                        case 1:
                        {
                            ESP_LOGI(TAG, "    #%" PRIu32 ": Battery Supply PDO", index);

                            uint32_t max_voltage = (pdo_value >> 20) & 0x1FF; /* in 50mV units */
                            uint32_t min_voltage = (pdo_value >> 10) & 0x1FF; /* in 50mV units */
                            uint32_t max_power = (pdo_value >> 0) & 0x1FF;    /* in 250mW units */

                            ESP_LOGI(TAG, "        %" PRIu32 " mV - %" PRIu32 " mV, %" PRIu32 " mW",
                                     min_voltage * 50, max_voltage * 50, max_power * 250);
                            break;
                        }
                        case 2:
                        {
                            ESP_LOGI(TAG, "    #%" PRIu32 ": Variable Supply (non-battery) PDO", index);

                            uint32_t max_voltage = (pdo_value >> 20) & 0x1FF; /* in 50mV units */
                            uint32_t min_voltage = (pdo_value >> 10) & 0x1FF; /* in 50mV units */
                            uint32_t max_power = (pdo_value >> 0) & 0x1FF;    /* in 250mW units */

                            ESP_LOGI(TAG, "        %" PRIu32 " mV - %" PRIu32 " mV, %" PRIu32 " mW",
                                     min_voltage * 50, max_voltage * 50, max_power * 250);
                            break;
                        }
                        case 3:
                        {
                            uint32_t subtype = (pdo_value >> 28) & 0x03;

                            switch (subtype)
                            {
                            case 0:
                            {
                                ESP_LOGI(TAG, "    #%" PRIu32 ": Augmented PDO (SPR PPS)", index);
                                bool limited = (pdo_value >> 27) & 0x01;
                                uint32_t max_voltage = (pdo_value >> 17) & 0xFF; /* in 100mV units */
                                uint32_t min_voltage = (pdo_value >> 8) & 0xFF;  /* in 100mV units */
                                uint32_t max_current = (pdo_value >> 0) & 0x7F;  /* in 50mA units */

                                ESP_LOGI(TAG, "        %" PRIu32 " mV - %" PRIu32 " mV, %" PRIu32 " mA (%s)",
                                         min_voltage * 100, max_voltage * 100, max_current * 50, limited ? "Limited" : "Unlimited");
                                break;
                            }
                            case 1:
                            {

                                ESP_LOGI(TAG, "    #%" PRIu32 ": Augmented PDO (EPR AVS)", index);
                                uint32_t peak_current = (pdo_value >> 26) & 0x03;
                                uint32_t max_voltage = (pdo_value >> 17) & 0xFF; /* in 100mV units */
                                uint32_t min_voltage = (pdo_value >> 8) & 0xFF;  /* in 100mV units */
                                uint32_t pdp = (pdo_value >> 0) & 0x7F;          /* in 1W units */

                                ESP_LOGI(TAG, "        Peak Current: %" PRIu32 ", Voltage Range: %" PRIu32 " mV - %" PRIu32 " mV, PDP: %" PRIu32 " W",
                                         peak_current, min_voltage * 100, max_voltage * 100, pdp);
                                break;
                            }
                            case 2:
                            {

                                ESP_LOGI(TAG, "    #%" PRIu32 ": Augmented PDO (SPR AVS)", index);
                                uint32_t peak_current = (pdo_value >> 26) & 0x03;
                                uint32_t max_current_15 = (pdo_value >> 10) & 0x1FF; /* in 10mA units */
                                uint32_t max_current_20 = (pdo_value >> 0) & 0x1FF;  /* in 10mA units */

                                ESP_LOGI(TAG, "        Peak Current: %" PRIu32 ", %" PRIu32 " mA@15V, %" PRIu32 " mA@20V",
                                         peak_current, max_current_15 * 10, max_current_20 * 10);
                                break;
                            }
                            case 3:
                            {

                                ESP_LOGI(TAG, "    #%" PRIu32 ": Augmented PDO (unknown)", index);
                                break;
                            }

                            default:
                                break;
                            }
                            break;
                        }
                        }
                    }
                    break;
                }

                default:
                {
                    ESP_LOGI(TAG, "    Not implemented yet: 0x%02" PRIx8, hdr.message_type);
                    break;
                }
                }
            }
            break;
        }
        case PD_BUF_TYPE_SYMBOLS:
        {
            if (rx_data->target == PD_TARGET_HARD_RESET)
            {
                ESP_LOGE(TAG, "Reset:");
                ESP_LOGE(TAG, "  Hard Reset");
            }
            else if (rx_data->target == PD_TARGET_CABLE_RESET)
            {
                ESP_LOGE(TAG, "Reset:");
                ESP_LOGE(TAG, "  Cable Reset");
            }
            else
            {
                ESP_LOGE(TAG, "Failed packet:");
                ESP_LOGE(TAG, "  Raw symbols:");
                ESP_LOG_BUFFER_HEX(TAG, rx_data->symbols, rx_data->symbol_count);
                ESP_LOGE(TAG, "  Parsed data:");
                ESP_LOG_BUFFER_HEX(TAG, rx_data->payload, rx_data->length);
            }
            break;
        }
        case PD_BUF_TYPE_TIMINGS:
        {
            ESP_LOGI(TAG, "  Timings:");
            ESP_LOG_BUFFER_HEX(TAG, rx_data->symbols, rx_data->symbol_count);
            break;
        }
        default:
        {
            ESP_LOGI(TAG, "  Unknown buffer type");
            break;
        }
        }

        /* Return the buffer to the empty buffer queue */
        if (xQueueSend(pd_queue_empty, &rx_data, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to return buffer to pd_queue_empty");
            free(rx_data);
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Bottom half that handles RX packets from the ISR and triggers the confirmation.
 *
 * This function is very time-critical as it processes received packets and sends a confirmation
 * (GoodCRC) within less than a millisecond. After sending the confirmation, it hands over the
 * buffer to the slower user task.
 */

void IRAM_ATTR pd_rx_ack_task()
{
    pd_rx_ack_t ack;
    uint8_t buffer[32];

    while (1)
    {
        if (xQueueReceive(pd_queue_rx_ack, &ack, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        pd_msg response = {0};

        response.target = ack.target;
        response.header.message_id = ack.message_id;
        response.header.num_data_objects = 0;
        response.header.power_role = PD_DATA_ROLE_UFP;
        response.header.spec_revision = 2;
        response.header.data_role = PD_DATA_ROLE_UFP;
        response.header.message_type = PD_CONTROL_GOOD_CRC;

        size_t length = 0;
        buffer[0] = response.target;
        length += 1;

        /* construct the 16 bit header  */
        pd_build_msg_header(&response.header, &buffer[length]);
        length += 2;

        /* finally calc and append the CRC */
        uint32_t crc = crc32buf(&buffer[1], length - 1);
        SPLIT_LE_UINT32(crc, buffer, length);
        length += 4;

        pd_tx_start(buffer, length);
    }
}

void pd_state_reset()
{
    memset(&state, 0, sizeof(state));
    state.request_voltage_mv = 12345;
    state.request_current_ma = 1000;
}

void pd_init()
{
    ESP_LOGI(TAG, "  * Initialize PD");
    pd_status_init();
    pd_init_queues();
    pd_state_reset();

    /* the rx_ack task needs highes priority as it has to respons with a RMT tx within a few hundred usec */
    xTaskCreate(pd_rx_ack_task, "pd_rx_ack_task", 4096, NULL, PD_RX_ACK_TASK_PRIO, NULL);
    /* the user task turned out to also be a bit "time critical". every CAP message needs to be answered immediately */
    xTaskCreate(pd_protocol_task, "pd_protocol_task", 4096, NULL, PD_PROTOCOL_TASK_PRIO, NULL);
    xTaskCreate(pd_log_task, "pd_log_task", 4096, NULL, PD_LOG_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "  * Initialize RX");
    pd_rx_init();
    ESP_LOGI(TAG, "  * Initialize TX");
    pd_tx_init();
    ESP_LOGI(TAG, "  * Enter sink mode");
    pd_mode(PD_MODE_SINK);

    if (0)
    {
        uint8_t *buf = calloc(1, 100);
        while (1)
        {
            for (int pos = 0; pos < 100; pos++)
            {
                buf[pos] = (2 * pos) << 4 | (2 * pos + 1);
                vTaskDelay(75 / portTICK_PERIOD_MS);
                pd_tx_start(buf, 1 + pos);
            }
        }
    }

    if (0)
    {
        while (1)
        {
            for (int pos = 0; pos < 100; pos++)
            {
                vTaskDelay(10000 / portTICK_PERIOD_MS);

                uint8_t buf[64] = {0};
                int pos = 0;

                ESP_LOGI(TAG, "  Get status");
                uint16_t header_tx = pd_tx_header(0, 0, state.message_id++, 0, 2, PD_DATA_ROLE_UFP, PD_CONTROL_GET_STATUS);

                buf[pos++] = header_tx;
                buf[pos++] = header_tx >> 8;

                uint32_t crc = crc32buf(buf, pos);
                buf[pos++] = crc;
                buf[pos++] = crc >> 8;
                buf[pos++] = crc >> 16;
                buf[pos++] = crc >> 24;

                pd_tx(buf, pos);
            }
        }
    }
}
