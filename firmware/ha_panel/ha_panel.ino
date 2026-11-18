#include <Arduino.h>
#include <SPI.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <time.h>
#include "config.h"
#include "fonts/PanelFonts.h"

// The LCD stores its own pixels. Changed glyphs are drawn directly over SPI.
constexpr uint16_t BG = 0x0884, CARD = 0x0841, LINE = 0x4A49;
constexpr uint16_t WHITE = 0xFFFF, MUTED = 0x9CF3, CYAN = 0x66BF;
constexpr uint16_t GREEN = 0x6711, RED = 0xFB8D, AMBER = 0xFDEB;
constexpr uint16_t FORECAST_BG = CARD, FORECAST_BORDER = LINE;
constexpr uint32_t CONFIG_MAGIC = 0x48504131;
struct NetworkSettings {
  uint32_t magic;
  char broker[64], port[6], user[64], password[96], panel[25];
  uint32_t checksum;
};
struct TextCache {
  char text[40] = {};
  const PanelFont* font = nullptr;
  uint16_t color = 0, background = 0, width = 0;
  uint8_t height = 0;
  int8_t top = 0;
};
struct TextMetrics {
  uint16_t width = 0;
  uint8_t height = 0;
  int8_t top = 0;
  bool supported = true;
};
struct Reading {
  char label[29] = {}, value[19] = {}, unit[9] = {};
  uint16_t color = MUTED;
  uint8_t circuitRole = 255;
};

struct ForecastReading {
  uint32_t time = 0;
  char temperature[16] = {};
  int probability = -1;
};
struct WeatherReading {
  char condition[25] = {}, conditionText[33] = {};
  char temperature[16] = {}, unit[9] = {};
  char sensorTemperature[16] = {}, sensorUnit[9] = {};
  char rainSummary[40] = {}, forecastKind[16] = {};
  int probability = -1;
  uint32_t nextRain = 0;
  ForecastReading forecast[3];
  uint8_t forecastCount = 0;
};
WeatherReading weather;
bool weatherEnabled = false;
uint8_t activePage = 0;
uint16_t pageSeconds = 20;
uint32_t lastPageChange = 0;
TextCache weatherText[12];
char iconCondition[25] = {};
bool iconStale = false;
bool isStale();
void renderPageTitle();
void renderWeather(bool stale);
void renderHeatingDiagram(bool stale);
void drawHeatingDiagram();
void showPage(uint8_t page);

NetworkSettings settings;
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
WiFiManager wifiManager;
WiFiManagerParameter brokerField("broker", "MQTT broker IP / hostname", DEFAULT_BROKER, 63);
WiFiManagerParameter portField("port", "MQTT port", "1883", 5);
WiFiManagerParameter userField("user", "MQTT username", "", 63);
WiFiManagerParameter passwordField("mqtt_password", "MQTT password (blank keeps saved)", "", 95, "type='password'");
WiFiManagerParameter panelField("panel", "Panel ID in Home Assistant", DEFAULT_PANEL_ID, 24);
void updateClock();

// PubSubClient waits for CONNACK and packet fragments through available().
// Service the display clock during those bounded waits as well as in loop().
class PanelWiFiClient : public WiFiClient {
 public:
  int available() override {
    updateClock();
    return WiFiClient::available();
  }
};
PanelWiFiClient tcp;
PubSubClient mqtt(tcp);
Reading readings[6];
TextCache labels[6], values[6], units[6], clockCache, dateCache, titleCache, statusCache;
char title[29] = "Home Assistant", timezone[96] = {};
char stateTopic[64], availabilityTopic[64], clientId[32], apName[32], apPassword[20];
uint8_t readingCount = 0, slots = 6;
uint32_t lastMqttAttempt, lastSignal, lastMessage, disconnectedSince, portalSince, receivedTimestamp;
time_t displayedSecond = -1;
int displayedBars = -2;
bool received = false, wasStale = true, portalRequested = false, ntpStarted = false;
bool restartRequested = false;
uint32_t restartAt;


void displayText(const char* input, char* output, size_t capacity) {
  size_t used = 0;
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(input);
  while (*cursor && used + 1 < capacity) {
    if (cursor[0] == 0xC2 && cursor[1] == 0xB0) {
      output[used++] = char(0xB0);
      cursor += 2;
    } else if (*cursor == 0xB0 || *cursor == 0xF8) {
      output[used++] = char(0xB0);
      ++cursor;
    } else if (*cursor >= 0x80) {
      output[used++] = '?';
      ++cursor;
      while ((*cursor & 0xC0) == 0x80) ++cursor;
    } else {
      output[used++] = *cursor >= 32 && *cursor < 127 ? char(*cursor) : ' ';
      ++cursor;
    }
  }
  output[used] = 0;
}

void copyUnit(char* output, size_t capacity, const char* input) {
  // Version 1.1 sent ?C after its degree symbol was lost in text encoding.
  // Also accept the standard Unicode Celsius and Fahrenheit ligatures.
  if (!strcmp(input, "?C") || !strcmp(input, "\xE2\x84\x83")) {
    strlcpy(output, "\xC2\xB0" "C", capacity);
  } else if (!strcmp(input, "?F") || !strcmp(input, "\xE2\x84\x89")) {
    strlcpy(output, "\xC2\xB0" "F", capacity);
  } else {
    strlcpy(output, input, capacity);
  }
}

PanelGlyph fontGlyph(const PanelFont* font, uint8_t character) {
  const uint8_t index = character == 0xB0 ? 95 :
      character >= 32 && character <= 126 ? character - 32 : '?' - 32;
  PanelGlyph glyph;
  memcpy_P(&glyph, font->glyphs + index, sizeof(glyph));
  return glyph;
}

