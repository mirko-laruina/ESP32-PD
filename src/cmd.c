

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_partition.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cmd_system.h"
#include "cmd_nvs.h"
#include "argtable3/argtable3.h"

#include "pd.h"
#include "pd_tx.h"
#include "pd_proto.h"

#define PROMPT_STR "usb_pd"
#define TAG "cmd"
#define CONFIG_CONSOLE_MAX_COMMAND_LINE_LENGTH 64

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#if !CONFIG_ESP_CONSOLE_SECONDARY_NONE
#warning "A secondary serial console is not useful when using the console component. Please disable it in menuconfig."
#endif
#endif

esp_console_repl_t *repl = NULL;
esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

static struct
{
    struct arg_int *object;
    struct arg_int *voltage_mv;
    struct arg_int *current_ma;
    struct arg_end *end;
} cmd_req_pps_args;

static struct
{
    struct arg_int *object;
    struct arg_int *current_ma;
    struct arg_end *end;
} cmd_req_obj_args;

static struct
{
    struct arg_int *command;
    struct arg_int *mode;
    struct arg_end *end;
} cmd_vdm_args;

static int cmd_req_pps(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&cmd_req_pps_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, cmd_req_pps_args.end, argv[0]);
        return 1;
    }

    uint16_t arg_obj = *cmd_req_pps_args.object->ival;
    uint16_t arg_mv = *cmd_req_pps_args.voltage_mv->ival;
    uint16_t arg_ma = *cmd_req_pps_args.current_ma->ival;

    pd_request_pps(arg_obj, arg_mv, arg_ma, 0);

    return 0;
}

static int cmd_req_obj(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&cmd_req_obj_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, cmd_req_obj_args.end, argv[0]);
        return 1;
    }

    uint16_t arg_obj = *cmd_req_obj_args.object->ival;
    uint16_t arg_ma = *cmd_req_obj_args.current_ma->ival;

    pd_request(arg_obj, arg_ma, 0);

    return 0;
}

static int cmd_vdm(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&cmd_vdm_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, cmd_vdm_args.end, argv[0]);
        return 1;
    }

    uint16_t arg_command = *cmd_vdm_args.command->ival;
    uint16_t arg_mode = cmd_vdm_args.mode->count ? *cmd_vdm_args.mode->ival : 0;

    pd_msg response = {0};

    response.target = PD_TARGET_SOP;
    response.header.power_role = PD_DATA_ROLE_UFP;
    response.header.spec_revision = 1;
    response.header.message_type = PD_VENDOR_MESSAGE;

    pd_vdm_packet resp_vdm = {
        .vdm_header = {
            .svid = PD_VDM_SID_PD,
            .vdm_type = 1,
            .vdm_version_major = 1,
            .vdm_version_minor = 0,
            .object_position = arg_mode,
            .command_type = PD_VDM_CMD_TYPE_REQ,
            .command = (pd_vdm_command)arg_command,
        },
    };

    pd_build_vdm(&resp_vdm, &response);
    pd_tx_enqueue(&response);

    return 0;
}

static int cmd_get_src_cap(int argc, char **argv)
{
    pd_send_control(PD_CONTROL_GET_SOURCE_CAP);

    return 0;
}

static int list_partitions(int argc, char **argv)
{
    /* Get the iterator for all partitions */
    const esp_partition_t *partition;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

    if (it == NULL)
    {
        printf("No partitions found.\n");
        return ESP_OK;
    }

    printf("Partitions:\n");
    printf("------------------------------------------------------------\n");
    printf("| Type        | Subtype     | Address    | Size    | Label |\n");
    printf("------------------------------------------------------------\n");

    /* Iterate through all partitions */
    while (it != NULL)
    {
        partition = esp_partition_get(it);
        printf("| %-11s | %-11s | 0x%08" PRIx32 " | %-7" PRId32 " | %-5s\n",
               partition->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
               partition->subtype == ESP_PARTITION_SUBTYPE_ANY ? "any" : "specific",
               partition->address,
               partition->size,
               partition->label);

        it = esp_partition_next(it);
    }

    printf("------------------------------------------------------------\n");

    /* Free the iterator */
    esp_partition_iterator_release(it);
    return ESP_OK;
}

