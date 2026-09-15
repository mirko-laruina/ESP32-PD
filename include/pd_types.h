#pragma once

#include <stdint.h>

#define BUILD_LE_UINT16(arr, idx) ((arr[(idx)]) | ((arr[(idx) + 1]) << 8))
#define BUILD_LE_UINT32(arr, idx) ((arr[(idx)]) | ((arr[(idx) + 1]) << 8) | ((arr[(idx) + 2]) << 16) | ((arr[(idx) + 3]) << 24))

#define SPLIT_LE_UINT16(val, arr, idx)        \
    do                                        \
    {                                         \
        arr[(idx)] = (val) & 0xFF;            \
        arr[(idx) + 1] = ((val) >> 8) & 0xFF; \
    } while (0)

#define SPLIT_LE_UINT32(val, arr, idx)         \
    do                                         \
    {                                          \
        arr[(idx)] = (val) & 0xFF;             \
        arr[(idx) + 1] = ((val) >> 8) & 0xFF;  \
        arr[(idx) + 2] = ((val) >> 16) & 0xFF; \
        arr[(idx) + 3] = ((val) >> 24) & 0xFF; \
    } while (0)

/* Enum for special 4b5b symbols */
typedef enum
{
    HEX_0 = 0x0,
    HEX_1 = 0x1,
    HEX_2 = 0x2,
    HEX_3 = 0x3,
    HEX_4 = 0x4,
    HEX_5 = 0x5,
    HEX_6 = 0x6,
    HEX_7 = 0x7,
    HEX_8 = 0x8,
    HEX_9 = 0x9,
    HEX_A = 0xA,
    HEX_B = 0xB,
    HEX_C = 0xC,
    HEX_D = 0xD,
    HEX_E = 0xE,
    HEX_F = 0xF,
    SYNC_1 = 0x10,
    SYNC_2 = 0x11,
    RST_1 = 0x12,
    RST_2 = 0x13,
    EOP = 0x14,
    SYNC_3 = 0x15,
    ERROR = 0xFF
} line_code_symbol_t;

typedef enum
{
    PD_BUF_TYPE_INVALID = 0,
    PD_BUF_TYPE_TIMINGS,
    PD_BUF_TYPE_SYMBOLS,
    PD_BUF_TYPE_DATA,
} buf_type_t;

typedef enum
{
    PD_TARGET_SOP,
    PD_TARGET_SOP_P,
    PD_TARGET_SOP_PP,
    PD_TARGET_SOP_PD,
    PD_TARGET_SOP_PPD,
    PD_TARGET_HARD_RESET,
    PD_TARGET_CABLE_RESET,
} pd_rx_target_t;

#define TARGET_SOP (SYNC_1 | (SYNC_1 << 8) | (SYNC_1 << 16) | (SYNC_2 << 24))
#define TARGET_SOP_P (SYNC_1 | (SYNC_1 << 8) | (SYNC_3 << 16) | (SYNC_3 << 24))
#define TARGET_SOP_PP (SYNC_1 | (SYNC_3 << 8) | (SYNC_1 << 16) | (SYNC_3 << 24))
#define TARGET_SOP_PD (SYNC_1 | (RST_2 << 8) | (RST_2 << 16) | (SYNC_3 << 24))
#define TARGET_SOP_PPD (SYNC_1 | (RST_2 << 8) | (SYNC_3 << 16) | (SYNC_2 << 24))
#define TARGET_HARD_RESET (RST_1 | (RST_1 << 8) | (RST_1 << 16) | (RST_2 << 24))
#define TARGET_CABLE_RESET (RST_1 | (SYNC_1 << 8) | (RST_1 << 16) | (SYNC_3 << 24))

typedef enum
{
    PD_RX_INIT = 0,
    PD_RX_PREAMBLE,
    PD_RX_SOP,
    PD_RX_PAYLOAD,
    PD_RX_FINISHED
} pd_rx_state_t;

typedef enum
{
    PD_TX_PATTERN,
    PD_TX_SYNC,
    PD_TX_DATA,
    PD_TX_EOP,
    PD_TX_DONE
} pd_tx_state_t;