TextMetrics measurePanelText(const PanelFont* font, const char* text) {
  TextMetrics metrics;
  int top = 127, bottom = 0, pen = 0;
  bool numeric = text[0] != 0;
  for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text); *cursor; ++cursor) {
    const PanelGlyph glyph = fontGlyph(font, *cursor);
    if (!glyph.advance) metrics.supported = false;
    if (glyph.height) {
      top = min(top, int(glyph.yOffset));
      bottom = max(bottom, int(glyph.yOffset + glyph.height));
      metrics.width = max(int(metrics.width), pen + glyph.xOffset + glyph.width);
    }
    pen += glyph.advance;
    numeric = numeric && (*cursor == 0xB0 || strchr("0123456789:.-+ %eECF", *cursor));
  }
  metrics.width = max(int(metrics.width), pen);
  // Tabular digit advances and a shared baseline keep the clock and numbers
  // stationary while the proportional letters retain their natural spacing.
  if (numeric) {
    for (uint8_t character = '0'; character <= '9'; ++character) {
      const PanelGlyph glyph = fontGlyph(font, character);
      top = min(top, int(glyph.yOffset));
      bottom = max(bottom, int(glyph.yOffset + glyph.height));
    }
  }
  if (top < bottom) {
    metrics.top = top;
    metrics.height = bottom - top;
  }
  return metrics;
}

uint16_t blendTextColor(uint16_t foreground, uint16_t background, uint8_t alpha) {
  const uint8_t inverse = 15 - alpha;
  const uint16_t red = ((((foreground >> 11) & 31) * alpha +
                         ((background >> 11) & 31) * inverse + 7) / 15);
  const uint16_t green = ((((foreground >> 5) & 63) * alpha +
                           ((background >> 5) & 63) * inverse + 7) / 15);
  const uint16_t blue = (((foreground & 31) * alpha + (background & 31) * inverse + 7) / 15);
  return (red << 11) | (green << 5) | blue;
}

void textAt(TextCache& cache, int x, int y, int width, const char* input,
            uint16_t color, uint16_t background, uint8_t preferred) {
  static uint16_t scanline[240];
  const PanelFont* fonts[] = {&PanelInter10, &PanelInter11, &PanelInter18, &PanelInter26, &PanelInter33, &PanelInter40};
  const uint8_t maximumHeights[] = {13, 13, 22, 25, 30, 34};
  char text[40];
  displayText(input, text, sizeof(text));
  uint8_t style = min(uint8_t(5), preferred);
  const int maximumHeight = min(int(maximumHeights[style]), tft.height() - y);
  TextMetrics metrics = measurePanelText(fonts[style], text);
  while (style && (!metrics.supported || metrics.width > width || metrics.height > maximumHeight)) {
    --style;
    metrics = measurePanelText(fonts[style], text);
  }
  size_t length = strlen(text);
  while (length && metrics.width > width) {
    text[--length] = 0;
    metrics = measurePanelText(fonts[style], text);
  }
  const PanelFont* font = fonts[style];
  if (cache.font == font && cache.color == color && cache.background == background &&
      !strcmp(cache.text, text)) return;

  int skip = 0;
  if (cache.font == font && cache.color == color && cache.background == background &&
      cache.top == metrics.top && cache.height == metrics.height) {
    size_t common = 0;
    while (text[common] && text[common] == cache.text[common]) {
      skip += fontGlyph(font, uint8_t(text[common])).advance;
      ++common;
    }
  }
  const int right = min(width, int(max(cache.width, metrics.width)));
  const int height = min(maximumHeight, int(max(cache.height, metrics.height)));
  skip = min(skip, right);
  const int span = right - skip;
  if (span > 0 && height > 0) {
    uint16_t palette[16];
    for (uint8_t alpha = 0; alpha < 16; ++alpha) palette[alpha] = blendTextColor(color, background, alpha);
    tft.startWrite();
    tft.setAddrWindow(x + skip, y, span, height);
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < span; ++column) scanline[column] = background;
      int pen = 0;
      for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text); *cursor; ++cursor) {
        const PanelGlyph glyph = fontGlyph(font, *cursor);
        const int glyphRow = row + metrics.top - glyph.yOffset;
        if (glyphRow >= 0 && glyphRow < glyph.height) {
          const int start = max(0, skip - pen - glyph.xOffset);
          const int end = min(int(glyph.width), right - pen - glyph.xOffset);
          for (int column = start; column < end; ++column) {
            const uint32_t pixel = uint32_t(glyphRow) * glyph.width + column;
            const uint8_t packed = pgm_read_byte(font->bitmap + glyph.offset + pixel / 2);
            const uint8_t alpha = (pixel & 1) ? packed & 15 : packed >> 4;
            scanline[pen + glyph.xOffset + column - skip] = palette[alpha];
          }
        }
        pen += glyph.advance;
      }
      tft.writePixels(scanline, span);
    }
    tft.endWrite();
  }
  strlcpy(cache.text, text, sizeof(cache.text));
  cache.font = font;
  cache.color = color;
  cache.background = background;
  cache.width = metrics.width;
  cache.height = metrics.height;
  cache.top = metrics.top;
}

void drawGrid() {
  tft.fillRect(0, 80, 240, 225, BG);
  const int height = slots == 4 ? 105 : 68;
  for (int i = 0; i < slots; ++i) {
    const int x = 8 + (i % 2) * 116, y = 82 + (i / 2) * (height + 6);
    tft.fillRoundRect(x, y, 108, height, 6, CARD);
    tft.drawRoundRect(x, y, 108, height, 6, LINE);
    labels[i] = TextCache{};
    values[i] = TextCache{};
    units[i] = TextCache{};
  }
}

