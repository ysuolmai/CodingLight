/*******************************************************************************
  ESP32-C3 AI Coding Status Light

  Board:
    ESP32-C3 Super Mini

  Wiring diagram:
    ESP32-C3 GPIO2 ---- resistor ----|<|---- +3V3  Green LED
    ESP32-C3 GPIO3 ---- resistor ----|<|---- +3V3  Yellow LED
    ESP32-C3 GPIO4 ---- resistor ----|<|---- +3V3  Red LED

    The LEDs are common anode / active-low:
      PWM duty 0   = LED off
      PWM duty 255 = LED fully on
    This sketch hides the inversion inside the LED functions.

  Supported Serial and BLE commands, one command per line:
    PING
    INFO
    STATE OFF
    STATE IDLE
    STATE THINKING
    STATE CODING
    STATE BUILD
    STATE SUCCESS
    STATE ERROR
    STATE WARNING
    STATE OTA
    BRIGHTNESS 0-255

    Responses:
      PONG
      JSON object for INFO
      OK for accepted state/brightness changes
      ERR for malformed or unknown commands

  REST API:
    GET  /                 Small browser UI using fetch()
    GET  /api/info         JSON status
    POST /api/state        Body: {"state":"CODING"}
    POST /api/brightness   Body: {"brightness":120}

  WiFi credentials:
    Put credentials in a separate file named wifi_secrets.h in the same sketch
    folder, or create an Arduino IDE tab with that exact name:

      #pragma once
      static const char WIFI_SSID[] = "your_ssid";
      static const char WIFI_PASSWORD[] = "your_password";

  Firmware update:
    OTA upload is intentionally disabled to keep the sketch below the default
    ESP32-C3 app partition size. Update firmware over USB from Arduino IDE.

  BLE Nordic UART Service UUIDs:
    Service:        6E400001-B5A3-F393-E0A9-E50E24DCCA9E
    RX characteristic: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
      Central writes commands here.
    TX characteristic: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
      Device sends command responses here using notify.

  Memory usage estimate:
    Sketch flash: typically 1.2-1.5 MB with WiFi, WebServer, BLE, mDNS, OTA.
    Runtime RAM: typically 90-140 KB free after WiFi and BLE startup, depending
    on board package, partition table, and BLE stack configuration.

  Future extension notes:
    - Add authentication before exposing this on untrusted WiFi networks.
    - Persist state and brightness in NVS if power-cycle restoration is needed.
    - Add a WebSocket/SSE endpoint if live browser updates become important.
    - Add rate limiting for HTTP/BLE commands in shared environments.
*******************************************************************************/

// ============================================================================
// Configuration
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
static const char WIFI_SSID[] = "";
static const char WIFI_PASSWORD[] = "";
#endif

static const char DEVICE_NAME[] = "CodingLight";
static const char MDNS_NAME[] = "codinglight";

static const uint8_t PIN_GREEN = 2;
static const uint8_t PIN_YELLOW = 3;
static const uint8_t PIN_RED = 4;

static const uint32_t LEDC_FREQ_HZ = 5000;
static const uint8_t LEDC_RES_BITS = 8;

static const uint32_t SERIAL_BAUD = 115200;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000UL;

static const size_t COMMAND_BUFFER_SIZE = 96;
static const size_t RESPONSE_BUFFER_SIZE = 384;
static const uint8_t BLE_RESPONSE_QUEUE_DEPTH = 4;

static const char NUS_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char NUS_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char NUS_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// ============================================================================
// Enums
// ============================================================================

enum LightState : uint8_t {
  STATE_OFF = 0,
  STATE_IDLE,
  STATE_THINKING,
  STATE_CODING,
  STATE_BUILD,
  STATE_SUCCESS,
  STATE_ERROR,
  STATE_WARNING,
  STATE_OTA
};

// ============================================================================
// Globals
// ============================================================================

static WebServer server(80);

