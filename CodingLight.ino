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
    GET  /control           Browser UI, even while setup portal is active
    GET  /config            WiFi captive portal setup page
    POST /config            Save WiFi credentials from captive portal form
    GET  /api/info         JSON status
    POST /api/state        Body: {"state":"CODING"}
    POST /api/brightness   Body: {"brightness":120}

  WiFi credentials:
    Runtime WiFi credentials saved from the captive portal are stored in NVS
    and take priority.

    Optional build-time credentials can be put in a separate file named
    wifi_secrets.h in the same sketch folder, or in an Arduino IDE tab with
    that exact name:

      #pragma once
      static const char WIFI_SSID[] = "your_ssid";
      static const char WIFI_PASSWORD[] = "your_password";

    If no runtime or build-time SSID is configured, the device starts an open
    setup AP named CodingLight-Setup. Connect to it and open:

      http://192.168.4.1/

    Long-press BOOT for 2.5 seconds to re-enable the setup AP. Opening the
    setup portal does not erase existing credentials; credentials are replaced
    only after a new SSID is submitted.

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
    Sketch flash: typically 1.2-1.5 MB with WiFi, WebServer, BLE, mDNS, and
    captive portal support.
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
#include <DNSServer.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
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
static const uint8_t PIN_BOOT = 9;

static const uint32_t LEDC_FREQ_HZ = 5000;
static const uint8_t LEDC_RES_BITS = 8;

static const uint32_t SERIAL_BAUD = 115200;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000UL;
static const uint32_t BOOT_LONG_PRESS_MS = 2500UL;
static const uint32_t CONFIG_PORTAL_CLOSE_DELAY_MS = 3000UL;

static const size_t COMMAND_BUFFER_SIZE = 96;
static const size_t RESPONSE_BUFFER_SIZE = 384;
static const size_t WIFI_SSID_BUFFER_SIZE = 33;
static const size_t WIFI_PASSWORD_BUFFER_SIZE = 65;
static const uint8_t BLE_RESPONSE_QUEUE_DEPTH = 4;

static const char CONFIG_AP_SSID[] = "CodingLight-Setup";
static const uint16_t DNS_PORT = 53;

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
static DNSServer dnsServer;
static Preferences wifiPreferences;

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

static char activeWifiSsid[WIFI_SSID_BUFFER_SIZE];
static char activeWifiPassword[WIFI_PASSWORD_BUFFER_SIZE];
static bool wifiCredentialsAvailable = false;
static uint32_t lastWifiReconnectAttemptMs = 0;
static bool mdnsStarted = false;
static bool configPortalActive = false;
static bool dnsServerStarted = false;
static uint32_t configPortalCloseAtMs = 0;

static bool bootPressed = false;
static bool bootLongPressHandled = false;
static uint32_t bootPressedAtMs = 0;

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

static void safeCopy(char *out, size_t outSize, const char *in) {
  if (out == nullptr || outSize == 0) {
    return;
  }

  if (in == nullptr) {
    out[0] = '\0';
    return;
  }

  strncpy(out, in, outSize - 1);
  out[outSize - 1] = '\0';
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
      if (elapsedMs >= 20000UL) {
        setState(STATE_IDLE);
        return;
      }

      const bool on = ((elapsedMs / 300UL) % 2UL) == 0UL;
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
  IPAddress apIp = WiFi.softAPIP();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;

  snprintf(out, outSize,
           "{\"state\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"wifi\":%s,\"ap\":%s,"
           "\"ap_ip\":\"%u.%u.%u.%u\",\"ble\":%s,\"brightness\":%u,"
           "\"uptime\":%lu}",
           stateToText(currentState),
           ip[0], ip[1], ip[2], ip[3],
           wifiConnected ? "true" : "false",
           configPortalActive ? "true" : "false",
           apIp[0], apIp[1], apIp[2], apIp[3],
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

static bool isBuildTimeSsidConfigured() {
  return WIFI_SSID[0] != '\0' && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

static bool loadWifiCredentials() {
  activeWifiSsid[0] = '\0';
  activeWifiPassword[0] = '\0';

  bool loaded = false;
  if (wifiPreferences.begin("codinglight", true)) {
    const size_t ssidLen = wifiPreferences.getString(
      "ssid", activeWifiSsid, sizeof(activeWifiSsid));
    wifiPreferences.getString("pass", activeWifiPassword, sizeof(activeWifiPassword));
    wifiPreferences.end();
    loaded = ssidLen > 0 && activeWifiSsid[0] != '\0';
  }

  if (!loaded && isBuildTimeSsidConfigured()) {
    safeCopy(activeWifiSsid, sizeof(activeWifiSsid), WIFI_SSID);
    safeCopy(activeWifiPassword, sizeof(activeWifiPassword), WIFI_PASSWORD);
    loaded = true;
  }

  wifiCredentialsAvailable = loaded;
  return loaded;
}

static bool saveRuntimeWifiCredentials(const char *ssid, const char *password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }

  if (!wifiPreferences.begin("codinglight", false)) {
    return false;
  }

  const size_t savedSsid = wifiPreferences.putString("ssid", ssid);
  wifiPreferences.putString("pass", password != nullptr ? password : "");
  wifiPreferences.end();

  if (savedSsid == 0) {
    return false;
  }

  safeCopy(activeWifiSsid, sizeof(activeWifiSsid), ssid);
  safeCopy(activeWifiPassword, sizeof(activeWifiPassword), password != nullptr ? password : "");
  wifiCredentialsAvailable = true;
  return true;
}

static void startMdnsIfNeeded() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
}

static void startConfigPortal() {
  if (configPortalActive) {
    return;
  }

  WiFi.mode(WIFI_AP_STA);

  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, gateway, subnet);

  if (WiFi.softAP(CONFIG_AP_SSID)) {
    configPortalActive = true;
    configPortalCloseAtMs = 0;
    dnsServerStarted = dnsServer.start(DNS_PORT, "*", apIp);
    Serial.println("CONFIG_AP_STARTED");
    Serial.println(apIp);
  } else {
    Serial.println("CONFIG_AP_FAILED");
  }
}

