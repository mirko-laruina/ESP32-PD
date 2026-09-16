#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"

#include "pd.h"
#include "pd_status.h"
#include "webserver.h"
#include "web_page.h"
#include "wifi_manager.h"

#define TAG "WEB"
#define NVS_NAMESPACE "web"
#define REQUEST_BUSY_US (1500000ULL)
#define CHARGER_STALE_US (10000000ULL)

static bool s_http_started;
static httpd_handle_t s_httpd;

static const char *req_state_name(pd_status_req_state_t state)
{
    switch (state)
    {
    case PD_STATUS_REQ_PENDING:
        return "pending";
    case PD_STATUS_REQ_ACCEPTED:
        return "accepted";
    case PD_STATUS_REQ_REJECTED:
        return "rejected";
    case PD_STATUS_REQ_READY:
        return "ready";
    case PD_STATUS_REQ_TIMEOUT:
        return "timeout";
    default:
        return "none";
    }
}

static esp_err_t serve_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, WEB_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t serve_status(httpd_req_t *req)
{
    pd_status_data_t st;
    char buf[3072];
    int len = 0;

    httpd_resp_set_type(req, "application/json");

    if (!pd_status_get(&st))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status unavailable");
        return ESP_FAIL;
    }

    uint64_t now = esp_timer_get_time();

    len += snprintf(buf, sizeof(buf),
                    "{\"uptime_s\":%lld,\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"ssid\":\"%s\"},"
                    "\"pd\":{\"last_rx_ms\":%lld,\"charger_seen\":%s,\"last_message\":\"%s\",\"pdos\":[",
                    (long long)(now / 1000000),
                    wifi_manager_is_connected() ? "true" : "false",
                    wifi_manager_ip(),
                    wifi_manager_ssid(),
                    st.charger_seen ? (long long)((now - st.last_rx_us) / 1000) : -1LL,
                    st.charger_seen ? "true" : "false",
                    st.last_message);

    for (int i = 0; i < st.pdo_count && (size_t)len < sizeof(buf) - 128; i++)
    {
        pd_status_pdo_t *p = &st.pdos[i];
        const char *type = (p->type == PD_STATUS_PDO_FIXED) ? "fixed" : (p->type == PD_STATUS_PDO_PPS) ? "pps"
                                                                                                          : "other";
        if (p->type == PD_STATUS_PDO_PPS)
        {
            len += snprintf(buf + len, sizeof(buf) - len,
                            "{\"pos\":%d,\"type\":\"%s\",\"min_mv\":%lu,\"max_mv\":%lu,\"ma\":%lu},",
                            i + 1, type, (unsigned long)p->min_mv, (unsigned long)p->max_mv, (unsigned long)p->ma);
        }
        else
        {
            len += snprintf(buf + len, sizeof(buf) - len,
                            "{\"pos\":%d,\"type\":\"%s\",\"mv\":%lu,\"ma\":%lu},",
                            i + 1, type, (unsigned long)p->mv, (unsigned long)p->ma);
        }
    }

    if (st.pdo_count > 0 && len > 0 && buf[len - 1] == ',')
    {
        len--;
    }

    const char *pps_capability = "unknown";
    if (st.pps_status_capability_known)
    {
        pps_capability = st.pps_status_supported ? "supported" : "unsupported";
    }

    const char *pps_state = "off";
    if (!st.req_pps || st.req_state != PD_STATUS_REQ_READY)
    {
        pps_state = "gated";
    }
    else if (!st.pps_status_capability_known || st.pps_status_pending)
    {
        pps_state = "checking";
    }
    else if (!st.pps_status_supported)
    {
        pps_state = "unsupported";
    }
    else if (st.pps_poll_enabled && st.pps_status_seen)
    {
        pps_state = "live";
    }
    else if (st.pps_poll_enabled)
    {
        pps_state = "waiting";
    }

    char mv_str[12], ma_str[12], age_str[16], code_str[12];
    if (st.pps_status_seen)
    {
        snprintf(mv_str, sizeof(mv_str), "%lu", (unsigned long)st.pps_status_mv);
        snprintf(ma_str, sizeof(ma_str), "%lu", (unsigned long)st.pps_status_ma);
        snprintf(age_str, sizeof(age_str), "%lu", (unsigned long)((now - st.pps_status_us) / 1000));
    }
    else
    {
        strlcpy(mv_str, "null", sizeof(mv_str));
        strlcpy(ma_str, "null", sizeof(ma_str));
        strlcpy(age_str, "null", sizeof(age_str));
    }
    snprintf(code_str, sizeof(code_str), "%u", st.pps_status_code);

    len += snprintf(buf + len, sizeof(buf) - len,
                    "],\"req\":{\"state\":\"%s\",\"pps\":%s,\"object\":%u,\"mv\":%lu,\"ma\":%lu,\"age_ms\":%lu},"
                    "\"actual\":{\"reported\":false,\"estimated\":%s,\"mv\":%lu,\"ma\":%lu},"
                    "\"pps_poll\":{\"enabled\":%s,\"capability\":\"%s\",\"state\":\"%s\",\"code\":%s,\"mv\":%s,\"ma\":%s,\"age_ms\":%s}}}",
                    req_state_name(st.req_state),
                    st.req_pps ? "true" : "false",
                    st.req_object,
                    (unsigned long)st.req_mv,
                    (unsigned long)st.req_ma,
                    (unsigned long)((now - st.req_time_us) / 1000),
                    st.req_state == PD_STATUS_REQ_READY ? "true" : "false",
                    (unsigned long)st.req_mv,
                    (unsigned long)st.req_ma,
                    st.pps_poll_enabled ? "true" : "false",
                    pps_capability,
                    pps_state,
                    code_str,
                    mv_str,
                    ma_str,
                    age_str);

    return httpd_resp_send(req, buf, len);
}

