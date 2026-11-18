#pragma once
#include <Arduino.h>
constexpr int TFT_CS = D1, TFT_DC = D2, TFT_RST = -1, TFT_LED = D8;
constexpr uint8_t TFT_ROTATION = 0;
constexpr char DEFAULT_BROKER[] = "";
constexpr char DEFAULT_PANEL_ID[] = "heating";
constexpr char DEFAULT_TIMEZONE[] = "GMT0BST,M3.5.0/1,M10.5.0";
constexpr uint32_t STALE_SECONDS = 90;
constexpr size_t MQTT_BUFFER_BYTES = 4096;
