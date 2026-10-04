# zephyr-esp32s3-mqtt

ESP32-S3 + Zephyr RTOS: streams MPU6500 accelerometer and button state
over MQTT, and lets a web dashboard control the LED.

## Hardware

| Part    | Connection                              |
|---------|-----------------------------------------|
| LED     | GPIO12 (active high)                    |
| Button  | GPIO15 (pull-down, active high)         |
| MPU6500 | I2C1, address 0x68      |

I2C SDA/SCL: GPIO4 , GPIO5


## MQTT topics

| Topic                    | Direction        | Payload                                              |
|--------------------------|------------------|------------------------------------------------------|
| `<prefix>/data`          | device -> broker | `{"button":"on","mpu_accel":{"x":..,"y":..,"z":..}}` |
| `<prefix>/telemetry`     | broker -> device | LED command                                          |

## Build

```
cp secrets.conf.example secrets.conf   # fill in Wi-Fi + broker
 west build -p always -b esp32s3_devkitc/esp32s3/procpu  ./   -- -DEXTRA_CONF_FILE=secrets.conf
west flash
```

Requires Zephyr v4.x and a working west workspace.

## Dashboard

See `dashboard/`

## Notes

- Stack size is bumped (`CONFIG_MAIN_STACK_SIZE=4096`): MQTT init + DNS
  overflows the 2048 and crashes right after DHCP.
- Broker is plain MQTT (no TLS) for now.