static bool charger_data_fresh(pd_status_data_t *st)
{
    return st->charger_seen && st->pdo_count > 0 &&
           (esp_timer_get_time() - st->last_rx_us) < CHARGER_STALE_US;
}

static esp_err_t handle_request(httpd_req_t *req)
{
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);

    httpd_resp_set_type(req, "application/json");

    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"corpo richiesta mancante\"}");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"JSON non valido\"}");
        return ESP_FAIL;
    }

    cJSON *j_mode = cJSON_GetObjectItem(root, "mode");
    cJSON *j_object = cJSON_GetObjectItem(root, "object");
    cJSON *j_mv = cJSON_GetObjectItem(root, "mv");
    cJSON *j_ma = cJSON_GetObjectItem(root, "ma");
    char err[128] = {0};
    bool ok = false;
    bool is_pps = false;
    uint8_t object = 0;
    uint32_t mv = 0, ma = 0;

    pd_status_data_t st;
    bool have_status = pd_status_get(&st);

    if (!have_status || !charger_data_fresh(&st))
    {
        strlcpy(err, "Nessuna comunicazione recente con il caricatore: impossibile richiedere tensioni.", sizeof(err));
    }
    else if (!cJSON_IsString(j_mode) || !cJSON_IsNumber(j_object) || !cJSON_IsNumber(j_ma))
    {
        strlcpy(err, "Parametri mancanti o non validi.", sizeof(err));
    }
    else
    {
        object = (uint8_t)cJSON_GetNumberValue(j_object);
        ma = (uint32_t)cJSON_GetNumberValue(j_ma);
        mv = cJSON_IsNumber(j_mv) ? (uint32_t)cJSON_GetNumberValue(j_mv) : 0;

        if (object < 1 || object > st.pdo_count)
        {
            snprintf(err, sizeof(err), "Modalita' %d inesistente: il caricatore ne dichiara %d.", object, st.pdo_count);
        }
        else if (st.req_state == PD_STATUS_REQ_PENDING &&
                 esp_timer_get_time() - st.req_time_us < REQUEST_BUSY_US)
        {
            strlcpy(err, "Richiesta gia' in corso, attendere la risposta del caricatore.", sizeof(err));
        }
        else
        {
            pd_status_pdo_t *p = &st.pdos[object - 1];

            if (strcmp(j_mode->valuestring, "fixed") == 0 && p->type == PD_STATUS_PDO_FIXED)
            {
                if (mv != 0 && mv != p->mv)
                {
                    snprintf(err, sizeof(err), "Tensione richiesta (%lu mV) diversa da quella del PDO (%lu mV).",
                             (unsigned long)mv, (unsigned long)p->mv);
                }
                else if (ma == 0 || ma > p->ma)
                {
                    snprintf(err, sizeof(err), "Corrente non valida: il PDO %d offre fino a %lu mA.", object, (unsigned long)p->ma);
                }
                else
                {
                    is_pps = false;
                    mv = p->mv;
                    ok = true;
                }
            }
            else if (strcmp(j_mode->valuestring, "pps") == 0 && p->type == PD_STATUS_PDO_PPS)
            {
                if (mv < p->min_mv)
                {
                    mv = p->min_mv;
                }
                if (mv > p->max_mv)
                {
                    mv = p->max_mv;
                }
                mv = ((mv + 10) / 20) * 20;

                if (ma == 0 || ma > p->ma)
                {
                    snprintf(err, sizeof(err), "Corrente non valida: l'APDO PPS offre fino a %lu mA.", (unsigned long)p->ma);
                }
                else
                {
                    is_pps = true;
                    ok = true;
                }
            }
            else
            {
                strlcpy(err, "Modalita' non compatibile con l'oggetto selezionato.", sizeof(err));
            }
        }
    }

    cJSON_Delete(root);

    if (!ok)
    {
        char resp[192];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, resp);
        return ESP_FAIL;
    }

    pd_request_object(is_pps, object, mv, ma);
    pd_status_begin_request(is_pps, object, mv, ma);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"object\":%u,\"mv\":%lu,\"ma\":%lu,\"pps\":%s}",
             object, (unsigned long)mv, (unsigned long)ma, is_pps ? "true" : "false");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_refresh(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    pd_send_control(PD_CONTROL_GET_SOURCE_CAP);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_pps_poll(httpd_req_t *req)
{
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);

    httpd_resp_set_type(req, "application/json");

    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"missing body\"}");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL || !cJSON_IsBool(cJSON_GetObjectItem(root, "enabled")))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_FAIL;
    }

    bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "enabled"));
    cJSON_Delete(root);
    pd_status_pps_poll_set(enabled);

    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t uri_index = {
    .uri = "/", .method = HTTP_GET, .handler = serve_index, .user_ctx = NULL};
