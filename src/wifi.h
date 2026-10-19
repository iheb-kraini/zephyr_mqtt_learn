#ifndef WIFI_H_GUARD
#define WIFI_H_GUARD

#define NET_EVENT_WIFI_MASK                                               \
    (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |   \
     NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT | \
     NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

enum State
{
    init,
    read_credential,
    connecting,
    connected,
};

typedef struct
{
    char ssid[33];
    char password[65];
} wifi_credenital_t;

/**
 * @brief helper function to read parameter query from http post req
 * @param body http req URL with the query prams
 * @param key the key ur querying for
 * @param out output buffer to get the value for that key
 * @param out_len the out buffer size
 */
void form_get(const char *body, const char *key, char *out,
              size_t out_len);

/**
 * @typedef http_resource_dynamic_cb_t
 * @brief Callback used when data is received on /wifi end point.
 *
 * @param client HTTP context information for this client connection.
 * @param status HTTP transaction status, HTTP_SERVER_REQUEST_DATA_MORE and
 *               HTTP_SERVER_REQUEST_DATA_FINAL describe the request-body delivery progress.
 *               HTTP_SERVER_TRANSACTION_ABORTED and HTTP_SERVER_TRANSACTION_COMPLETE are
 *               terminal notifications.
 * @param request_ctx Request context structure containing HTTP request data that was received.
 * @param response_ctx Response context structure for application to populate with response data.
 * @param user_data User specified data.
 *
 * @return 0 success, server can send any response data provided in the response_ctx.
 *         <0 error, close the connection.
 */
int wifi_post_handler(struct http_client_ctx *client,
                      enum http_transaction_status status,
                      const struct http_request_ctx *req,
                      struct http_response_ctx *rsp, void *user_data);

/**
 * @brief relase net_ready mutext when ipv4 is ready */
void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t evt, struct net_if *iface);

/**
 * @brief wifi_sta_ap event handler
 * @param cb event callback initialized by net_mgmt_init_event_callback macro
 */
void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface);

/**
 * @brief enable dhcpv4 server
 */
void enable_dhcpv4_server(void);

int enable_ap_mode(void);

int connect_to_wifi(void);

int wifi_init(void);
#endif