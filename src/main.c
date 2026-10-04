/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_client.h"
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
#include <zephyr/net/mqtt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>
LOG_MODULE_REGISTER(MAIN);

BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_SSID) > 1,
             "Set CONFIG_APP_WIFI_SSID in secrets.conf");
BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_PSK) > 1,
             "Set CONFIG_APP_WIFI_PSK in secrets.conf");

bool button_state = false;
bool led_state = false;

static void format_sensor_value(char *buf, size_t buf_len,
                                struct sensor_value *v) {
  int32_t frac = v->val2 < 0 ? -v->val2 : v->val2;

  snprintf(buf, buf_len, "%d.%06d", v->val1, frac);
}

static K_SEM_DEFINE(net_ready, 0, 1);
static struct net_mgmt_event_callback ipv4_cb;

static void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t evt,
                         struct net_if *iface) {
  if (evt == NET_EVENT_IPV4_ADDR_ADD) {
    k_sem_give(&net_ready);
  }
}

#define NET_EVENT_WIFI_MASK                                                    \
  (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |          \
   NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |        \
   NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

static struct net_if *sta_iface;

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
  default:
    break;
  }
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

  sta_iface = net_if_get_wifi_sta();
  connect_to_wifi();

  k_sem_take(&net_ready, K_FOREVER);
  // MQTT
  struct mqtt_client client_ctx;
  while (app_mqtt_init(&client_ctx) != 0) {
    k_sleep(K_SECONDS(2)); /* DNS may still be flaky */
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