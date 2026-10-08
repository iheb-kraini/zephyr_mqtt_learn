/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_client.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <sys/_intsup.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>
LOG_MODULE_REGISTER(MAIN);

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

static char wifi_body[160];
static size_t wifi_body_len;

/* find "key=value" in a form body and URL-decode the value */
static void form_get(const char *body, const char *key, char *out,
                     size_t out_len) {
  size_t klen = strlen(key);
  const char *p = body;

  out[0] = '\0';
  while (p && *p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      size_t o = 0;

      p += klen + 1;
      while (*p && *p != '&' && o < out_len - 1) {
        if (*p == '+') {
          out[o++] = ' ';
          p++;
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                   isxdigit((unsigned char)p[2])) {
          char hex[3] = {p[1], p[2], 0};

          out[o++] = (char)strtol(hex, NULL, 16);
          p += 3;
        } else {
          out[o++] = *p++;
        }
      }
      out[o] = '\0';
      return;
    }
    p = strchr(p, '&');
    if (p) {
      p++;
    }
  }
}

static int wifi_post_handler(struct http_client_ctx *client,
                             enum http_transaction_status status,
                             const struct http_request_ctx *req,
                             struct http_response_ctx *rsp, void *user_data) {
  static const char ok[] = "Saved";

  if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
    wifi_body_len = 0;
    return 0;
  }

  /* body can arrive in several chunks */
  if (req->data_len > 0 && wifi_body_len + req->data_len < sizeof(wifi_body)) {
    memcpy(&wifi_body[wifi_body_len], req->data, req->data_len);
    wifi_body_len += req->data_len;
  }

  if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
    char ssid[33], psk[65];

    wifi_body[wifi_body_len] = '\0';
    form_get(wifi_body, "ssid", ssid, sizeof(ssid));
    form_get(wifi_body, "psk", psk, sizeof(psk));
    printk("WIFI FORM -> ssid='%s' psk='%s'\n", ssid, psk);

    wifi_body_len = 0;
    rsp->status = HTTP_200_OK;
    rsp->body = (const uint8_t *)ok;
    rsp->body_len = sizeof(ok) - 1;
    rsp->final_chunk = true;
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
static uint16_t http_port = 8080;

HTTP_SERVICE_DEFINE(my_service, NULL, &http_port,
                    CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(index_resource, my_service, "/", &index_resource_detail);

bool button_state = false;
bool led_state = false;

static void format_sensor_value(char *buf, size_t buf_len,
                                struct sensor_value *v) {
  int32_t frac = v->val2 < 0 ? -v->val2 : v->val2;

  snprintf(buf, buf_len, "%d.%06d", v->val1, frac);
}

static K_SEM_DEFINE(net_ready, 0, 1);
static K_SEM_DEFINE(ap_ready, 0, 1);

static struct net_mgmt_event_callback ipv4_cb;
static struct net_if *sta_iface;

static void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t evt,
                         struct net_if *iface) {
  if (evt == NET_EVENT_IPV4_ADDR_ADD && iface == sta_iface) {
    k_sem_give(&net_ready);
  }
}