typedef enum
{
    PD_PACKET_RECEIVED,
    PD_PACKET_RECEIVED_ACKNOWLEDGED,
    PD_PACKET_SENT,
    PD_PACKET_SENT_ACKNOWLEDGED
} pd_packet_dir_t;

typedef struct
{
    pd_rx_target_t target;
    uint8_t message_id;
} pd_rx_ack_t;

typedef struct
{
    /* - PD working variables - */
    pd_rx_state_t state;

    /* - BMC working variables - */
    uint32_t bit_count;
    uint32_t bit_data;
    bool short_pulse;
    int last_shortened;
    /* we have 2 bytes for header, 7*4 for payload, 4 for checksum and one EOP symbol */
    int symbol_count;
    uint8_t symbols[/*2 * (2 + 7 * 4 + 4) + 1 */ 256];

    /* - parsed data for next layer - */
    pd_packet_dir_t dir;
    pd_rx_target_t target;
    buf_type_t type;
    uint8_t length;
    uint8_t payload[/*2 + 7 * 4 + 4 */ 256];

    uint64_t start_time;
} pd_rx_buf_t;

typedef struct
{
    bool level;
    uint8_t data_pos;
    uint8_t sync_bits;
    uint8_t sync_symbols;
    uint8_t eop_symbols;
    pd_tx_state_t state;
} pd_tx_ctx_t;

typedef struct
{
    bool connected;
    uint8_t message_id;
    uint8_t requested_object;
    bool requested_pps;
    uint8_t accepted_object;
    uint32_t request_voltage_mv;
    uint32_t request_current_ma;
    uint64_t request_last_timestamp;
} pd_state_t;

typedef enum
{
    PD_CONTROL_RESERVED = 0x00,
    PD_CONTROL_GOOD_CRC = 0x01,
    PD_CONTROL_GOTO_MIN = 0x02, /* Deprecated */
    PD_CONTROL_ACCEPT = 0x03,
    PD_CONTROL_REJECT = 0x04,
    PD_CONTROL_PING = 0x05, /* Deprecated */
    PD_CONTROL_PS_RDY = 0x06,
    PD_CONTROL_GET_SOURCE_CAP = 0x07,
    PD_CONTROL_GET_SINK_CAP = 0x08,
    PD_CONTROL_DR_SWAP = 0x09,
    PD_CONTROL_PR_SWAP = 0x0A,
    PD_CONTROL_VCONN_SWAP = 0x0B,
    PD_CONTROL_WAIT = 0x0C,
    PD_CONTROL_SOFT_RESET = 0x0D,
    PD_CONTROL_DATA_RESET = 0x0E,
    PD_CONTROL_DATA_RESET_COMPLETE = 0x0F,
    PD_CONTROL_NOT_SUPPORTED = 0x10,
    PD_CONTROL_GET_SOURCE_CAP_EXTENDED = 0x11,
    PD_CONTROL_GET_STATUS = 0x12,
    PD_CONTROL_FR_SWAP = 0x13,
    PD_CONTROL_GET_PPS_STATUS = 0x14,
    PD_CONTROL_GET_COUNTRY_CODES = 0x15,
    PD_CONTROL_GET_SINK_CAP_EXTENDED = 0x16,
    PD_CONTROL_GET_SOURCE_INFO = 0x17,
    PD_CONTROL_GET_REVISION = 0x18,
    PD_DATA_SOURCE_CAPABILITIES = 0x01,
    PD_DATA_REQUEST = 0x02,
    PD_DATA_BIST = 0x03,
    PD_DATA_SINK_CAPABILITIES = 0x04,
    PD_DATA_BATTERY_STATUS = 0x05,
    PD_DATA_ALERT = 0x06,
    PD_DATA_GET_COUNTRY_INFO = 0x07,
    PD_DATA_ENTER_USB = 0x08,
    PD_DATA_EPR_REQUEST = 0x09,
    PD_DATA_EPR_MODE = 0x0A,
    PD_DATA_SOURCE_INFO = 0x0B,
    PD_DATA_REVISION = 0x0C,
    PD_DATA_PPS_STATUS = 0x0D,
    PD_DATA_COUNTRY_CODE = 0x0E,
    PD_VENDOR_MESSAGE = 0x0F
} pd_message_type_t;

typedef enum
{
    PD_MODE_IDLE,
    PD_MODE_SINK
} pd_mode_t;
