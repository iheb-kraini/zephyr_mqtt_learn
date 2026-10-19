#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>
#include <ctype.h>
#include "wifi.h"

LOG_MODULE_REGISTER(WIFI);

static char wifi_body[160];
static size_t wifi_body_len;
static bool wifi_body_overflow = false;
uint16_t http_port = 8080;

enum State state;

struct net_mgmt_event_callback ipv4_cb;

struct net_if *sta_iface;
struct net_if *ap_iface;
struct wifi_connect_req_params ap_config;
struct wifi_connect_req_params sta_config;
struct net_mgmt_event_callback cb;

struct k_sem net_ready;
struct k_sem ap_ready;

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"

BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_SSID) > 1,
             "Set CONFIG_APP_WIFI_SSID in secrets.conf");
BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_PSK) > 1,
             "Set CONFIG_APP_WIFI_PSK in secrets.conf");

BUILD_ASSERT(
    sizeof(CONFIG_WIFI_SAMPLE_AP_SSID) > 1,
    "CONFIG_WIFI_SAMPLE_AP_SSID is empty. Please set it in conf file.");

BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_AP_PSK) > 1,
             "CONFIG_WIFI_SAMPLE_SSID is empty. Please set it in conf file.");

/* find "key=value" in a form body and URL-decode the value */
void form_get(const char *body, const char *key, char *out,
              size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;

    out[0] = '\0';
    while (p && *p)
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            size_t o = 0;

            p += klen + 1;
            while (*p && *p != '&' && o < out_len - 1)
            {
                if (*p == '+')
                {
                    out[o++] = ' ';
                    p++;
                }
                else if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                         isxdigit((unsigned char)p[2]))
                {
                    char hex[3] = {p[1], p[2], 0};

                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                }
                else
                {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (p)
        {
            p++;
        }
    }
}

wifi_credenital_t wifi_credential;
K_SEM_DEFINE(wifi_creds_ready, 0, 1);

int wifi_post_handler(struct http_client_ctx *client,
                      enum http_transaction_status status,
                      const struct http_request_ctx *req,
                      struct http_response_ctx *rsp, void *user_data)
{
    static const char ok[] = "Saved";
    static const char too_big[] = "Request too large";

    if (status == HTTP_SERVER_TRANSACTION_ABORTED)
    {
        wifi_body_len = 0;
        wifi_body_overflow = false;
        return 0;
    }

    /* body can arrive in several chunks */
    if (!wifi_body_overflow && req->data_len > 0 &&
        wifi_body_len + req->data_len < sizeof(wifi_body))
    {
        memcpy(&wifi_body[wifi_body_len], req->data, req->data_len);
        wifi_body_len += req->data_len;
    }
    else
    {
        if (req->data_len > 0)
            wifi_body_overflow = true;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_FINAL)
    {
        rsp->final_chunk = true;
        if (wifi_body_overflow)
        {
            LOG_ERR("WIFI FORM rejected: body too large");
            rsp->status = 413;
            rsp->body = (const uint8_t *)too_big;
            rsp->body_len = sizeof(too_big) - 1;
            wifi_body_len = 0;
            wifi_body_overflow = false;
            return 0;
        }

        wifi_body[wifi_body_len] = '\0';
        form_get(wifi_body, "ssid", wifi_credential.ssid, sizeof(wifi_credential.ssid));
        form_get(wifi_body, "psk", wifi_credential.password, sizeof(wifi_credential.password));
        LOG_INF("WIFI FORM -> ssid='%s' psk='%s'\n", wifi_credential.ssid, wifi_credential.password);

        wifi_body_len = 0;
        rsp->status = HTTP_200_OK;
        rsp->body = (const uint8_t *)ok;
        rsp->body_len = sizeof(ok) - 1;
        if (state == read_credential)
        {
            k_sem_give(&wifi_creds_ready);
        }
        else
        {
            LOG_INF("unexpected state or mal user beahvour dont set credentail after initialization current state is :%d", state);
        }
    }
    return 0;
}