void renderReadings(bool stale) {
  if (activePage != 0) return;
  const int height = slots == 4 ? 105 : 68;
  for (uint8_t i = 0; i < slots; ++i) {
    const int x = 15 + (i % 2) * 116, y = 91 + (i / 2) * (height + 6);
    const Reading& row = readings[i];
    textAt(labels[i], x, y, 94, i < readingCount ? row.label : "", WHITE, CARD, 1);
    char temperature[40] = {};
    if (i < readingCount) {
      snprintf(temperature, sizeof(temperature), "%s%s%s", row.value,
               row.unit[0] ? " " : "", row.unit);
    }
    // Fit the value and its unit together; native font sizes keep edges smooth.
    textAt(values[i], x, y + 18, 94, temperature,
           stale ? MUTED : row.color, CARD, 3);
  }
}

void renderPageTitle() {
  char caption[40];
  const uint8_t pageNumber = activePage >= 2 ? activePage + (weatherEnabled ? 1 : 0) : activePage + 1;
  snprintf(caption, sizeof(caption), "%s  %u/%u",
           activePage == 3 ? "Water tank" : activePage == 2 ? "Boiler" : activePage == 1 ? "Weather" : title,
           pageNumber, weatherEnabled ? 4 : 3);
  textAt(titleCache, 12, 64, 216, caption, WHITE, BG, 1);
}

void drawWeatherIcon(bool stale) {
  if (!strcmp(iconCondition, weather.condition) && iconStale == stale) return;
  strlcpy(iconCondition, weather.condition, sizeof(iconCondition));
  iconStale = stale;
  tft.fillRect(166, 91, 60, 73, CARD);
  const uint16_t light = stale ? MUTED : WHITE;
  const uint16_t sun = stale ? MUTED : AMBER;
  if (!strcmp(weather.condition, "unavailable") || !weather.condition[0]) {
    tft.drawLine(181, 110, 210, 139, MUTED);
    tft.drawLine(181, 139, 210, 110, MUTED);
    return;
  }
  if (!strcmp(weather.condition, "clear-night")) {
    tft.fillCircle(194, 122, 18, light);
    tft.fillCircle(202, 115, 16, CARD);
    return;
  }
  const bool sunny = !strcmp(weather.condition, "sunny");
  if (sunny || !strcmp(weather.condition, "partlycloudy")) {
    tft.fillCircle(190, 116, 12, sun);
    tft.drawLine(190, 96, 190, 101, sun);
    tft.drawLine(190, 131, 190, 136, sun);
    tft.drawLine(170, 116, 175, 116, sun);
    tft.drawLine(205, 116, 210, 116, sun);
    tft.drawLine(176, 102, 180, 106, sun);
    tft.drawLine(200, 126, 204, 130, sun);
  }
  if (sunny) return;
  tft.fillCircle(181, 126, 9, light);
  tft.fillCircle(194, 119, 13, light);
  tft.fillCircle(208, 127, 10, light);
  tft.fillRect(179, 125, 30, 12, light);
  if (strstr(weather.condition, "rain") || !strcmp(weather.condition, "pouring")) {
    const uint16_t rain = stale ? MUTED : CYAN;
    for (int i = 0; i < 3; ++i) tft.drawLine(183 + i * 12, 143, 179 + i * 12, 153, rain);
  }
  if (strstr(weather.condition, "lightning")) {
    tft.fillTriangle(197, 138, 187, 150, 198, 148, sun);
    tft.fillTriangle(198, 146, 190, 160, 203, 146, sun);
  }
  if (!strcmp(weather.condition, "fog") || strstr(weather.condition, "windy")) {
    tft.drawFastHLine(175, 144, 39, MUTED);
    tft.drawFastHLine(181, 150, 34, MUTED);
  }
  if (!strcmp(weather.condition, "snowy") || !strcmp(weather.condition, "hail")) {
    for (int i = 0; i < 3; ++i) {
      tft.drawFastVLine(181 + i * 13, 143, 7, light);
      tft.drawFastHLine(178 + i * 13, 146, 7, light);
    }
  }
}

void drawWeatherBase() {
  tft.fillRect(0, 80, 240, 225, BG);
  tft.fillRoundRect(8, 82, 224, 92, 6, CARD);
  tft.drawRoundRect(8, 82, 224, 92, 6, LINE);
  tft.fillRoundRect(8, 180, 224, 40, 6, CARD);
  tft.drawRoundRect(8, 180, 224, 40, 6, LINE);
  tft.fillRoundRect(8, 226, 224, 40, 6, CARD);
  tft.drawRoundRect(8, 226, 224, 40, 6, LINE);
  for (int i = 0; i < 3; ++i) {
    tft.fillRoundRect(8 + i * 76, 272, 72, 30, 5, FORECAST_BG);
    tft.drawRoundRect(8 + i * 76, 272, 72, 30, 5, FORECAST_BORDER);
  }
  for (TextCache& cache : weatherText) cache = TextCache{};
  iconCondition[0] = 0;
  TextCache providerCaption, sensorCaption;
  textAt(providerCaption, 18, 153, 138, "Weather app", WHITE, CARD, 1);
  textAt(sensorCaption, 18, 194, 108, "Local sensor", WHITE, CARD, 1);
}