static void stopConfigPortal() {
  if (!configPortalActive) {
    return;
  }

  if (dnsServerStarted) {
    dnsServer.stop();
    dnsServerStarted = false;
  }

  WiFi.softAPdisconnect(true);
  configPortalActive = false;
  configPortalCloseAtMs = 0;

  if (WiFi.status() == WL_CONNECTED || wifiCredentialsAvailable) {
    WiFi.mode(WIFI_STA);
  }

  Serial.println("CONFIG_AP_STOPPED");
}

static void beginWifiAttempt(uint32_t nowMs) {
  if (!wifiCredentialsAvailable) {
    return;
  }

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  WiFi.mode(configPortalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.begin(activeWifiSsid, activeWifiPassword);
  lastWifiReconnectAttemptMs = nowMs;
}

static void setupWifi() {
  WiFi.persistent(false);
  WiFi.setHostname(DEVICE_NAME);

  if (loadWifiCredentials()) {
    beginWifiAttempt(millis());
  } else {
    startConfigPortal();
  }
}

static void serviceWifi() {
  const uint32_t nowMs = millis();

  if (dnsServerStarted) {
    dnsServer.processNextRequest();
  }

  if (WiFi.status() == WL_CONNECTED) {
    startMdnsIfNeeded();

    if (configPortalCloseAtMs != 0 && nowMs >= configPortalCloseAtMs) {
      stopConfigPortal();
    }
    return;
  }

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  if (!wifiCredentialsAvailable) {
    startConfigPortal();
    return;
  }

  if (lastWifiReconnectAttemptMs == 0 ||
      nowMs - lastWifiReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    beginWifiAttempt(nowMs);
  }
}

static void serviceBootButton() {
  const uint32_t nowMs = millis();
  const bool pressedNow = digitalRead(PIN_BOOT) == LOW;

  if (pressedNow && !bootPressed) {
    bootPressed = true;
    bootLongPressHandled = false;
    bootPressedAtMs = nowMs;
    return;
  }

  if (!pressedNow) {
    bootPressed = false;
    bootLongPressHandled = false;
    return;
  }

  if (!bootLongPressHandled && nowMs - bootPressedAtMs >= BOOT_LONG_PRESS_MS) {
    bootLongPressHandled = true;
    startConfigPortal();
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

static void sendConfigPage(const char *message, bool isError) {
  IPAddress apIp = WiFi.softAPIP();
  const bool staConnected = WiFi.status() == WL_CONNECTED;

  char page[3200];
  snprintf(page, sizeof(page),
           "<!doctype html><html><head>"
           "<meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>CodingLight WiFi Setup</title>"
           "<style>"
           "body{font-family:system-ui,sans-serif;margin:0;padding:20px;background:#f4f6f8;color:#111}"
           "main{max-width:560px;margin:auto;background:#fff;border:1px solid #ccd3db;border-radius:8px;padding:20px}"
           "h1{font-size:24px;margin:0 0 12px}"
           "label{display:block;font-weight:650;margin:14px 0 6px}"
           "input{box-sizing:border-box;width:100%%;font:inherit;padding:10px;border:1px solid #aeb8c2;border-radius:6px}"
           "button{margin-top:16px;border:1px solid #0f766e;background:#0f766e;color:#fff;border-radius:6px;padding:11px 14px;font-weight:700}"
           ".msg{padding:10px;border-radius:6px;background:%s;color:%s}"
           ".meta{color:#57606a;font-size:13px;line-height:1.5;margin-top:16px}"
           "a{color:#0f766e}"
           "</style></head><body><main>"
           "<h1>CodingLight WiFi Setup</h1>"
           "%s%s%s"
           "<form method=\"post\" action=\"/config\">"
           "<label for=\"ssid\">WiFi SSID</label>"
           "<input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required autocomplete=\"off\">"
           "<label for=\"password\">WiFi Password</label>"
           "<input id=\"password\" name=\"password\" maxlength=\"64\" type=\"password\" autocomplete=\"current-password\">"
           "<button type=\"submit\">Save and Connect</button>"
           "</form>"
           "<p class=\"meta\">Setup AP: %s<br>AP IP: %u.%u.%u.%u<br>Station: %s<br>"
           "Saved credentials are kept until you submit a new SSID. Opening this page does not erase existing WiFi settings.</p>"
           "<p class=\"meta\"><a href=\"/control\">Open light controls</a></p>"
           "</main></body></html>",
           isError ? "#fee2e2" : "#dcfce7",
           isError ? "#991b1b" : "#166534",
           message != nullptr && message[0] != '\0' ? "<p class=\"msg\">" : "",
           message != nullptr ? message : "",
           message != nullptr && message[0] != '\0' ? "</p>" : "",
           CONFIG_AP_SSID,
           apIp[0], apIp[1], apIp[2], apIp[3],
           staConnected ? "connected" : "not connected");

  server.send(200, "text/html", page);
}

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
  if (configPortalActive) {
    sendConfigPage("", false);
    return;
  }

  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleControl() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleConfigGet() {
  sendConfigPage("", false);
}

static void handleConfigPost() {
  if (!server.hasArg("ssid")) {
    sendConfigPage("Missing SSID.", true);
    return;
  }

  String ssidString = server.arg("ssid");
  String passwordString = server.hasArg("password") ? server.arg("password") : "";

  if (ssidString.length() == 0 || ssidString.length() >= WIFI_SSID_BUFFER_SIZE) {
    sendConfigPage("SSID must be 1-32 bytes.", true);
    return;
  }

  if (passwordString.length() >= WIFI_PASSWORD_BUFFER_SIZE) {
    sendConfigPage("Password must be 64 bytes or less.", true);
    return;
  }

  char ssid[WIFI_SSID_BUFFER_SIZE];
  char password[WIFI_PASSWORD_BUFFER_SIZE];
  ssidString.toCharArray(ssid, sizeof(ssid));
  passwordString.toCharArray(password, sizeof(password));

  if (!saveRuntimeWifiCredentials(ssid, password)) {
    sendConfigPage("Failed to save credentials.", true);
    return;
  }

  beginWifiAttempt(millis());
  configPortalCloseAtMs = millis() + CONFIG_PORTAL_CLOSE_DELAY_MS;
  sendConfigPage("Saved. CodingLight is connecting to WiFi now.", false);
}

static void redirectToConfigPortal() {
  IPAddress apIp = WiFi.softAPIP();
  char location[48];
  snprintf(location, sizeof(location), "http://%u.%u.%u.%u/config",
           apIp[0], apIp[1], apIp[2], apIp[3]);
  server.sendHeader("Location", location, true);
  server.send(302, "text/plain", "");
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
  if (configPortalActive) {
    redirectToConfigPortal();
    return;
  }

  sendPlain(404, "ERR");
}

static void setupHttp() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_GET, handleControl);
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/generate_204", HTTP_GET, redirectToConfigPortal);
  server.on("/hotspot-detect.html", HTTP_GET, redirectToConfigPortal);
  server.on("/connecttest.txt", HTTP_GET, redirectToConfigPortal);
  server.on("/fwlink", HTTP_GET, redirectToConfigPortal);
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

  pinMode(PIN_BOOT, INPUT_PULLUP);

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
  serviceBootButton();
  serviceWifi();
  server.handleClient();
}
