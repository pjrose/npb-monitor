#pragma once

#include <Arduino.h>

// Fill these in before flashing. The device serves the web UI on this LAN.
static constexpr const char* WIFI_SSID     = "YOUR_WIFI_SSID";
static constexpr const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static constexpr const char* MDNS_NAME     = "npb";
static constexpr const char* TIME_ZONE     = "CST6CDT,M3.2.0,M11.1.0";

// Mean Well NPB CAN addressing. Address 0x00 matched the working WiCAN setup.
static constexpr uint8_t NPB_ADDRESS = 0x00;

// CoreS3 Port.C: yellow G17, white G18. Unit CAN pinout: CAN_TX, CAN_RX.
static constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_17;
static constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_18;
static constexpr uint32_t CAN_BITRATE  = 250000;

// CoreS3 Port.A I2C: SDA G2, SCL G1. Used by Unit ACSSR in I2C mode.
static constexpr uint8_t ACSSR_SDA_PIN = 2;
static constexpr uint8_t ACSSR_SCL_PIN = 1;

// The ACSSR relay is assumed ON when AC input should be applied to the charger.
static constexpr uint32_t AC_POWER_CYCLE_MS = 10000;

static constexpr uint32_t POLL_INTERVAL_MS = 1000;
static constexpr uint32_t POLL_GAP_MS      = 60;
static constexpr size_t HISTORY_SECONDS    = 24UL * 60UL * 60UL;
static constexpr float SD_MAX_USED_FRACTION = 0.75f;