void renderWeather(bool stale) {
  if (activePage != 1) return;
  const uint16_t valueColor = stale ? MUTED : AMBER;
  char text[40];
  textAt(weatherText[0], 18, 92, 146, weather.conditionText, stale ? MUTED : WHITE, CARD, 2);
  snprintf(text, sizeof(text), "%s %s", weather.temperature, weather.unit);
  textAt(weatherText[1], 18, 114, 146, text, valueColor, CARD, 5);
  snprintf(text, sizeof(text), "%s %s", weather.sensorTemperature, weather.sensorUnit);
  textAt(weatherText[2], 130, 190, 94, text, valueColor, CARD, 2);
  textAt(weatherText[3], 18, 233, 204, weather.rainSummary, stale ? MUTED : WHITE, CARD, 1);
  char chance[12] = {};
  if (weather.probability >= 0) snprintf(chance, sizeof(chance), "%d%%", weather.probability);
  if (weather.nextRain) {
    const time_t stamp = weather.nextRain;
    struct tm local;
    localtime_r(&stamp, &local);
    char when[16];
    strftime(when, sizeof(when), !strcmp(weather.forecastKind, "hourly") ? "%H:%M" : "%a %d", &local);
    snprintf(text, sizeof(text), "Rain ~%s%s%s", when, chance[0] ? " | max " : "", chance);
  } else if (chance[0]) {
    snprintf(text, sizeof(text), "Max rain chance %s", chance);
  } else {
    strlcpy(text, weather.forecastCount ? "Rain chance not provided" : "Waiting for forecast data", sizeof(text));
  }
  textAt(weatherText[4], 18, 250, 204, text, stale ? MUTED : WHITE, CARD, 1);
  for (uint8_t i = 0; i < 3; ++i) {
    char when[12] = "--";
    char detail[24] = "--";
    if (i < weather.forecastCount) {
      const time_t stamp = weather.forecast[i].time;
      struct tm local;
      localtime_r(&stamp, &local);
      strftime(when, sizeof(when), !strcmp(weather.forecastKind, "hourly") ? "%H:%M" : "%a %d", &local);
      if (weather.forecast[i].probability >= 0) {
        snprintf(detail, sizeof(detail), "%s %d%%", weather.forecast[i].temperature, weather.forecast[i].probability);
      } else {
        snprintf(detail, sizeof(detail), "%s %s", weather.forecast[i].temperature, weather.unit);
      }
    }
    textAt(weatherText[5 + i * 2], 15 + i * 76, 275, 58, when,
           stale ? MUTED : WHITE, FORECAST_BG, 1);
    textAt(weatherText[6 + i * 2], 15 + i * 76, 289, 58, detail,
           stale ? MUTED : AMBER, FORECAST_BG, 1);
  }
  drawWeatherIcon(stale);
}