const esp_console_cmd_t req_get_src_cap_cmd = {
    .command = "get_src_cap",
    .help = "Request source capabilities report."
            "t.b.d\n",
    .hint = NULL,
    .func = &cmd_get_src_cap,
    .argtable = NULL};

const esp_console_cmd_t req_pps_cmd = {
    .command = "req_pps",
    .help = "Request a PPS mode."
            "t.b.d\n",
    .hint = NULL,
    .func = &cmd_req_pps,
    .argtable = &cmd_req_pps_args};

const esp_console_cmd_t req_obj_cmd = {
    .command = "req_obj",
    .help = "Request a normal mode."
            "t.b.d\n",
    .hint = NULL,
    .func = &cmd_req_obj,
    .argtable = &cmd_req_obj_args};

const esp_console_cmd_t vdm_cmd = {
    .command = "vdm",
    .help = "Send a Vendor Defined Message (VDM) request.\n"
            "\n"
            "This command allows sending VDM requests as specified by the USB Power Delivery protocol.\n"
            "\n"
            "Parameters:\n"
            "  - command:\n"
            "    0 = Reserved, Shall Not be used\n"
            "    1 = Discover Identity\n"
            "    2 = Discover SVIDs\n"
            "    3 = Discover Modes\n"
            "    4 = Enter Mode\n"
            "    5 = Exit Mode\n"
            "    6 = Attention\n"
            "    7-15 = Reserved, Shall Not be used\n"
            "    16…31 = SVID Specific Commands\n"
            "\n"
            "  - object:\n"
            "    For Enter Mode, Exit Mode, and Attention Commands (Requests/Responses):\n"
            "      - 000b = Reserved and Shall Not be used\n"
            "      - 001b…110b = Index into the list of VDOs to identify the desired Mode VDO\n"
            "      - 111b = Exit all Active Modes (equivalent to a power-on reset). Shall only be used with the Exit Mode Command.\n"
            "    Commands 0…3, 7…15:\n"
            "      - 000b\n"
            "      - 001b…111b = Reserved and Shall Not be used\n"
            "    SVID Specific Commands (16…31) are defined by the SVID.\n",
    .hint = NULL,
    .func = &cmd_vdm,
    .argtable = &cmd_vdm_args};

const esp_console_cmd_t cmd_list_partitions = {
    .command = "list_partitions",
    .help = "List all partitions in the device",
    .hint = NULL,
    .func = &list_partitions,
};

void cmd_init()
{
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = CONFIG_CONSOLE_MAX_COMMAND_LINE_LENGTH;

    /* Register commands */
    esp_console_register_help_command();
    register_system_common();
#if SOC_LIGHT_SLEEP_SUPPORTED
    register_system_light_sleep();
#endif
#if SOC_DEEP_SLEEP_SUPPORTED
    register_system_deep_sleep();
#endif
    register_nvs();

    cmd_req_pps_args.object = arg_int1(NULL, NULL, "<object>", "object index");
    cmd_req_pps_args.voltage_mv = arg_int1(NULL, NULL, "<voltage_mv>", "requested voltage");
    cmd_req_pps_args.current_ma = arg_int1(NULL, NULL, "<current_ma>", "maximum current");
    cmd_req_pps_args.end = arg_end(2);

    cmd_req_obj_args.object = arg_int1(NULL, NULL, "<object>", "object index");
    cmd_req_obj_args.current_ma = arg_int1(NULL, NULL, "<current_ma>", "maximum current");
    cmd_req_obj_args.end = arg_end(2);

    cmd_vdm_args.command = arg_int1(NULL, NULL, "<command>", "command to send");
    cmd_vdm_args.mode = arg_int0(NULL, NULL, "<mode>", "mode to enter");
    cmd_vdm_args.end = arg_end(2);

    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_list_partitions));
    ESP_ERROR_CHECK(esp_console_cmd_register(&req_pps_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&req_obj_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&req_get_src_cap_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&vdm_cmd));
}

void cmd_main(void)
{
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

#else
#error Unsupported console type
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