static BLEServer *bleServer = nullptr;
static BLECharacteristic *bleTxCharacteristic = nullptr;
static bool bleClientConnected = false;
static bool bleStarted = false;
static char bleCommandBuffer[COMMAND_BUFFER_SIZE];
static size_t bleCommandLength = 0;
static uint32_t bleLastRxMs = 0;
static portMUX_TYPE bleCommandMux = portMUX_INITIALIZER_UNLOCKED;
static char bleResponseQueue[BLE_RESPONSE_QUEUE_DEPTH][RESPONSE_BUFFER_SIZE];
static volatile uint8_t bleResponseHead = 0;
static volatile uint8_t bleResponseTail = 0;
static volatile uint8_t bleResponseCount = 0;
static portMUX_TYPE bleResponseMux = portMUX_INITIALIZER_UNLOCKED;

static LightState currentState = STATE_IDLE;
static LightState previousState = STATE_IDLE;
static uint32_t stateStartedAtMs = 0;
static uint8_t globalBrightness = 180;

static char serialCommandBuffer[COMMAND_BUFFER_SIZE];
static size_t serialCommandLength = 0;

static uint32_t lastWifiReconnectAttemptMs = 0;
static bool mdnsStarted = false;

// ============================================================================
// Forward Declarations
// ============================================================================

static const char *stateToText(LightState state);
static bool parseStateName(const char *text, LightState *outState);
static bool setState(LightState nextState);
static void renderAnimation(uint32_t nowMs);
static void buildInfoJson(char *out, size_t outSize);
static bool processCommand(const char *command, char *response, size_t responseSize);
static void handleBleCommandBytes(const uint8_t *data, size_t length);
static void sendBleResponse(const char *response);
static void serviceBleRx();
static void serviceBleTx();

// ============================================================================
// Utility
// ============================================================================

static bool equalsIgnoreCase(const char *a, const char *b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }

  while (*a != '\0' && *b != '\0') {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
      return false;
    }
    ++a;
    ++b;
  }

  return *a == '\0' && *b == '\0';
}

static void trimInPlace(char *text) {
  if (text == nullptr) {
    return;
  }

  char *start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    ++start;
  }

  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }

  size_t len = strlen(text);
  while (len > 0 && isspace((unsigned char)text[len - 1])) {
    text[len - 1] = '\0';
    --len;
  }
}

static uint8_t clampToByte(long value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return (uint8_t)value;
}

static uint8_t scaleByGlobalBrightness(uint8_t value) {
  return (uint8_t)(((uint16_t)value * (uint16_t)globalBrightness + 127U) / 255U);
}

static uint8_t sineBreath(uint32_t elapsedMs, uint32_t periodMs) {
  const uint32_t position = elapsedMs % periodMs;
  const uint32_t halfPeriod = periodMs / 2U;
  const uint32_t ramp = position <= halfPeriod ? position : periodMs - position;
  const uint32_t linear = (ramp * 255U) / halfPeriod;

  // Integer smoothstep: y = x*x*(3 - 2*x), with x normalized to 0..255.
  return (uint8_t)((linear * linear * (765U - (2U * linear))) / 16581375UL);
}

static uint8_t visiblePulse(uint32_t elapsedMs, uint32_t periodMs, uint8_t minimumValue) {
  const uint8_t wave = sineBreath(elapsedMs, periodMs);
  return minimumValue + (uint8_t)(((uint16_t)wave * (uint16_t)(255U - minimumValue)) / 255U);
}

static void renderWorkCycle(uint32_t elapsedMs) {
  const uint32_t phaseMs = elapsedMs % 1440UL;
  const uint32_t localMs = (phaseMs % 480UL) / 2U;
  const uint8_t value = visiblePulse(localMs, 240U, 70U);

  if (phaseMs < 480UL) {
    setRGB(value, 0, 0);
  } else if (phaseMs < 960UL) {
    setRGB(0, value, 0);
  } else {
    setRGB(0, 0, value);
  }
}

// ============================================================================
// LED
// ============================================================================

