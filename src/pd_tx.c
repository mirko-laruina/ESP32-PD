#include <string.h>
#include <stdint.h>

#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"

/*
 * This section is needed for reconfiguring the Tx GPIO just before transmission to output mode
 * and then back to input mode after transmission. This is necessary to prevent interference with
 * other transmissions. Reconfiguring using new Rx RMT channels each time would be too time-consuming.
 */
#include "driver/rmt_types.h"                      // Include RMT driver types
#include "../src/rmt_private.h"                    // Include private RMT definitions
#include "soc/rmt_periph.h"                        // Required for accessing rmt_signal_conn_t and rmt_periph_signals.
extern const rmt_signal_conn_t rmt_periph_signals; // External declaration of RMT peripheral signals

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd_config.h"
#include "pd_coding.h"
#include "pd_proto.h"
#include "pd_rx.h"
#include "pd_tx.h"
#include "pd.h"
#include "crc32.h"

#define TAG "PD-TX"

#define PD_TX_FREQ (10000000)
#define PD_TX_SYMS (64)
#define PD_TX_SHORT_DURATION (16) /* 1.6us */

static rmt_channel_handle_t pd_tx_chan;
static rmt_encoder_handle_t pd_tx_encoder;
static QueueHandle_t pd_queue_tx;
static QueueHandle_t pd_queue_tx_acks;

extern QueueHandle_t pd_queue_rx_data_log;
extern QueueHandle_t pd_queue_empty;

static volatile bool pd_tx_ongoing_flag = false;
static uint8_t pd_tx_message_id = 0;

bool IRAM_ATTR pd_tx_ongoing()
{
    return pd_tx_ongoing_flag;
}

void IRAM_ATTR pd_tx_active()
{
#ifdef GPIO_CC1
    gpio_set_drive_capability(GPIO_CC1, GPIO_DRIVE_CAP_0);
#endif
    gpio_set_drive_capability(GPIO_TX, GPIO_DRIVE_CAP_0);
    esp_rom_gpio_connect_out_signal(
        GPIO_TX,
        rmt_periph_signals.groups[pd_tx_chan->group->group_id].channels[pd_tx_chan->channel_id + RMT_TX_CHANNEL_OFFSET_IN_GROUP].tx_sig,
        true, false);
#ifdef GPIO_CC1
    gpio_set_direction(GPIO_CC1, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_CC1, 0);
    /* we now have configured the GPIO_CC1 pin and GPIO_TX pin to drive against each other.
    with these drive capabilities, we get a voltage divider to approx 1.7V, which is closer to
    the expected 1.1V on the PD lines */
#else
    /* we now have a weak TX pin driving 3.3V on the PD bus line. also works, its just far from specs. */
#endif
}

void IRAM_ATTR pd_tx_inactive()
{
    /* now configure the TX pin back to an input so communication isn't blocked. order is chosen that there are less spikes. */
#ifdef GPIO_CC1
    gpio_set_direction(GPIO_CC1, GPIO_MODE_INPUT);
#endif
    gpio_set_direction(GPIO_TX, GPIO_MODE_INPUT);
#ifdef GPIO_CC1
    gpio_set_pull_mode(GPIO_CC1, GPIO_FLOATING);
#endif
    gpio_set_pull_mode(GPIO_TX, GPIO_FLOATING);
}

static IRAM_ATTR void add_bit(pd_tx_ctx_t *ctx, rmt_symbol_word_t *symbols, size_t *symbols_used, bool bit, uint32_t short_duration)
{
    /*
     * In this encoding scheme:
     * - A 0-bit creates two equally long pulses of the same level.
     * - A 1-bit creates an alternating bit pair.
     */
    symbols[*symbols_used].level0 = ctx->level;
    symbols[*symbols_used].duration0 = short_duration;
    symbols[*symbols_used].duration1 = short_duration;
    if (!bit)
    {
        ctx->level = !ctx->level;
    }
    symbols[*symbols_used].level1 = !ctx->level;

    (*symbols_used)++;
}

static IRAM_ATTR void add_half(pd_tx_ctx_t *ctx, rmt_symbol_word_t *symbols, size_t *symbols_used, uint8_t half)
{
    for (int pos = 0; pos < 5; pos++)
    {
        add_bit(ctx, symbols, symbols_used, half & 1, PD_TX_SHORT_DURATION);
        half >>= 1;
    }
}