// Dedicated circuit artwork, using the existing anti-aliased Inter renderer.
constexpr uint16_t DIAGRAM_BODY = 0x18E4, DIAGRAM_EDGE = 0x7C11;
constexpr uint16_t DIAGRAM_CTRL = 0x2987, DIAGRAM_DETAIL = 0x3A09;
constexpr uint16_t DIAGRAM_COIL = 0xAB67, DIAGRAM_PILL = 0x10C3;
TextCache diagramValues[6];
uint16_t diagramPipeColors[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

void diagramLabel(int x, int y, int width, const char* label, uint16_t background = CARD) {
  TextCache cache;
  textAt(cache, x, y, width, label, WHITE, background, 1);
}

void drawHeatingDiagram() {
  const bool boiler = activePage == 2;
  tft.fillRect(0, 80, 240, 225, BG);
  tft.fillRoundRect(4, 82, 232, 220, 6, CARD);
  tft.drawRoundRect(4, 82, 232, 220, 6, LINE);
  diagramLabel(108, 94, 120, "FLOW");
  tft.fillRoundRect(104, 158, 129, 58, 10, DIAGRAM_PILL);
  tft.drawRoundRect(104, 158, 129, 58, 10, LINE);
  tft.drawTriangle(116, 169, 112, 176, 120, 176, WHITE);
  diagramLabel(125, 168, 100, "DELTA", DIAGRAM_PILL);
  diagramLabel(108, 231, 120, "RETURN");
  if (boiler) {
    // Light enamel cabinet with a shaded side, dark fascia and service door.
    constexpr uint16_t shell = 0xCE79, face = 0xEF7D, edge = 0xA514;
    constexpr uint16_t side = 0xBDF7, door = 0xDEFB, ink = 0x4208;
    tft.fillRoundRect(12, 90, 64, 204, 6, shell);
    tft.drawRoundRect(12, 90, 64, 204, 6, edge);
    tft.fillRoundRect(15, 93, 57, 196, 4, face);
    tft.fillRect(69, 96, 4, 189, side);
    tft.drawFastVLine(14, 98, 183, WHITE);
    tft.drawFastHLine(19, 93, 47, WHITE);
    tft.fillRoundRect(18, 99, 49, 27, 3, DIAGRAM_CTRL);
    tft.drawCircle(30, 112, 7, WHITE);
    tft.drawCircle(30, 112, 5, DIAGRAM_EDGE);
    tft.drawLine(30, 112, 33, 108, WHITE);
    tft.drawRoundRect(45, 106, 8, 11, 2, DIAGRAM_EDGE);
    tft.drawFastVLine(49, 108, 4, WHITE);
    tft.fillCircle(60, 112, 2, DIAGRAM_EDGE);
    tft.fillRoundRect(19, 136, 48, 34, 3, ink);
    diagramLabel(24, 141, 42, "GRANT", ink);
    diagramLabel(21, 155, 45, "VORTEX", ink);
    tft.fillRoundRect(19, 183, 47, 96, 3, door);
    tft.drawRoundRect(19, 183, 47, 96, 3, edge);
    tft.fillRoundRect(55, 192, 5, 12, 2, ink);
    for (int y = 244; y <= 260; y += 8) tft.drawFastHLine(27, y, 31, edge);
    tft.fillRoundRect(17, 285, 53, 9, 2, ink);
  } else {
    constexpr uint16_t tankGreen = 0x1BA8, tankEdge = 0x65F2;
    tft.fillRoundRect(14, 90, 62, 204, 20, tankGreen);
    tft.drawRoundRect(14, 90, 62, 204, 20, tankEdge);
    tft.drawRoundRect(15, 91, 60, 202, 19, tankEdge);
    tft.fillRect(26, 128, 50, 4, DIAGRAM_COIL);
    tft.fillRect(26, 264, 50, 4, DIAGRAM_COIL);
    for (int step = 0; step < 13; ++step) {
      const int x1 = step % 2 ? 62 : 26, x2 = step % 2 ? 26 : 62;
      for (int shift = 0; shift < 3; ++shift)
        tft.drawLine(x1 + shift, 129 + step * 10, x2 + shift, 139 + step * 10, DIAGRAM_COIL);
    }
    for (int shift = 0; shift < 3; ++shift)
      tft.drawLine(62 + shift, 259, 26 + shift, 265, DIAGRAM_COIL);
  }
  for (TextCache& cache : diagramValues) cache = TextCache{};
  for (uint16_t& color : diagramPipeColors) color = 0xFFFF;
}

bool diagramTemperature(uint8_t role, float& value) {
  for (uint8_t i = 0; i < readingCount; ++i) {
    const Reading& row = readings[i];
    if (row.circuitRole != role ||
        (strcmp(row.unit, "\xC2\xB0" "C") && strcmp(row.unit, "C"))) continue;
    char* end = nullptr;
    const float parsed = strtof(row.value, &end);
    if (end == row.value || *end || !isfinite(parsed)) return false;
    value = roundf(parsed * 10.0f) / 10.0f;
    return true;
  }
  return false;
}

uint16_t diagramTemperatureColor(float value) {
  return value > 50.0f ? RED : value >= 30.0f ? AMBER : CYAN;
}

void renderHeatingDiagram(bool stale) {
  if (activePage < 2) return;
  const uint8_t circuit = activePage == 2 ? 0 : 1;
  float flow = 0, returning = 0;
  const bool flowValid = !stale && diagramTemperature(circuit * 3, flow);
  const bool returnValid = !stale && diagramTemperature(circuit * 3 + 1, returning);
  const float temperatures[] = {flow, flow - returning, returning};
  const bool valid[] = {flowValid, flowValid && returnValid, returnValid};
  const int ys[] = {117, 185, 256};
  const int xs[] = {108, 125, 125};
  const int widths[] = {122, 104, 105};
  for (uint8_t field = 0; field < 3; ++field) {
    char text[32] = "--";
    uint16_t color = MUTED;
    if (valid[field]) {
      snprintf(text, sizeof(text), "%.1f\xC2\xB0" "C", temperatures[field]);
      color = field == 1 ? WHITE : diagramTemperatureColor(temperatures[field]);
    }
    textAt(diagramValues[field], xs[field], ys[field], widths[field], text,
           color, field == 1 ? DIAGRAM_PILL : CARD, field == 0 ? 4 : 3);
  }
  for (uint8_t side = 0; side < 2; ++side) {
    const uint16_t color = valid[side == 0 ? 0 : 2] ?
        diagramTemperatureColor(temperatures[side == 0 ? 0 : 2]) : MUTED;
    if (diagramPipeColors[side] == color) continue;
    diagramPipeColors[side] = color;
    const int y = side ? 265 : 129;
    tft.fillRect(76, y - 7, 28, 15, CARD);
    tft.fillRect(76, y - 3, 28, 7, color);
    const bool right = circuit == side;
    if (right) tft.fillTriangle(86, y - 7, 94, y, 86, y + 7, color);
    else tft.fillTriangle(94, y - 7, 86, y, 94, y + 7, color);
  }
}

void showPage(uint8_t page) {
  activePage = page;
  lastPageChange = millis();
  if (page >= 2) {
    drawHeatingDiagram();
    renderHeatingDiagram(isStale());
  } else if (page == 1) {
    drawWeatherBase();
    renderWeather(isStale());
  } else {
    drawGrid();
    renderReadings(isStale());
  }
  renderPageTitle();
}

void updateClock() {
  const time_t now = time(nullptr);
  if (now < 1704067200 || now == displayedSecond) return;
  displayedSecond = now;
  struct tm local;
  localtime_r(&now, &local);
  const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char date[20], clock[12];
  snprintf(date, sizeof(date), "%s %s %02d", days[local.tm_wday], months[local.tm_mon], local.tm_mday);
  strftime(clock, sizeof(clock), "%H:%M:%S", &local);
  textAt(dateCache, 12, 8, 180, date, WHITE, BG, 1);
  textAt(clockCache, 12, 25, 216, clock, WHITE, BG, 5);
}

bool isStale() {
  if (!received || millis() - lastMessage > STALE_SECONDS * 1000UL) return true;
  const time_t now = time(nullptr);
  return now > 1704067200 && (receivedTimestamp < uint32_t(now - STALE_SECONDS) ||
                             receivedTimestamp > uint32_t(now + 300));
}

uint16_t color565(const char* value) {
  if (!value || strlen(value) != 7 || value[0] != '#') return CYAN;
  for (int i = 1; i < 7; ++i) if (!isxdigit(value[i])) return CYAN;
  const uint32_t rgb = strtoul(value + 1, nullptr, 16);
  return tft.color565(rgb >> 16, rgb >> 8, rgb);
}

void receivePanel(char* topic, uint8_t* payload, unsigned int length) {
  if (strcmp(topic, stateTopic) || length > MQTT_BUFFER_BYTES - 100) return;
  JsonDocument document;
  if (deserializeJson(document, payload, length)) return;
  if (document["v"].as<int>() != 1 || !document["rows"].is<JsonArray>()) return;
  const uint8_t newSlots = document["slots"] | 6;
  const JsonArray rows = document["rows"].as<JsonArray>();
  if ((newSlots != 4 && newSlots != 6) || rows.size() > newSlots) return;
  for (JsonVariant row : rows) {
    if (!row["label"].is<const char*>() || !row["value"].is<const char*>() ||
        !row["unit"].is<const char*>()) return;
  }
  if (!document["updated"].is<uint32_t>()) return;
  if (slots != newSlots) {
    slots = newSlots;
    if (activePage == 0) drawGrid();
  }
  readingCount = rows.size();
  uint8_t i = 0;
  for (JsonVariant row : rows) {
    strlcpy(readings[i].label, row["label"], sizeof(readings[i].label));
    // Confirmed plumbing: probe A is flow; probe B is return.
    // Upgrade legacy default labels while preserving custom labels and deltas.
    if (!strcmp(readings[i].label, "Boiler probe A")) {
      strlcpy(readings[i].label, "Boiler flow", sizeof(readings[i].label));
    } else if (!strcmp(readings[i].label, "Boiler probe B")) {
      strlcpy(readings[i].label, "Boiler return", sizeof(readings[i].label));
    }
    strlcpy(readings[i].value, row["value"], sizeof(readings[i].value));
    copyUnit(readings[i].unit, sizeof(readings[i].unit), row["unit"]);
    readings[i].color = color565(row["color"] | "#65d7ff");
    // Both sides of the heating circuits use temperature colours; exclude deltas.
    String label = readings[i].label;
    label.toLowerCase();
    String entity = row["entity"] | "";
    const bool circuitTemperature =
        (label.indexOf("flow") >= 0 || label.indexOf("return") >= 0 ||
         label.indexOf("rtn") >= 0 || entity.endsWith("_flow") ||
         entity.endsWith("_return")) && label.indexOf("delta") < 0 &&
        label.indexOf("a-b") < 0;
    // Identify diagram roles independently of row order. Never reuse a delta
    // as its source temperature, even though HA supplies the same entity ID.
    readings[i].circuitRole = 255;
    if (label.indexOf("delta") >= 0 || label.indexOf("a-b") >= 0) {
      if (label.indexOf("boiler") >= 0) readings[i].circuitRole = 2;
      else if (label.indexOf("hot") >= 0) readings[i].circuitRole = 5;
    } else if (entity == "sensor.boiler_flow" || label == "boiler flow") {
      readings[i].circuitRole = 0;
    } else if (entity == "sensor.boiler_return" || label == "boiler return") {
      readings[i].circuitRole = 1;
    } else if (entity == "sensor.water_tank_flow" || label == "hot press flow") {
      readings[i].circuitRole = 3;
    } else if (entity == "sensor.water_tank_return" || label == "hot press rtn" || label == "hot press return") {
      readings[i].circuitRole = 4;
    }
    // Support descriptive labels, including the legacy spelling alias,
    // without relying on row position.
    const bool boilerLabel = label.indexOf("boiler") >= 0 || label.indexOf("bolier") >= 0;
    const bool tankLabel = label.indexOf("water tank") >= 0 || label.indexOf("hot press") >= 0;
    if (boilerLabel || tankLabel) {
      const uint8_t base = boilerLabel ? 0 : 3;
      if (label.indexOf("delta") >= 0 || label.indexOf("a-b") >= 0) readings[i].circuitRole = base + 2;
      else if (label.indexOf("return") >= 0 || label.indexOf("rtn") >= 0) readings[i].circuitRole = base + 1;
      else if (label.indexOf("flow") >= 0) readings[i].circuitRole = base;
    }
    char* end = nullptr;
    const float temperature = strtof(readings[i].value, &end);
    if (circuitTemperature && end != readings[i].value && *end == 0 && isfinite(temperature) &&
        (!strcmp(readings[i].unit, "\xC2\xB0" "C") || !strcmp(readings[i].unit, "C"))) {
      readings[i].color = temperature > 50.0f ? RED : temperature >= 30.0f ? AMBER : CYAN;
    }
    ++i;
  }
  strlcpy(title, document["title"] | "Home Assistant", sizeof(title));
  const char* newTimezone = document["timezone"] | DEFAULT_TIMEZONE;
  if (strlen(newTimezone) < sizeof(timezone) && strcmp(timezone, newTimezone)) {
    strlcpy(timezone, newTimezone, sizeof(timezone));
    setenv("TZ", timezone, 1);
    tzset();
    displayedSecond = -1;
  }
  lastMessage = millis();
  receivedTimestamp = document["updated"].as<uint32_t>();
  received = true;
  wasStale = isStale();
  const bool previouslyEnabled = weatherEnabled;
  JsonObject details = document["weather"].as<JsonObject>();
  weatherEnabled = !details.isNull() && (details["enabled"] | false);
  pageSeconds = constrain(document["page_seconds"] | 20, 10, 300);
  if (weatherEnabled) {
    strlcpy(weather.condition, details["condition"] | "unavailable", sizeof(weather.condition));
    strlcpy(weather.conditionText, details["condition_text"] | "Weather unavailable", sizeof(weather.conditionText));
    strlcpy(weather.temperature, details["temperature"] | "--", sizeof(weather.temperature));
    copyUnit(weather.unit, sizeof(weather.unit), details["unit"] | "");
    strlcpy(weather.sensorTemperature, details["sensor_temperature"] | "--", sizeof(weather.sensorTemperature));
    copyUnit(weather.sensorUnit, sizeof(weather.sensorUnit), details["sensor_unit"] | "");
    strlcpy(weather.rainSummary, details["rain_summary"] | "Forecast unavailable", sizeof(weather.rainSummary));
    strlcpy(weather.forecastKind, details["forecast_kind"] | "hourly", sizeof(weather.forecastKind));
    weather.probability = details["rain_probability"].isNull() ? -1 : details["rain_probability"].as<int>();
    weather.nextRain = details["next_rain"] | 0UL;
    JsonArray forecast = details["forecast"].as<JsonArray>();
    weather.forecastCount = min(size_t(3), forecast.size());
    for (uint8_t index = 0; index < weather.forecastCount; ++index) {
      weather.forecast[index].time = forecast[index]["time"] | 0UL;
      strlcpy(weather.forecast[index].temperature, forecast[index]["temperature"] | "--", sizeof(weather.forecast[index].temperature));
      weather.forecast[index].probability = forecast[index]["probability"].isNull() ? -1 : forecast[index]["probability"].as<int>();
    }
  }
  if (weatherEnabled != previouslyEnabled) {
    showPage(0);
  } else {
    renderPageTitle();
    renderReadings(wasStale);
    renderWeather(wasStale);
    renderHeatingDiagram(wasStale);
  }
  if (activePage >= 2) {
    float probe;
    const unsigned mapped = unsigned(diagramTemperature(0, probe)) +
        unsigned(diagramTemperature(1, probe)) + unsigned(diagramTemperature(3, probe)) +
        unsigned(diagramTemperature(4, probe));
    Serial.printf("Heating diagram: %u/4 live circuit probes\n", mapped);
  }
  Serial.printf("Panel update: %u readings, weather %s, page %u/%u\n",
                readingCount, weatherEnabled ? "on" : "off", activePage >= 2 ? activePage + (weatherEnabled ? 1 : 0) : activePage + 1, weatherEnabled ? 4 : 3);
}

uint32_t checksum(const NetworkSettings& value) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(NetworkSettings, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619UL;
  return hash;
}

bool validPanelId(const char* id) {
  const size_t length = strlen(id);
  if (!length || length > 24) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!(id[i] >= 'a' && id[i] <= 'z') && !isdigit(id[i]) && id[i] != '_' && id[i] != '-') return false;
  }
  return true;
}