static void setupLedPwm() {
  ledcAttach(PIN_GREEN, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttach(PIN_YELLOW, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttach(PIN_RED, LEDC_FREQ_HZ, LEDC_RES_BITS);
}

static void writeLedActiveLow(uint8_t pin, uint8_t value) {
  const uint8_t scaled = scaleByGlobalBrightness(value);
  ledcWrite(pin, 255U - scaled);
}

void setGreen(uint8_t value) {
  writeLedActiveLow(PIN_GREEN, value);
}

void setYellow(uint8_t value) {
  writeLedActiveLow(PIN_YELLOW, value);
}

void setRed(uint8_t value) {
  writeLedActiveLow(PIN_RED, value);
}

void setRGB(uint8_t green, uint8_t yellow, uint8_t red) {
  setGreen(green);
  setYellow(yellow);
  setRed(red);
}

// ============================================================================
// Animation
// ============================================================================

static const char *stateToText(LightState state) {
  switch (state) {
    case STATE_OFF: return "OFF";
    case STATE_IDLE: return "IDLE";
    case STATE_THINKING: return "THINKING";
    case STATE_CODING: return "CODING";
    case STATE_BUILD: return "BUILD";
    case STATE_SUCCESS: return "SUCCESS";
    case STATE_ERROR: return "ERROR";
    case STATE_WARNING: return "WARNING";
    case STATE_OTA: return "OTA";
    default: return "UNKNOWN";
  }
}

static bool parseStateName(const char *text, LightState *outState) {
  if (text == nullptr || outState == nullptr) {
    return false;
  }

  if (equalsIgnoreCase(text, "OFF")) {
    *outState = STATE_OFF;
  } else if (equalsIgnoreCase(text, "IDLE")) {
    *outState = STATE_IDLE;
  } else if (equalsIgnoreCase(text, "THINKING")) {
    *outState = STATE_THINKING;
  } else if (equalsIgnoreCase(text, "CODING")) {
    *outState = STATE_CODING;
  } else if (equalsIgnoreCase(text, "BUILD")) {
    *outState = STATE_BUILD;
  } else if (equalsIgnoreCase(text, "SUCCESS")) {
    *outState = STATE_SUCCESS;
  } else if (equalsIgnoreCase(text, "ERROR")) {
    *outState = STATE_ERROR;
  } else if (equalsIgnoreCase(text, "WARNING")) {
    *outState = STATE_WARNING;
  } else if (equalsIgnoreCase(text, "OTA")) {
    *outState = STATE_OTA;
  } else {
    return false;
  }

  return true;
}

static bool setState(LightState nextState) {
  const uint32_t nowMs = millis();

  if (nextState == STATE_SUCCESS) {
    if (currentState != STATE_SUCCESS) {
      previousState = currentState;
    }
  } else {
    previousState = nextState;
  }

  currentState = nextState;
  stateStartedAtMs = nowMs;
  renderAnimation(nowMs);
  return true;
}

static void renderAnimation(uint32_t nowMs) {
  const uint32_t elapsedMs = nowMs - stateStartedAtMs;

  switch (currentState) {
    case STATE_OFF:
      setRGB(0, 0, 0);
      break;

    case STATE_IDLE:
      setRGB(255, 0, 0);
      break;

    case STATE_THINKING:
      renderWorkCycle(elapsedMs);
      break;

    case STATE_CODING:
      renderWorkCycle(elapsedMs);
      break;

    case STATE_BUILD:
      renderWorkCycle(elapsedMs);
      break;

    case STATE_SUCCESS: {
      if (elapsedMs >= 1200UL) {
        setState(previousState);
        return;
      }

      const bool on = ((elapsedMs / 200UL) % 2UL) == 0UL;
      setRGB(on ? 255 : 0, 0, 0);
      break;
    }

    case STATE_ERROR: {
      const bool on = ((elapsedMs / 300UL) % 2UL) == 0UL;
      setRGB(0, 0, on ? 255 : 0);
      break;
    }

    case STATE_WARNING: {
      const bool on = ((elapsedMs / 300UL) % 2UL) == 0UL;
      setRGB(0, on ? 255 : 0, 0);
      break;
    }

    case STATE_OTA: {
      const uint8_t phase = (uint8_t)((elapsedMs / 250UL) % 3UL);
      setRGB(phase == 0 ? 255 : 0, phase == 1 ? 255 : 0, phase == 2 ? 255 : 0);
      break;
    }
  }
}

// ============================================================================
// Serial and Command Processing
// ============================================================================

static void buildInfoJson(char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }

  IPAddress ip = WiFi.localIP();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;

  snprintf(out, outSize,
           "{\"state\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"wifi\":%s,\"ble\":%s,"
           "\"brightness\":%u,\"uptime\":%lu}",
           stateToText(currentState),
           ip[0], ip[1], ip[2], ip[3],
           wifiConnected ? "true" : "false",
           bleStarted ? "true" : "false",
           globalBrightness,
           (unsigned long)millis());
}

static bool parseUnsignedByte(const char *text, uint8_t *outValue) {
  if (text == nullptr || outValue == nullptr || *text == '\0') {
    return false;
  }

  char *endPtr = nullptr;
  const long value = strtol(text, &endPtr, 10);
  if (endPtr == text) {
    return false;
  }

  while (*endPtr != '\0') {
    if (!isspace((unsigned char)*endPtr)) {
      return false;
    }
    ++endPtr;
  }

  if (value < 0 || value > 255) {
    return false;
  }

  *outValue = (uint8_t)value;
  return true;
}

static bool processCommand(const char *command, char *response, size_t responseSize) {
  if (response == nullptr || responseSize == 0) {
    return false;
  }

  response[0] = '\0';

  if (command == nullptr) {
    snprintf(response, responseSize, "ERR");
    return false;
  }

  char buffer[COMMAND_BUFFER_SIZE];
  strncpy(buffer, command, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';
  trimInPlace(buffer);

  if (buffer[0] == '\0') {
    snprintf(response, responseSize, "ERR");
    return false;
  }

  if (equalsIgnoreCase(buffer, "PING")) {
    snprintf(response, responseSize, "PONG");
    return true;
  }

  if (equalsIgnoreCase(buffer, "INFO")) {
    buildInfoJson(response, responseSize);
    return true;
  }

  char *space = buffer;
  while (*space != '\0' && !isspace((unsigned char)*space)) {
    ++space;
  }

  if (*space != '\0') {
    *space = '\0';
    ++space;
    while (*space != '\0' && isspace((unsigned char)*space)) {
      ++space;
    }
  }

  if (equalsIgnoreCase(buffer, "STATE")) {
    LightState nextState = STATE_OFF;
    if (parseStateName(space, &nextState) && setState(nextState)) {
      snprintf(response, responseSize, "OK");
      return true;
    }
    snprintf(response, responseSize, "ERR");
    return false;
  }

  if (equalsIgnoreCase(buffer, "BRIGHTNESS")) {
    uint8_t value = 0;
    if (parseUnsignedByte(space, &value)) {
      globalBrightness = value;
      renderAnimation(millis());
      snprintf(response, responseSize, "OK");
      return true;
    }
    snprintf(response, responseSize, "ERR");
    return false;
  }

  snprintf(response, responseSize, "ERR");
  return false;
}

static void handleSerialByte(char c) {
  if (c == '\r') {
    return;
  }

  if (c == '\n') {
    if (serialCommandLength > 0) {
      serialCommandBuffer[serialCommandLength] = '\0';

      char response[RESPONSE_BUFFER_SIZE];
      processCommand(serialCommandBuffer, response, sizeof(response));
      Serial.println(response);

      serialCommandLength = 0;
    }
    return;
  }

  if (serialCommandLength < sizeof(serialCommandBuffer) - 1) {
    serialCommandBuffer[serialCommandLength++] = c;
  } else {
    serialCommandLength = 0;
    Serial.println("ERR");
  }
}

static void serviceSerial() {
  while (Serial.available() > 0) {
    handleSerialByte((char)Serial.read());
  }
}

// ============================================================================
// BLE
// ============================================================================

class CodingLightServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    (void)server;
    bleClientConnected = true;
  }

  void onDisconnect(BLEServer *server) override {
    (void)server;
    bleClientConnected = false;
    BLEDevice::startAdvertising();
  }
};

class CodingLightRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    handleWrite(characteristic);
  }

  void handleWrite(BLECharacteristic *characteristic) {
    String value = characteristic->getValue();
    if (value.length() > 0) {
      handleBleCommandBytes((const uint8_t *)value.c_str(), value.length());
    }
  }
};

static CodingLightServerCallbacks bleServerCallbacks;
static CodingLightRxCallbacks bleRxCallbacks;
static BLE2902 bleTxClientConfigDescriptor;

static void sendBleResponse(const char *response) {
  if (response == nullptr) {
    return;
  }

  char formatted[RESPONSE_BUFFER_SIZE];
  snprintf(formatted, sizeof(formatted), "%s\n", response);

  portENTER_CRITICAL(&bleResponseMux);

  if (bleResponseCount >= BLE_RESPONSE_QUEUE_DEPTH) {
    bleResponseTail = (uint8_t)((bleResponseTail + 1U) % BLE_RESPONSE_QUEUE_DEPTH);
    --bleResponseCount;
  }

  strncpy(bleResponseQueue[bleResponseHead], formatted, RESPONSE_BUFFER_SIZE - 1);
  bleResponseQueue[bleResponseHead][RESPONSE_BUFFER_SIZE - 1] = '\0';
  bleResponseHead = (uint8_t)((bleResponseHead + 1U) % BLE_RESPONSE_QUEUE_DEPTH);
  ++bleResponseCount;

  portEXIT_CRITICAL(&bleResponseMux);
}

