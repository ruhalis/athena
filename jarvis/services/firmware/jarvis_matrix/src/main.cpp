#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "secrets.h"

#ifndef WIFI_SSID
#error "Copy include/secrets.h.example to include/secrets.h"
#endif

static const uint16_t PANEL_W = 64;
static const uint16_t PANEL_H = 64;

MatrixPanel_I2S_DMA *display = nullptr;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

enum Mode { MODE_CLOCK, MODE_THINK };
volatile Mode mode = MODE_CLOCK;
char clock_text[8] = "--:--";
uint8_t think_frame = 0;

void drawClock() {
  display->fillScreen(0);
  display->setTextColor(display->color565(180, 180, 180));
  display->setTextSize(2);
  display->setCursor(8, 24);
  display->print(clock_text);
}

void drawThink() {
  display->fillScreen(0);
  uint16_t c = display->color565(0, 120, 255);
  int x = 8 + (think_frame % 48);
  display->fillRect(x, 28, 8, 8, c);
  think_frame++;
}

void applyJson(const char *raw) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) return;
  const char *m = doc["mode"] | "";
  if (strcmp(m, "think") == 0) {
    mode = MODE_THINK;
  } else if (strcmp(m, "clock") == 0) {
    mode = MODE_CLOCK;
    const char *t = doc["t"] | clock_text;
    strncpy(clock_text, t, sizeof(clock_text) - 1);
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (!info->final || info->index != 0 || info->opcode != WS_TEXT) return;
  char buf[256];
  size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, data, n);
  buf[n] = 0;
  applyJson(buf);
}

void setup() {
  Serial.begin(115200);
  HUB75_I2S_CFG cfg(PANEL_W, PANEL_H, 1);
  display = new MatrixPanel_I2S_DMA(cfg);
  display->begin();
  display->setBrightness8(40);
  display->fillScreen(0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

void loop() {
  if (mode == MODE_THINK) drawThink();
  else drawClock();
  delay(80);
}