static struct http_resource_detail_dynamic wifi_resource_detail = {
    .common =
        {
            .type = HTTP_RESOURCE_TYPE_DYNAMIC,
            .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        },
    .cb = wifi_post_handler,
    .user_data = NULL,
};

HTTP_RESOURCE_DEFINE(wifi_resource, my_service, "/wifi", &wifi_resource_detail);

static uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static struct http_resource_detail_static index_resource_detail = {
    .common =
        {
            .type = HTTP_RESOURCE_TYPE_STATIC,
            .bitmask_of_supported_http_methods = BIT(HTTP_GET),
            .content_encoding = "gzip",
            .content_type = "text/html",
        },
    .static_data = index_html_gz,
    .static_data_len = sizeof(index_html_gz),
};

HTTP_SERVICE_DEFINE(my_service, NULL, &http_port,
                    CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(index_resource, my_service, "/", &index_resource_detail);

K_SEM_DEFINE(net_ready, 0, 1);
K_SEM_DEFINE(ap_ready, 0, 1);

void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t evt,
                  struct net_if *iface)
{
    if (evt == NET_EVENT_IPV4_ADDR_ADD && iface == sta_iface)
    {
        k_sem_give(&net_ready);
    }
}

void wifi_event_handler(struct net_mgmt_event_callback *cb,
                        uint64_t mgmt_event, struct net_if *iface)
{
    switch (mgmt_event)
    {
    case NET_EVENT_WIFI_CONNECT_RESULT:
    {
        LOG_INF("Connected to %s", CONFIG_WIFI_SAMPLE_SSID);
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
    {
        LOG_INF("Disconnected from %s", CONFIG_WIFI_SAMPLE_SSID);
        break;
    }
    case NET_EVENT_WIFI_AP_ENABLE_RESULT:
    {
        LOG_INF("AP Mode is enabled. Waiting for station to connect");
        k_sem_give(&ap_ready);
        break;
    }
    case NET_EVENT_WIFI_AP_DISABLE_RESULT:
    {
        LOG_INF("AP Mode is disabled.");
        break;
    }
    case NET_EVENT_WIFI_AP_STA_CONNECTED:
    {
        struct wifi_ap_sta_info *sta_info = (struct wifi_ap_sta_info *)cb->info;

        LOG_INF("station: " MACSTR " joined ", sta_info->mac[0], sta_info->mac[1],
                sta_info->mac[2], sta_info->mac[3], sta_info->mac[4],
                sta_info->mac[5]);
        break;
    }
    case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
    {
        struct wifi_ap_sta_info *sta_info = (struct wifi_ap_sta_info *)cb->info;

        LOG_INF("station: " MACSTR " leave ", sta_info->mac[0], sta_info->mac[1],
                sta_info->mac[2], sta_info->mac[3], sta_info->mac[4],
                sta_info->mac[5]);
        break;
    }
    default:
        break;
    }
}

void enable_dhcpv4_server(void)
{
    static struct net_in_addr addr;
    static struct net_in_addr netmaskAddr;

    if (net_addr_pton(NET_AF_INET, CONFIG_WIFI_SAMPLE_AP_IP_ADDRESS, &addr))
    {
        LOG_ERR("Invalid address: %s", CONFIG_WIFI_SAMPLE_AP_IP_ADDRESS);
        return;
    }

    if (net_addr_pton(NET_AF_INET, CONFIG_WIFI_SAMPLE_AP_NETMASK, &netmaskAddr))
    {
        LOG_ERR("Invalid netmask: %s", CONFIG_WIFI_SAMPLE_AP_NETMASK);
        return;
    }

    net_if_ipv4_set_gw(ap_iface, &addr);

    if (net_if_ipv4_addr_add(ap_iface, &addr, NET_ADDR_MANUAL, 0) == NULL)
    {
        LOG_ERR("unable to set IP address for AP interface");
    }

    if (!net_if_ipv4_set_netmask_by_addr(ap_iface, &addr, &netmaskAddr))
    {
        LOG_ERR("Unable to set netmask for AP interface: %s",
                CONFIG_WIFI_SAMPLE_AP_NETMASK);
    }

    addr.s4_addr[3] += 10; /* Starting IPv4 address for DHCPv4 address pool. */

    if (net_dhcpv4_server_start(ap_iface, &addr) != 0)
    {
        LOG_ERR("DHCP server is not started for desired IP");
        return;
    }

    LOG_INF("DHCPv4 server started...\n");
}

int enable_ap_mode(void)
{
    if (!ap_iface)
    {
        LOG_INF("AP: is not initialized");
        return -EIO;
    }

    LOG_INF("Turning on AP Mode");
    ap_config.ssid = (const uint8_t *)CONFIG_WIFI_SAMPLE_AP_SSID;
    ap_config.ssid_length = sizeof(CONFIG_WIFI_SAMPLE_AP_SSID) - 1;
    ap_config.psk = (const uint8_t *)CONFIG_WIFI_SAMPLE_AP_PSK;
    ap_config.psk_length = sizeof(CONFIG_WIFI_SAMPLE_AP_PSK) - 1;
    ap_config.channel = WIFI_CHANNEL_ANY;
    ap_config.band = WIFI_FREQ_BAND_2_4_GHZ;

    if (sizeof(CONFIG_WIFI_SAMPLE_AP_PSK) == 1)
    {
        ap_config.security = WIFI_SECURITY_TYPE_NONE;
    }
    else
    {

        ap_config.security = WIFI_SECURITY_TYPE_PSK;
    }

    enable_dhcpv4_server();

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap_config,
                       sizeof(struct wifi_connect_req_params));
    if (ret)
    {
        LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed, err: %d", ret);
    }

    return ret;
}