static IRAM_ATTR size_t pd_tx_enc_cbr(const void *data, size_t data_size,
                                      size_t symbols_written, size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    size_t symbols_used = 0;
    pd_tx_ctx_t *ctx = (pd_tx_ctx_t *)arg;

    if (symbols_written == 0)
    {
        /* reset all states */
        memset(ctx, 0, sizeof(pd_tx_ctx_t));
        ctx->state = PD_TX_PATTERN;
        ctx->level = true;
        pd_tx_ongoing_flag = true;
    }

    /* We need a minimum of 10 symbol spaces per byte */
    bool loop = true;
    while (loop)
    {
        if (symbols_used >= symbols_free)
        {
            break;
        }

        switch (ctx->state)
        {
        case PD_TX_PATTERN:
        {
            if (ctx->sync_bits >= 64)
            {
                ctx->state = PD_TX_SYNC;
                break;
            }
            add_bit(ctx, symbols, &symbols_used, ctx->sync_bits & 1, PD_TX_SHORT_DURATION);
            ctx->sync_bits++;
            break;
        }

        case PD_TX_SYNC:
        {
            if (ctx->sync_symbols >= 4)
            {
                ctx->data_pos = 1;
                ctx->state = PD_TX_DATA;
                break;
            }
            uint32_t sync_symbol = 0;
            uint8_t sync_symbols[4];

            switch ((pd_rx_target_t)((uint8_t *)data)[0])
            {
            case PD_TARGET_SOP:
                sync_symbol = TARGET_SOP;
                break;
            case PD_TARGET_SOP_P:
                sync_symbol = TARGET_SOP_P;
                break;
            case PD_TARGET_SOP_PP:
                sync_symbol = TARGET_SOP_PP;
                break;
            case PD_TARGET_SOP_PD:
                sync_symbol = TARGET_SOP_PD;
                break;
            case PD_TARGET_SOP_PPD:
                sync_symbol = TARGET_SOP_PPD;
                break;
            case PD_TARGET_HARD_RESET:
                sync_symbol = TARGET_HARD_RESET;
                break;
            case PD_TARGET_CABLE_RESET:
                sync_symbol = TARGET_CABLE_RESET;
                break;
            }
            SPLIT_LE_UINT32(sync_symbol, sync_symbols, 0);

            add_half(ctx, symbols, &symbols_used, line_code_encode[sync_symbols[ctx->sync_symbols]]);
            ctx->sync_symbols++;
            break;
        }

        case PD_TX_DATA:
        {
            if (ctx->data_pos >= data_size)
            {
                ctx->state = PD_TX_EOP;
                break;
            }
            if ((symbols_used + 10) > symbols_free)
            {
                loop = false;
                break;
            }

            /* if all bytes transmitted, finish */
            uint8_t data_byte = ((uint8_t *)data)[ctx->data_pos];
            uint8_t half_lower = line_code_encode[data_byte & 0x0F];
            uint8_t half_upper = line_code_encode[data_byte >> 4];

            add_half(ctx, symbols, &symbols_used, half_lower);
            add_half(ctx, symbols, &symbols_used, half_upper);

            ctx->data_pos++;
            break;
        }

        case PD_TX_EOP:
        {
            if (ctx->eop_symbols)
            {
                ctx->state = PD_TX_DONE;
                break;
            }
            add_half(ctx, symbols, &symbols_used, line_code_encode[EOP]);
            /* create a longer final edge */
            add_bit(ctx, symbols, &symbols_used, 0, 2 * PD_TX_SHORT_DURATION);
            ctx->eop_symbols++;
            break;
        }

        case PD_TX_DONE:
        {
            /* not documented good enough IMO - only set done when no symbols got enqueued */
            loop = false;
            *done = !symbols_used;
            break;
        }
        }
    }

    if (symbols_written == 0)
    {
        /* first transmission starts now */
        pd_tx_active();
    }

    return symbols_used;
}

bool IRAM_ATTR pd_tx_done_cbr(rmt_channel_handle_t tx_chan, const rmt_tx_done_event_data_t *edata, void *user_ctx)
{
    pd_tx_inactive();
    pd_tx_ongoing_flag = false;
    return false;
}

void IRAM_ATTR pd_tx_start(const uint8_t *data, size_t length)
{
    rmt_transmit_config_t config = {
        .loop_count = 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(pd_tx_chan, pd_tx_encoder, data, length, &config));
}