static void processBleBufferedCommand() {
  char command[COMMAND_BUFFER_SIZE];

  portENTER_CRITICAL(&bleCommandMux);
  if (bleCommandLength == 0) {
    portEXIT_CRITICAL(&bleCommandMux);
    return;
  }

  bleCommandBuffer[bleCommandLength] = '\0';
  strncpy(command, bleCommandBuffer, sizeof(command) - 1);
  command[sizeof(command) - 1] = '\0';
  bleCommandLength = 0;
  portEXIT_CRITICAL(&bleCommandMux);

  char response[RESPONSE_BUFFER_SIZE];
  processCommand(command, response, sizeof(response));
  sendBleResponse(response);
}

static void serviceBleRx() {
  const uint32_t nowMs = millis();
  bool shouldFlush = false;

  portENTER_CRITICAL(&bleCommandMux);
  shouldFlush = bleCommandLength > 0 && nowMs - bleLastRxMs >= 80UL;
  portEXIT_CRITICAL(&bleCommandMux);

  if (shouldFlush) {
    processBleBufferedCommand();
  }
}

static void serviceBleTx() {
  if (!bleStarted || !bleClientConnected || bleTxCharacteristic == nullptr) {
    return;
  }

  char localResponse[RESPONSE_BUFFER_SIZE];

  portENTER_CRITICAL(&bleResponseMux);
  if (bleResponseCount == 0) {
    portEXIT_CRITICAL(&bleResponseMux);
    return;
  }

  strncpy(localResponse, bleResponseQueue[bleResponseTail], sizeof(localResponse) - 1);
  localResponse[sizeof(localResponse) - 1] = '\0';
  bleResponseTail = (uint8_t)((bleResponseTail + 1U) % BLE_RESPONSE_QUEUE_DEPTH);
  --bleResponseCount;

  portEXIT_CRITICAL(&bleResponseMux);

  const size_t totalLength = strlen(localResponse);
  size_t offset = 0;

  while (offset < totalLength) {
    const size_t remaining = totalLength - offset;
    const size_t chunkLength = remaining > 20U ? 20U : remaining;
    bleTxCharacteristic->setValue((uint8_t *)(localResponse + offset), chunkLength);
    bleTxCharacteristic->notify();
    offset += chunkLength;
  }
}

