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
#include "wifi.h"
LOG_MODULE_REGISTER(MAIN);

bool button_state = false;
bool led_state = false;

static void format_sensor_value(char *buf, size_t buf_len,
                                struct sensor_value *v)
{
  int32_t frac = v->val2 < 0 ? -v->val2 : v->val2;

  snprintf(buf, buf_len, "%d.%06d", v->val1, frac);
}

#define LED0_NODE DT_ALIAS(led0)
static const struct led_dt_spec led0 = LED_DT_SPEC_GET(LED0_NODE);
const struct device *mpu = DEVICE_DT_GET(DT_NODELABEL(mpu6500));

static void button_input_cb(struct input_event *evt, void *user_data)
{
  if (evt->sync == 0)
  {
    return;
  }

  printk("Button %d %s at %" PRIu32 "\n", evt->code,
         evt->value ? "pressed" : "released", k_cycle_get_32());
  if (evt->code == 2 && evt->value)
    button_state = true;
  else
  {
    button_state = false;
  }
}

INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);
const char axes[] = {'x', 'y', 'z'};
struct sensor_value accel[3];
struct sensor_value gyro[3];
struct sensor_value temp;

int main(void)
{
  wifi_init();

  // MQTT
  struct mqtt_client client_ctx;
  while (app_mqtt_init(&client_ctx) != 0)
  {
    k_sleep(K_SECONDS(2));
  }
  app_mqtt_connect(&client_ctx);
  app_mqtt_subscribe(&client_ctx, CONFIG_READ_TOPIC);

  if (!led_is_ready_dt(&led0))
  {
    LOG_ERR("led not ready\n");
    return -ENODEV;
  }
  if (!device_is_ready(mpu))
  {
    LOG_ERR("MPU6500 not ready\n");
    return -ENODEV;
  }
  printk("Press the button\n");
  char msg[256]; // json publish data buffer
  while (true)
  {
    if (led_state)
    {
      led_on_dt(&led0);
    }
    else
    {
      led_off_dt(&led0);
    }

    if (mqtt_connected)
    {
      app_mqtt_process(&client_ctx);
    }
    else
    {
      /* Reconnect if dropped */
      app_mqtt_connect(&client_ctx);
      app_mqtt_subscribe(&client_ctx, CONFIG_READ_TOPIC);
    }

    // Step 1 — fetch all raw data from sensor over I2C
    int rc = sensor_sample_fetch(mpu);

    if (rc == 0)
    {
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