void IRAM_ATTR pd_tx_task(void *pvParameters)
{
    uint8_t buffer[1 + 2 + 7 * 4 + 4];
    pd_msg *msg;

    while (1)
    {
        if (xQueueReceive(pd_queue_tx, &msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        msg->header.message_id = pd_tx_message_id;
        if (!msg->target)
        {
            msg->target = PD_TARGET_SOP;
        }

        /* we can only handle normal messages with header, pdos and crc */
        size_t length = 0;

        buffer[0] = msg->target;
        length += 1;

        /* construct the 16 bit header  */
        pd_build_msg_header(&msg->header, &buffer[length]);
        length += 2;

        /* append all PDOs*/
        for (int pdo = 0; pdo < msg->header.num_data_objects; pdo++)
        {
            SPLIT_LE_UINT32(msg->pdo[pdo], buffer, length);
            length += 4;
        }

        /* finally calc and append the CRC */
        uint32_t crc = crc32buf(&buffer[1], length - 1);
        SPLIT_LE_UINT32(crc, buffer, length);
        length += 4;

        /* now retry three times to send the message and get an ACK for it */
        bool ack = false;
        uint32_t retries = 1;
        do
        {
            /* transmit our buffer */
            if (!msg->immediate)
            {
                while (pd_rx_ongoing() || pd_tx_ongoing())
                {
                    vPortYield();
                }
            }

            pd_tx_start(buffer, length);

            /* wait for the RX path to detect an ack */
            uint32_t ack_id = 0;
            while (xQueueReceive(pd_queue_tx_acks, &ack_id, 10 / portTICK_PERIOD_MS) == pdTRUE)
            {
                ack = (ack_id == pd_tx_message_id);
            }

#ifdef PD_LOG_TX_PACKETS
            /* log the message */
            pd_rx_buf_t *rx_data;
            if (xQueueReceive(pd_queue_empty, &rx_data, 0))
            {
                memset(rx_data, 0x00, sizeof(pd_rx_buf_t));

                rx_data->type = PD_BUF_TYPE_DATA;
                rx_data->target = msg->target;
                rx_data->dir = ack ? PD_PACKET_SENT_ACKNOWLEDGED : PD_PACKET_SENT;

                rx_data->length = length - 1;
                memcpy(rx_data->payload, &buffer[1], rx_data->length);

                if (xQueueSend(pd_queue_rx_data_log, &rx_data, 0) != pdTRUE)
                {
                    ESP_LOGE(TAG, "Failed to return buffer to pd_queue_empty");
                    free(rx_data);
                }
            }
#endif
        } while (retries-- && !ack);

        if (ack)
        {
            pd_tx_message_id = (pd_tx_message_id + 1) & MESSAGE_ID_MASK;
        }

        if (msg->cbr)
        {
            msg->cbr(msg, ack);
        }

        free(msg);
    }
}

void pd_tx_init()
{
    const rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = PD_TX_FREQ,
        .mem_block_symbols = PD_TX_SYMS,
        .gpio_num = GPIO_TX,
        .trans_queue_depth = 10,
        .flags.io_loop_back = true,
        .flags.invert_out = false,
        .flags.with_dma = false,
        .flags.io_od_mode = false,
    };

    const rmt_tx_event_callbacks_t cbr = {
        .on_trans_done = &pd_tx_done_cbr,
    };

    const rmt_simple_encoder_config_t encoder_cfg = {
        .callback = pd_tx_enc_cbr,
        .min_chunk_size = 10,
        .arg = calloc(1, sizeof(pd_tx_ctx_t)),
    };

    ESP_LOGI(TAG, "  Init TX channel");
    ESP_LOGI(TAG, "    Register channel");
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &pd_tx_chan));
    ESP_LOGI(TAG, "    Register callback");
    ESP_ERROR_CHECK(rmt_tx_register_event_callbacks(pd_tx_chan, &cbr, NULL));
    ESP_LOGI(TAG, "    Create encoder");
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_cfg, &pd_tx_encoder));
    ESP_LOGI(TAG, "    Enable channel");
    ESP_ERROR_CHECK(rmt_enable(pd_tx_chan));

    gpio_set_direction(GPIO_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_TX, GPIO_FLOATING);

    pd_queue_tx = xQueueCreate(PD_BUFFER_COUNT, sizeof(pd_msg *));
    if (pd_queue_tx == NULL)
    {
        ESP_LOGE(TAG, "Failed to create pd_queue_tx");
        return;
    }

    pd_queue_tx_acks = xQueueCreate(PD_BUFFER_COUNT, sizeof(uint32_t));
    if (pd_queue_tx_acks == NULL)
    {
        ESP_LOGE(TAG, "Failed to create pd_queue_tx_acks");
        return;
    }

    xTaskCreate(pd_tx_task, "pd_tx_task", 4096, NULL, PD_TX_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "    Done");
}

uint16_t IRAM_ATTR pd_tx_header(
    uint8_t extended, uint8_t num_data_objects, uint8_t message_id,
    uint8_t power_role, uint8_t spec_revision, uint8_t data_role,
    uint8_t message_type)
{
    uint16_t header = 0;

    /* Construct the 16-bit header */
    header |= (extended & 0x01) << 15;
    header |= (num_data_objects & 0x07) << 12;
    header |= (message_id & 0x07) << 9;
    header |= (power_role & 0x01) << 8;
    header |= (spec_revision & 0x03) << 6;
    header |= (data_role & 0x01) << 5;
    header |= (message_type & 0x1F);

    return header;
}

void pd_tx_enqueue(pd_msg *msg)
{
    pd_msg *copy = malloc(sizeof(pd_msg));
    memcpy(copy, msg, sizeof(pd_msg));

    /* Return the buffer to the empty buffer queue */
    if (xQueueSend(pd_queue_tx, &copy, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to send buffer to pd_queue_tx");
        free(copy);
    }
    vPortYield();
}

void pd_tx_ack_received(uint32_t msg_id)
{
    /* Return the buffer to the empty buffer queue */
    if (xQueueSend(pd_queue_tx_acks, &msg_id, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to send buffer to pd_queue_tx_acks");
    }
}