int connect_to_wifi(void)
{
    if (!sta_iface)
    {
        LOG_ERR("STA: interface no initialized");
        return -EIO;
    }

    sta_config.ssid = (const uint8_t *)wifi_credential.ssid;
    sta_config.ssid_length = sizeof(wifi_credential.ssid) - 1;
    sta_config.psk = (const uint8_t *)wifi_credential.password;
    sta_config.psk_length = sizeof(wifi_credential.password) - 1;
    sta_config.security = WIFI_SECURITY_TYPE_PSK;
    sta_config.channel = WIFI_CHANNEL_ANY;
    sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

    LOG_INF("Connecting to SSID: %s\n", sta_config.ssid);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config,
                       sizeof(struct wifi_connect_req_params));
    if (ret)
    {
        LOG_ERR("Unable to Connect to (%s)", CONFIG_WIFI_SAMPLE_SSID);
    }

    return ret;
}

int wifi_init(void)
{
    state = init;

    while (state != connected)
    {
        int rc;
        switch (state)
        {
        case init:
            net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
            net_mgmt_add_event_callback(&cb);

            net_mgmt_init_event_callback(&ipv4_cb, ipv4_handler, NET_EVENT_IPV4_ADDR_ADD);
            net_mgmt_add_event_callback(&ipv4_cb);

            ap_iface = net_if_get_wifi_sap();
            sta_iface = net_if_get_wifi_sta();
            if (ap_iface == NULL || sta_iface == NULL)
            {
                LOG_ERR("fail to initiallize network interface");
                return 1;
            }
            if ((rc = enable_ap_mode()) != 0)
            {
                LOG_ERR("Failed to enable ap mode :%d", rc);
                return rc;
            }
            k_sem_take(&ap_ready, K_FOREVER);
            /* Start HTTP server */

            if ((rc = http_server_start()) != 0)
            {
                LOG_ERR("Failed to start HTTP server:%d", rc);
                return rc;
            }
            state = read_credential;
            break;
        case read_credential:

            k_sem_take(&wifi_creds_ready, K_FOREVER);
            state = connecting;

            break;
        case connecting:

            if ((rc = connect_to_wifi()) != 0)
            {
                LOG_ERR("failed to connect to wifi :%d", rc);
                state = read_credential;
                break;
            }
            k_sem_take(&net_ready, K_FOREVER);
            state = connected;
            break;
        case connected:
            break;

        default:
            break;
        }
    }

    return 0;
}