static const httpd_uri_t uri_status = {
    .uri = "/api/status", .method = HTTP_GET, .handler = serve_status, .user_ctx = NULL};
static const httpd_uri_t uri_request = {
    .uri = "/api/request", .method = HTTP_POST, .handler = handle_request, .user_ctx = NULL};
static const httpd_uri_t uri_refresh = {
    .uri = "/api/refresh", .method = HTTP_POST, .handler = handle_refresh, .user_ctx = NULL};
static const httpd_uri_t uri_pps_poll = {
    .uri = "/api/pps_poll", .method = HTTP_POST, .handler = handle_pps_poll, .user_ctx = NULL};

static void start_httpd(void)
{
    if (s_http_started)
    {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    if (httpd_start(&s_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(s_httpd, &uri_index);
        httpd_register_uri_handler(s_httpd, &uri_status);
        httpd_register_uri_handler(s_httpd, &uri_request);
        httpd_register_uri_handler(s_httpd, &uri_refresh);
        httpd_register_uri_handler(s_httpd, &uri_pps_poll);
        s_http_started = true;
        ESP_LOGI(TAG, "Web server avviato su http://%s/", wifi_manager_ip());
    }
}

static void wifi_ip_ready(const char *ip)
{
    (void)ip;
    start_httpd();
}

static struct
{
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} webconfig_args;

static int cmd_webconfig(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&webconfig_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, webconfig_args.end, argv[0]);
        return 1;
    }

    const char *ssid = webconfig_args.ssid->sval[0];
    const char *pass = webconfig_args.pass->count ? webconfig_args.pass->sval[0] : "";

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        printf("Errore apertura NVS\n");
        return 1;
    }
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", pass);
    nvs_commit(nvs);
    nvs_close(nvs);

    printf("Credenziali salvate. Connessione a \"%s\"...\n", ssid);
    wifi_manager_connect(ssid, pass);
    return 0;
}

void webserver_start(void)
{
    nvs_handle_t nvs;
    char ssid[33] = {0};
    char pass[64] = {0};
    size_t len;

    wifi_manager_set_ip_callback(wifi_ip_ready);

    webconfig_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID della rete Wi-Fi");
    webconfig_args.pass = arg_str0(NULL, NULL, "<password>", "Password Wi-Fi (vuota per reti aperte)");
    webconfig_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "webconfig",
        .help = "Configura e connette il Wi-Fi (salvataggio permanente in NVS)",
        .hint = NULL,
        .func = &cmd_webconfig,
        .argtable = &webconfig_args};
    esp_console_cmd_register(&cmd);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    {
        ESP_LOGW(TAG, "Nessuna credenziale Wi-Fi salvata: usa il comando console 'webconfig <ssid> <password>'");
        return;
    }

    len = sizeof(ssid);
    if (nvs_get_str(nvs, "ssid", ssid, &len) != ESP_OK || ssid[0] == '\0')
    {
        nvs_close(nvs);
        ESP_LOGW(TAG, "Nessuna credenziale Wi-Fi salvata: usa il comando console 'webconfig <ssid> <password>'");
        return;
    }

    len = sizeof(pass);
    nvs_get_str(nvs, "pass", pass, &len);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Connessione Wi-Fi a \"%s\"...", ssid);
    wifi_manager_connect(ssid, pass);
}