void loadSettings() {
  EEPROM.begin(512);
  EEPROM.get(0, settings);
  if (settings.magic != CONFIG_MAGIC || checksum(settings) != settings.checksum) {
    memset(&settings, 0, sizeof(settings));
    strlcpy(settings.broker, DEFAULT_BROKER, sizeof(settings.broker));
    strlcpy(settings.port, "1883", sizeof(settings.port));
    strlcpy(settings.panel, DEFAULT_PANEL_ID, sizeof(settings.panel));
  }
  settings.broker[63] = settings.port[5] = settings.user[63] = settings.password[95] = settings.panel[24] = 0;
}

void saveSettings() {
  NetworkSettings next = {};
  next.magic = CONFIG_MAGIC;
  strlcpy(next.broker, brokerField.getValue(), sizeof(next.broker));
  strlcpy(next.port, portField.getValue(), sizeof(next.port));
  strlcpy(next.user, userField.getValue(), sizeof(next.user));
  strlcpy(next.password, passwordField.getValue()[0] ? passwordField.getValue() : settings.password, sizeof(next.password));
  strlcpy(next.panel, panelField.getValue(), sizeof(next.panel));
  char* tail;
  const long port = strtol(next.port, &tail, 10);
  if (!next.broker[0] || *tail || port < 1 || port > 65535 || !validPanelId(next.panel)) {
    Serial.println("Invalid MQTT settings; nothing saved.");
    return;
  }
  next.checksum = checksum(next);
  EEPROM.put(0, next);
  if (EEPROM.commit()) {
    Serial.println("Network settings saved. Restarting.");
    restartRequested = true;
    restartAt = millis();
  }
}