#define NET_EVENT_WIFI_MASK                                                    \
  (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |          \
   NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |        \
   NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

static struct net_if *ap_iface;
static struct wifi_connect_req_params ap_config;
static struct wifi_connect_req_params sta_config;

static struct net_mgmt_event_callback cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface) {
  switch (mgmt_event) {
  case NET_EVENT_WIFI_CONNECT_RESULT: {
    LOG_INF("Connected to %s", CONFIG_WIFI_SAMPLE_SSID);
    break;
  }
  case NET_EVENT_WIFI_DISCONNECT_RESULT: {
    LOG_INF("Disconnected from %s", CONFIG_WIFI_SAMPLE_SSID);
    break;
  }
  case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
    LOG_INF("AP Mode is enabled. Waiting for station to connect");
    k_sem_give(&ap_ready);
    break;
  }
  case NET_EVENT_WIFI_AP_DISABLE_RESULT: {
    LOG_INF("AP Mode is disabled.");
    break;
  }
  case NET_EVENT_WIFI_AP_STA_CONNECTED: {
    struct wifi_ap_sta_info *sta_info = (struct wifi_ap_sta_info *)cb->info;

    LOG_INF("station: " MACSTR " joined ", sta_info->mac[0], sta_info->mac[1],
            sta_info->mac[2], sta_info->mac[3], sta_info->mac[4],
            sta_info->mac[5]);
    break;
  }
  case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
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

static void enable_dhcpv4_server(void) {
  static struct net_in_addr addr;
  static struct net_in_addr netmaskAddr;

  if (net_addr_pton(NET_AF_INET, CONFIG_WIFI_SAMPLE_AP_IP_ADDRESS, &addr)) {
    LOG_ERR("Invalid address: %s", CONFIG_WIFI_SAMPLE_AP_IP_ADDRESS);
    return;
  }

  if (net_addr_pton(NET_AF_INET, CONFIG_WIFI_SAMPLE_AP_NETMASK, &netmaskAddr)) {
    LOG_ERR("Invalid netmask: %s", CONFIG_WIFI_SAMPLE_AP_NETMASK);
    return;
  }

  net_if_ipv4_set_gw(ap_iface, &addr);

  if (net_if_ipv4_addr_add(ap_iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
    LOG_ERR("unable to set IP address for AP interface");
  }

  if (!net_if_ipv4_set_netmask_by_addr(ap_iface, &addr, &netmaskAddr)) {
    LOG_ERR("Unable to set netmask for AP interface: %s",
            CONFIG_WIFI_SAMPLE_AP_NETMASK);
  }

  addr.s4_addr[3] += 10; /* Starting IPv4 address for DHCPv4 address pool. */

  if (net_dhcpv4_server_start(ap_iface, &addr) != 0) {
    LOG_ERR("DHCP server is not started for desired IP");
    return;
  }

  LOG_INF("DHCPv4 server started...\n");
}

static int enable_ap_mode(void) {
  if (!ap_iface) {
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

  if (sizeof(CONFIG_WIFI_SAMPLE_AP_PSK) == 1) {
    ap_config.security = WIFI_SECURITY_TYPE_NONE;
  } else {

    ap_config.security = WIFI_SECURITY_TYPE_PSK;
  }

  enable_dhcpv4_server();

  int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap_config,
                     sizeof(struct wifi_connect_req_params));
  if (ret) {
    LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed, err: %d", ret);
  }

  return ret;
}

static int connect_to_wifi(void) {
  if (!sta_iface) {
    LOG_INF("STA: interface no initialized");
    return -EIO;
  }

  sta_config.ssid = (const uint8_t *)CONFIG_WIFI_SAMPLE_SSID;
  sta_config.ssid_length = sizeof(CONFIG_WIFI_SAMPLE_SSID) - 1;
  sta_config.psk = (const uint8_t *)CONFIG_WIFI_SAMPLE_PSK;
  sta_config.psk_length = sizeof(CONFIG_WIFI_SAMPLE_PSK) - 1;
  sta_config.security = WIFI_SECURITY_TYPE_PSK;
  sta_config.channel = WIFI_CHANNEL_ANY;
  sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

  LOG_INF("Connecting to SSID: %s\n", sta_config.ssid);

  int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config,
                     sizeof(struct wifi_connect_req_params));
  if (ret) {
    LOG_ERR("Unable to Connect to (%s)", CONFIG_WIFI_SAMPLE_SSID);
  }

  return ret;
}

#define LED0_NODE DT_ALIAS(led0)
static const struct led_dt_spec led0 = LED_DT_SPEC_GET(LED0_NODE);
const struct device *mpu = DEVICE_DT_GET(DT_NODELABEL(mpu6500));

static void button_input_cb(struct input_event *evt, void *user_data) {
  if (evt->sync == 0) {
    return;
  }

  printk("Button %d %s at %" PRIu32 "\n", evt->code,
         evt->value ? "pressed" : "released", k_cycle_get_32());
  if (evt->code == 2 && evt->value)
    button_state = true;
  else {
    button_state = false;
  }
}

INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);
const char axes[] = {'x', 'y', 'z'};
struct sensor_value accel[3];
struct sensor_value gyro[3];
struct sensor_value temp;

int main(void) {
  net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
  net_mgmt_add_event_callback(&cb);

  net_mgmt_init_event_callback(&ipv4_cb, ipv4_handler, NET_EVENT_IPV4_ADDR_ADD);
  net_mgmt_add_event_callback(&ipv4_cb);

  ap_iface = net_if_get_wifi_sap();
  sta_iface = net_if_get_wifi_sta();

  LOG_INF("sta=%d ap=%d default=%d", net_if_get_by_iface(sta_iface),
          net_if_get_by_iface(ap_iface),
          net_if_get_by_iface(net_if_get_default()));

  enable_ap_mode();
  k_sem_take(&ap_ready, K_FOREVER);

  connect_to_wifi();
  k_sem_take(&net_ready, K_FOREVER);

  /* Start HTTP server */
  int http_ret = http_server_start();
  if (http_ret != 0) {
    LOG_ERR("Failed to start HTTP server: %d", http_ret);
  }
  // MQTT
  struct mqtt_client client_ctx;
  while (app_mqtt_init(&client_ctx) != 0) {
    k_sleep(K_SECONDS(2));
  }
  app_mqtt_connect(&client_ctx);
  app_mqtt_subscribe(&client_ctx, CONFIG_READ_TOPIC);

  if (!led_is_ready_dt(&led0)) {
    return -ENODEV;
  }
  if (!device_is_ready(mpu)) {
    printk("MPU6500 not ready\n");
    return -ENODEV;
  }
  printk("Press the button\n");
  char msg[256]; // json publish data buffer
  while (true) {
    LOG_INF("led_state:%s", led_state ? "on" : "off");
    if (led_state) {
      led_on_dt(&led0);
    } else {
      led_off_dt(&led0);
    }

    if (mqtt_connected) {
      app_mqtt_process(&client_ctx);
    } else {
      /* Reconnect if dropped */
      app_mqtt_connect(&client_ctx);
      app_mqtt_subscribe(&client_ctx, CONFIG_READ_TOPIC);
    }

    // Step 1 — fetch all raw data from sensor over I2C
    int rc = sensor_sample_fetch(mpu);

    if (rc == 0) {
      rc = sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_XYZ, accel);
    }
    char xs[16], ys[16], zs[16];

    format_sensor_value(xs, sizeof(xs), &accel[0]);
    format_sensor_value(ys, sizeof(ys), &accel[1]);
    format_sensor_value(zs, sizeof(zs), &accel[2]);

    snprintf(msg, 256,
             "{\"button\":\"%s\",\"mpu_accel\":{\"x\":%s,\"y\":%s,\"z\":%s}}",
             button_state ? "on" : "off", xs, ys, zs);

    app_mqtt_publish(&client_ctx, CONFIG_WRITE_TOPIC, msg);

    k_sleep(K_MSEC(100));
  }

  k_sleep(K_FOREVER);

  return 0;
}