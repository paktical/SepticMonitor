#pragma once

// Selects the build profile. platformio.ini defines exactly one of
// CONFIG_DEV / CONFIG_RELEASE via build_flags.
//
//   env:dev      -> config_dev.h      (local broker, local WiFi, chatty logs)
//   env:release  -> config_release.h  (production broker, quiet logs)

#if defined(CONFIG_DEV) && defined(CONFIG_RELEASE)
#error "Define only one of CONFIG_DEV / CONFIG_RELEASE"
#elif defined(CONFIG_DEV)
#include "config_dev.h"
#elif defined(CONFIG_RELEASE)
#include "config_release.h"
#else
#error "No build profile selected. Build with -DCONFIG_DEV or -DCONFIG_RELEASE"
#endif

// ---------- Values shared by every profile ----------

// Hardware wiring / peripherals
#define I2C_SDA_PIN        8
#define I2C_SCL_PIN        9
#define ADS1115_I2C_ADDR   0x48

// Firmware identity
#define FW_VERSION         "1.0.0"

// MQTT topics, derived from the profile's TOPIC_PREFIX
#define TOPIC_TELEMETRY    TOPIC_PREFIX "/telemetry"
#define TOPIC_STATUS       TOPIC_PREFIX "/status"

// Serial console
#define SERIAL_BAUD        115200

// Sanity check: every profile must supply these.
#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) || !defined(MQTT_BROKER) || \
    !defined(MQTT_PORT) || !defined(DEVICE_ID) || !defined(TOPIC_PREFIX) ||    \
    !defined(WIFI_CONNECT_TIMEOUT_MS) ||                                       \
    !defined(SPREAD_THRESHOLD) || !defined(SAMPLE_MS) ||                       \
    !defined(PUBLISH_MS) || !defined(MQTT_RETRY_MS) || !defined(LOG_VERBOSE)
#error "Active config profile is missing one or more required settings"
#endif