void portalStarted(WiFiManager*) {
  portalSince = millis();
  portalRequested = true;
  received = false;
  readingCount = 6;
  const char* names[] = {"Setup Wi-Fi", "Password", "Open browser", "MQTT broker", "Panel ID", "Then save"};
  const String portalAddress = WiFi.softAPIP().toString();
  const char* texts[] = {apName, apPassword, portalAddress.c_str(), settings.broker, settings.panel, "Wi-Fi + MQTT"};
  for (uint8_t i = 0; i < 6; ++i) {
    strlcpy(readings[i].label, names[i], sizeof(readings[i].label));
    strlcpy(readings[i].value, texts[i], sizeof(readings[i].value));
    readings[i].unit[0] = 0;
    readings[i].color = CYAN;
  }
  activePage = 0;
  slots = 6;
  drawGrid();
  renderReadings(false);
  Serial.printf("Setup SSID: %s, password: %s, portal: %s\n", apName, apPassword, portalAddress.c_str());
}

void connectMqtt() {
  lastMqttAttempt = millis();
  if (!settings.broker[0] || !validPanelId(settings.panel)) return;
  tcp.setTimeout(150);
  const bool connected = mqtt.connect(clientId,
      settings.user[0] ? settings.user : nullptr, settings.user[0] ? settings.password : nullptr,
      availabilityTopic, 1, true, "offline");
  if (!connected) {
    Serial.printf("MQTT connection failed (%d).\n", mqtt.state());
    return;
  }
  mqtt.subscribe(stateTopic, 0);
  mqtt.publish(availabilityTopic, "online", true);
  disconnectedSince = millis();
  if (wifiManager.getConfigPortalActive()) wifiManager.stopConfigPortal();
  portalRequested = false;
  if (!received) {
    readingCount = 0;
    drawGrid();
  }
  Serial.printf("MQTT connected; waiting for %s\n", stateTopic);
}