static void handleBleCommandBytes(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0) {
    return;
  }

  for (size_t i = 0; i < length; ++i) {
    const char c = (char)data[i];

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      processBleBufferedCommand();
      continue;
    }

    portENTER_CRITICAL(&bleCommandMux);
    if (bleCommandLength < sizeof(bleCommandBuffer) - 1) {
      bleCommandBuffer[bleCommandLength++] = c;
      bleLastRxMs = millis();
      portEXIT_CRITICAL(&bleCommandMux);
    } else {
      bleCommandLength = 0;
      portEXIT_CRITICAL(&bleCommandMux);
      sendBleResponse("ERR");
    }
  }
}

static void setupBle() {
  BLEDevice::init(DEVICE_NAME);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(&bleServerCallbacks);

  BLEService *service = bleServer->createService(NUS_SERVICE_UUID);

  bleTxCharacteristic = service->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(&bleTxClientConfigDescriptor);

  BLECharacteristic *rxCharacteristic = service->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxCharacteristic->setCallbacks(&bleRxCallbacks);

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  bleStarted = true;
}

// ============================================================================
// WiFi and mDNS
// ============================================================================

static void startMdnsIfNeeded() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
}

static void beginWifiAttempt(uint32_t nowMs) {
  if (WIFI_SSID[0] == '\0' || strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiReconnectAttemptMs = nowMs;
}

static void setupWifi() {
  beginWifiAttempt(millis());
}

static void serviceWifi() {
  const uint32_t nowMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    startMdnsIfNeeded();
    return;
  }

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  if (lastWifiReconnectAttemptMs == 0 ||
      nowMs - lastWifiReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    beginWifiAttempt(nowMs);
  }
}

// ============================================================================
// HTTP
// ============================================================================

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CodingLight</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:20px;background:#f4f6f8;color:#111}
main{max-width:680px;margin:auto;background:#fff;border:1px solid #ccd3db;border-radius:8px;padding:20px}
h1{font-size:24px;margin:0 0 14px}.state{font-size:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:8px;margin:18px 0}
button{border:1px solid #aeb8c2;background:#fff;border-radius:6px;padding:11px 8px;font-weight:650}
button.active{background:#134e4a;color:#fff}.row{display:flex;justify-content:space-between}
input{width:100%}.small{color:#57606a;font-size:13px;margin-top:14px}
</style>
</head>
<body>
<main>
<h1>CodingLight</h1>
<p class="state">Current State: <strong id="state">...</strong></p>
<div class="grid" id="buttons"></div>
<div class="row">
<label for="brightness">Brightness</label>
<span id="brightnessValue">...</span>
</div>
<input id="brightness" type="range" min="0" max="255" step="1">
<p class="small" id="meta"></p>
</main>
<script>
const states=["OFF","IDLE","THINKING","CODING","BUILD","SUCCESS","ERROR","WARNING","OTA"];
const buttons=document.getElementById("buttons");
const stateEl=document.getElementById("state");
const slider=document.getElementById("brightness");
const brightnessValue=document.getElementById("brightnessValue");
const meta=document.getElementById("meta");
for(const s of states){
  const b=document.createElement("button");
  b.textContent=s;
  b.dataset.state=s;
  b.onclick=async()=>{await fetch("/api/state",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({state:s})});await refresh();};
  buttons.appendChild(b);
}
let brightnessTimer=0;
slider.oninput=()=>{
  brightnessValue.textContent=slider.value;
  clearTimeout(brightnessTimer);
  brightnessTimer=setTimeout(async()=>{
    await fetch("/api/brightness",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({brightness:Number(slider.value)})});
    await refresh();
  },120);
};
async function refresh(){
  const r=await fetch("/api/info",{cache:"no-store"});
  const info=await r.json();
  stateEl.textContent=info.state;
  slider.value=info.brightness;
  brightnessValue.textContent=info.brightness;
  meta.textContent=`IP ${info.ip} | WiFi ${info.wifi?"on":"off"} | BLE ${info.ble?"on":"off"} | ${info.uptime} ms`;
  for(const b of buttons.children)b.classList.toggle("active",b.dataset.state===info.state);
}
refresh();
setInterval(refresh,2000);
</script>
</body>
</html>
)HTML";

static bool extractJsonStringValue(const char *body, const char *key, char *out, size_t outSize) {
  if (body == nullptr || key == nullptr || out == nullptr || outSize == 0) {
    return false;
  }

  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);

  const char *keyPos = strstr(body, pattern);
  if (keyPos == nullptr) {
    return false;
  }

  const char *colon = strchr(keyPos + strlen(pattern), ':');
  if (colon == nullptr) {
    return false;
  }

  const char *value = colon + 1;
  while (*value != '\0' && isspace((unsigned char)*value)) {
    ++value;
  }

  if (*value != '"') {
    return false;
  }
  ++value;

  size_t len = 0;
  while (value[len] != '\0' && value[len] != '"' && len < outSize - 1) {
    out[len] = value[len];
    ++len;
  }

  if (value[len] != '"') {
    return false;
  }

  out[len] = '\0';
  return len > 0;
}