void updateStatus() {
  const bool wifi = WiFi.status() == WL_CONNECTED;
  const int32_t rssi = wifi ? WiFi.RSSI() : -100;
  const int bars = !wifi ? -1 : rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  if (bars != displayedBars) {
    displayedBars = bars;
    for (int i = 0; i < 4; ++i) {
      const int height = 4 + 3 * i;
      tft.fillRect(200 + i * 7, 20 - height, 4, height, i < bars ? GREEN : bars < 0 ? RED : LINE);
    }
  }
  const bool stale = isStale();
  if (received && stale != wasStale) {
    wasStale = stale;
    renderReadings(stale);
    renderWeather(stale);
    renderHeatingDiagram(stale);
  }
  char status[40];
  uint16_t color = MUTED;
  if (wifiManager.getConfigPortalActive()) {
    snprintf(status, sizeof(status), "SETUP: %s", WiFi.softAPIP().toString().c_str());
    color = AMBER;
  } else if (!wifi) {
    strlcpy(status, "Wi-Fi reconnecting", sizeof(status));
    color = AMBER;
  } else if (!mqtt.connected()) {
    strlcpy(status, mqtt.state() == 5 || mqtt.state() == 4 ? "MQTT login rejected" : "MQTT reconnecting", sizeof(status));
    color = AMBER;
  } else if (!received) strlcpy(status, "Waiting for HA panel", sizeof(status));
  else if (stale) {
    strlcpy(status, "HAOS offline / MQTT OK", sizeof(status));
    color = AMBER;
  } else {
    strlcpy(status, "HAOS + MQTT connected", sizeof(status));
    color = GREEN;
  }
  textAt(statusCache, 12, 308, 216, status, color, BG, 1);
}

void setup() {
  Serial.begin(115200);
  strlcpy(timezone, DEFAULT_TIMEZONE, sizeof(timezone));
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, LOW);
  SPI.begin();
  tft.begin();
  tft.setRotation(TFT_ROTATION);
  tft.setTextWrap(false);
  tft.cp437(true);
  tft.fillScreen(BG);
  digitalWrite(TFT_LED, HIGH);
  textAt(dateCache, 12, 8, 180, "SYNCING TIME", WHITE, BG, 1);
  textAt(clockCache, 12, 25, 216, "--:--:--", WHITE, BG, 5);
  textAt(titleCache, 12, 64, 216, title, WHITE, BG, 1);
  drawGrid();
  loadSettings();
  snprintf(clientId, sizeof(clientId), "ha-panel-%06x", ESP.getChipId());
  snprintf(apName, sizeof(apName), "HA-Panel-%06x", ESP.getChipId());
  snprintf(apPassword, sizeof(apPassword), "panel-%06x", ESP.getChipId());
  snprintf(stateTopic, sizeof(stateTopic), "ha_panel/%s/state", settings.panel);
  snprintf(availabilityTopic, sizeof(availabilityTopic), "ha_panel/%s/availability", settings.panel);
  brokerField.setValue(settings.broker, 63);
  portField.setValue(settings.port, 5);
  userField.setValue(settings.user, 63);
  panelField.setValue(settings.panel, 24);
  wifiManager.addParameter(&brokerField);
  wifiManager.addParameter(&portField);
  wifiManager.addParameter(&userField);
  wifiManager.addParameter(&passwordField);
  wifiManager.addParameter(&panelField);
  wifiManager.setDebugOutput(false);
  wifiManager.setConnectTimeout(12);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setSaveParamsCallback(saveSettings);
  wifiManager.setAPCallback(portalStarted);
  wifiManager.setConfigPortalTimeout(600);
  wifiManager.setBreakAfterConfig(true);
  wifiManager.setHostname(clientId);
  wifiManager.autoConnect(apName, apPassword);
  WiFi.setAutoReconnect(true);
  mqtt.setServer(settings.broker, atoi(settings.port));
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(1);
  mqtt.setCallback(receivePanel);
  disconnectedSince = millis();
  lastMqttAttempt = millis() - 10000;
}

void loop() {
  wifiManager.process();
  if (restartRequested && millis() - restartAt > 1000) ESP.restart();
  const uint32_t now = millis();
  const bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi && !ntpStarted) {
    configTime(timezone, "pool.ntp.org", "time.cloudflare.com");
    ntpStarted = true;
    Serial.printf("Panel IP: %s\n", WiFi.localIP().toString().c_str());
  }
  updateClock();
  if (wifi && !mqtt.connected() && now - lastMqttAttempt >= 10000) connectMqtt();
  if (mqtt.connected()) {
    mqtt.loop();
    disconnectedSince = now;
  }
  // Recover a changed Wi-Fi network or MQTT login through the setup portal.
  if (!mqtt.connected() && !wifiManager.getConfigPortalActive() &&
      now - disconnectedSince > 60000 && !portalRequested) {
    wifiManager.startConfigPortal(apName, apPassword);
  }
  if (portalRequested && !wifiManager.getConfigPortalActive() && now - portalSince > 600000) {
    portalRequested = false;
    disconnectedSince = now;
  }
  if (received && !wifiManager.getConfigPortalActive() &&
      millis() - lastPageChange >= uint32_t(pageSeconds) * 1000UL) {
    showPage(activePage == 0 ? (weatherEnabled ? 1 : 2) : activePage == 3 ? 0 : activePage + 1);
  }
  if (now - lastSignal >= 1000) {
    lastSignal = now;
    updateStatus();
  }
  yield();
}