static bool extractJsonIntValue(const char *body, const char *key, long *out) {
  if (body == nullptr || key == nullptr || out == nullptr) {
    return false;
  }

  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);

  const char *keyPos = strstr(body, pattern);
  if (keyPos == nullptr) {
    return false;
  }

  const char *colon = strchr(keyPos + strlen(pattern), ':');
  if (colon == nullptr) {
    return false;
  }

  const char *value = colon + 1;
  while (*value != '\0' && isspace((unsigned char)*value)) {
    ++value;
  }

  char *endPtr = nullptr;
  const long parsed = strtol(value, &endPtr, 10);
  if (endPtr == value) {
    return false;
  }

  *out = parsed;
  return true;
}

static void sendPlain(uint16_t code, const char *text) {
  server.send(code, "text/plain", text);
}

static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleApiInfo() {
  char json[RESPONSE_BUFFER_SIZE];
  buildInfoJson(json, sizeof(json));
  server.send(200, "application/json", json);
}

static void handleApiState() {
  if (!server.hasArg("plain")) {
    sendPlain(400, "ERR");
    return;
  }

  char body[160];
  String bodyString = server.arg("plain");
  bodyString.toCharArray(body, sizeof(body));

  char stateText[24];
  LightState nextState = STATE_OFF;
  if (!extractJsonStringValue(body, "state", stateText, sizeof(stateText)) ||
      !parseStateName(stateText, &nextState) ||
      !setState(nextState)) {
    sendPlain(400, "ERR");
    return;
  }

  sendPlain(200, "OK");
}

static void handleApiBrightness() {
  if (!server.hasArg("plain")) {
    sendPlain(400, "ERR");
    return;
  }

  char body[160];
  String bodyString = server.arg("plain");
  bodyString.toCharArray(body, sizeof(body));

  long value = 0;
  if (!extractJsonIntValue(body, "brightness", &value) || value < 0 || value > 255) {
    sendPlain(400, "ERR");
    return;
  }

  globalBrightness = clampToByte(value);
  renderAnimation(millis());
  sendPlain(200, "OK");
}

static void handleNotFound() {
  sendPlain(404, "ERR");
}

static void setupHttp() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/info", HTTP_GET, handleApiInfo);
  server.on("/api/state", HTTP_POST, handleApiState);
  server.on("/api/brightness", HTTP_POST, handleApiBrightness);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ============================================================================
// Loop
// ============================================================================

void setup() {
  Serial.begin(SERIAL_BAUD);

  setupLedPwm();
  setRGB(0, 0, 0);

  stateStartedAtMs = millis();
  setState(STATE_IDLE);

  setupBle();
  setupWifi();
  setupHttp();
}

void loop() {
  const uint32_t nowMs = millis();

  renderAnimation(nowMs);
  serviceSerial();
  serviceBleRx();
  serviceBleTx();
  serviceWifi();
  server.handleClient();
}
