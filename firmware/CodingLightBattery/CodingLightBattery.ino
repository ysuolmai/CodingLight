/*******************************************************************************
  CodingLight Battery - ESP32-C3 Super Mini

  Hardware:
    GPIO2 -> red LED cathode (active LOW, common anode)
    GPIO3 -> yellow LED cathode (active LOW, common anode)
    GPIO4 -> green LED cathode (active LOW, common anode)
    GPIO5 -> push button to GND (active LOW, INPUT_PULLUP)

  Controls:
    Short button press: cycle through local light states.
    Hold for 2-5 seconds, then release: enter deep sleep.
    Hold for 5 seconds, then release: open the WiFi setup portal.
    Press the button while sleeping: wake and restart in IDLE.

  The Serial, BLE NUS, and REST command contracts match the wired firmware.
  ArduinoOTA starts after WiFi connects. Use an OTA-capable partition scheme.
*******************************************************************************/

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "driver/gpio.h"
#include "esp_sleep.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
static const char WIFI_SSID[] = "";
static const char WIFI_PASSWORD[] = "";
#endif

#if __has_include("ota_secrets.h")
#include "ota_secrets.h"
#else
static const char OTA_PASSWORD[] = "";
#endif

static const char DEVICE_NAME[] = "CodingLight-Battery";
static const char MDNS_NAME[] = "codinglight-battery";

static const uint8_t PIN_RED = 2;
static const uint8_t PIN_YELLOW = 3;
static const uint8_t PIN_GREEN = 4;
static const uint8_t PIN_BUTTON = 5;

static const uint32_t SERIAL_BAUD = 115200;
static const uint32_t LEDC_FREQ_HZ = 5000;
static const uint8_t LEDC_RES_BITS = 8;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000UL;
static const uint32_t CONFIG_PORTAL_CLOSE_DELAY_MS = 3000UL;
static const int8_t WIFI_TX_POWER_QDBM = 68;
static const uint32_t BUTTON_DEBOUNCE_MS = 35UL;
static const uint32_t BUTTON_SLEEP_PRESS_MS = 2000UL;
static const uint32_t BUTTON_WIFI_PRESS_MS = 5000UL;
static const uint32_t IDLE_LIGHT_OFF_MS = 300000UL;
static const uint32_t IDLE_SLEEP_MS = 900000UL;

static const size_t COMMAND_BUFFER_SIZE = 96;
static const size_t RESPONSE_BUFFER_SIZE = 448;
static const size_t WIFI_SSID_BUFFER_SIZE = 33;
static const size_t WIFI_PASSWORD_BUFFER_SIZE = 65;
static const uint8_t BLE_RESPONSE_QUEUE_DEPTH = 4;

static const char CONFIG_AP_SSID[] = "CodingLight-Setup";
static const uint16_t DNS_PORT = 53;

static const char NUS_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char NUS_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char NUS_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

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
static uint32_t stateStartedAtMs = 0;
static uint8_t globalBrightness = 1;
static esp_sleep_wakeup_cause_t wakeupCause = ESP_SLEEP_WAKEUP_UNDEFINED;

static char serialCommandBuffer[COMMAND_BUFFER_SIZE];
static size_t serialCommandLength = 0;
static char activeWifiSsid[WIFI_SSID_BUFFER_SIZE];
static char activeWifiPassword[WIFI_PASSWORD_BUFFER_SIZE];
static bool wifiCredentialsAvailable = false;
static uint32_t lastWifiReconnectAttemptMs = 0;
static bool mdnsStarted = false;
static bool otaStarted = false;
static bool configPortalActive = false;
static bool dnsServerStarted = false;
static uint32_t configPortalCloseAtMs = 0;
static LightState stateBeforeConfigPortal = STATE_IDLE;

static bool buttonRawPressed = false;
static bool buttonStablePressed = false;
static bool buttonReady = false;
static bool sleepArmed = false;
static bool wifiSetupArmed = false;
static uint32_t buttonRawChangedAtMs = 0;
static uint32_t buttonPressedAtMs = 0;

static const char *stateToText(LightState state);
static bool parseStateName(const char *text, LightState *outState);
static bool setState(LightState nextState);
static void renderAnimation(uint32_t nowMs);
static void setLeds(uint8_t red, uint8_t yellow, uint8_t green);
static void buildInfoJson(char *out, size_t outSize);
static void loadBrightness();
static void setBrightness(uint8_t value);
static bool processCommand(const char *command, char *response, size_t responseSize);
static void handleBleCommandBytes(const uint8_t *data, size_t length);
static void enterDeepSleep();
static void startConfigPortal();

static bool equalsIgnoreCase(const char *a, const char *b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a != '\0' && *b != '\0') {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static void trimInPlace(char *text) {
  if (text == nullptr) return;
  char *start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) ++start;
  if (start != text) memmove(text, start, strlen(start) + 1);
  size_t len = strlen(text);
  while (len > 0 && isspace((unsigned char)text[len - 1])) text[--len] = '\0';
}

static void safeCopy(char *out, size_t outSize, const char *in) {
  if (out == nullptr || outSize == 0) return;
  if (in == nullptr) {
    out[0] = '\0';
    return;
  }
  strncpy(out, in, outSize - 1);
  out[outSize - 1] = '\0';
}

static uint8_t scaleByGlobalBrightness(uint8_t value) {
  return (uint8_t)(((uint16_t)value * globalBrightness + 127U) / 255U);
}

static uint8_t sineBreath(uint32_t elapsedMs, uint32_t periodMs) {
  const uint32_t position = elapsedMs % periodMs;
  const uint32_t halfPeriod = periodMs / 2U;
  const uint32_t ramp = position <= halfPeriod ? position : periodMs - position;
  const uint32_t linear = (ramp * 255U) / halfPeriod;
  return (uint8_t)((linear * linear * (765U - (2U * linear))) / 16581375UL);
}

static uint8_t visiblePulse(uint32_t elapsedMs, uint32_t periodMs, uint8_t minimumValue) {
  const uint8_t wave = sineBreath(elapsedMs, periodMs);
  return minimumValue + (uint8_t)(((uint16_t)wave * (255U - minimumValue)) / 255U);
}

static const char *wakeCauseToText(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "POWER_ON";
    default: return "OTHER";
  }
}

static void releaseLedHoldsAfterWake() {
  gpio_deep_sleep_hold_dis();
  const uint8_t pins[] = {PIN_RED, PIN_YELLOW, PIN_GREEN};
  for (uint8_t pin : pins) {
    gpio_hold_dis((gpio_num_t)pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
}

static void setupLedPwm() {
  ledcAttach(PIN_RED, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttach(PIN_YELLOW, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttach(PIN_GREEN, LEDC_FREQ_HZ, LEDC_RES_BITS);
}

static void writeLedActiveLow(uint8_t pin, uint8_t value) {
  ledcWrite(pin, 255U - scaleByGlobalBrightness(value));
}

static void setLeds(uint8_t red, uint8_t yellow, uint8_t green) {
  writeLedActiveLow(PIN_RED, red);
  writeLedActiveLow(PIN_YELLOW, yellow);
  writeLedActiveLow(PIN_GREEN, green);
}

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
  if (text == nullptr || outState == nullptr) return false;
  if (equalsIgnoreCase(text, "OFF")) *outState = STATE_OFF;
  else if (equalsIgnoreCase(text, "IDLE")) *outState = STATE_IDLE;
  else if (equalsIgnoreCase(text, "THINKING")) *outState = STATE_THINKING;
  else if (equalsIgnoreCase(text, "CODING")) *outState = STATE_CODING;
  else if (equalsIgnoreCase(text, "BUILD")) *outState = STATE_BUILD;
  else if (equalsIgnoreCase(text, "SUCCESS")) *outState = STATE_SUCCESS;
  else if (equalsIgnoreCase(text, "ERROR")) *outState = STATE_ERROR;
  else if (equalsIgnoreCase(text, "WARNING")) *outState = STATE_WARNING;
  else if (equalsIgnoreCase(text, "OTA")) *outState = STATE_OTA;
  else return false;
  return true;
}

static bool setState(LightState nextState) {
  currentState = nextState;
  stateStartedAtMs = millis();
  renderAnimation(stateStartedAtMs);
  return true;
}

static void renderWorkCycle(uint32_t elapsedMs) {
  const uint32_t phaseMs = elapsedMs % 1440UL;
  const uint32_t localMs = (phaseMs % 480UL) / 2U;
  const uint8_t value = visiblePulse(localMs, 240U, 70U);
  if (phaseMs < 480UL) setLeds(0, 0, value);
  else if (phaseMs < 960UL) setLeds(0, value, 0);
  else setLeds(value, 0, 0);
}

static void renderAnimation(uint32_t nowMs) {
  if (wifiSetupArmed) {
    setLeds(0, 255, 0);
    return;
  }
  if (sleepArmed) {
    setLeds(0, 0, 0);
    return;
  }
  const uint32_t elapsedMs = nowMs - stateStartedAtMs;
  switch (currentState) {
    case STATE_OFF: setLeds(0, 0, 0); break;
    case STATE_IDLE:
      if (elapsedMs >= IDLE_LIGHT_OFF_MS) setLeds(0, 0, 0);
      else setLeds(0, 0, 255);
      break;
    case STATE_THINKING:
    case STATE_CODING:
    case STATE_BUILD: renderWorkCycle(elapsedMs); break;
    case STATE_SUCCESS: {
      if (elapsedMs >= 20000UL) {
        setState(STATE_IDLE);
        return;
      }
      const bool on = ((elapsedMs / 300UL) % 2UL) == 0UL;
      setLeds(0, 0, on ? 255 : 0);
      break;
    }
    case STATE_ERROR: {
      const bool on = ((elapsedMs / 300UL) % 2UL) == 0UL;
      setLeds(on ? 255 : 0, 0, 0);
      break;
    }
    case STATE_WARNING: {
      const bool on = ((elapsedMs / 300UL) % 2UL) == 0UL;
      setLeds(0, on ? 255 : 0, 0);
      break;
    }
    case STATE_OTA: {
      const uint8_t phase = (uint8_t)((elapsedMs / 250UL) % 3UL);
      setLeds(phase == 2 ? 255 : 0, phase == 1 ? 255 : 0, phase == 0 ? 255 : 0);
      break;
    }
  }
}

static void setupButton() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  buttonRawPressed = digitalRead(PIN_BUTTON) == LOW;
  buttonStablePressed = buttonRawPressed;
  buttonReady = !buttonStablePressed;
  buttonRawChangedAtMs = millis();
}

static void cycleLocalState() {
  static const LightState states[] = {
    STATE_IDLE, STATE_THINKING, STATE_CODING, STATE_BUILD,
    STATE_SUCCESS, STATE_ERROR, STATE_WARNING, STATE_OFF
  };
  const size_t count = sizeof(states) / sizeof(states[0]);
  for (size_t i = 0; i < count; ++i) {
    if (currentState == states[i]) {
      setState(states[(i + 1U) % count]);
      return;
    }
  }
  setState(STATE_IDLE);
}

static void serviceButton(uint32_t nowMs) {
  const bool rawPressed = digitalRead(PIN_BUTTON) == LOW;
  if (rawPressed != buttonRawPressed) {
    buttonRawPressed = rawPressed;
    buttonRawChangedAtMs = nowMs;
  }
  if (buttonRawPressed != buttonStablePressed &&
      nowMs - buttonRawChangedAtMs >= BUTTON_DEBOUNCE_MS) {
    buttonStablePressed = buttonRawPressed;
    if (buttonStablePressed) {
      if (buttonReady) {
        buttonPressedAtMs = nowMs;
        sleepArmed = false;
        wifiSetupArmed = false;
      }
    } else if (!buttonReady) {
      buttonReady = true;
    } else if (wifiSetupArmed) {
      wifiSetupArmed = false;
      startConfigPortal();
    } else if (sleepArmed) {
      enterDeepSleep();
    } else {
      cycleLocalState();
    }
  }
  const uint32_t heldMs = nowMs - buttonPressedAtMs;
  if (buttonReady && buttonStablePressed && !wifiSetupArmed &&
      heldMs >= BUTTON_WIFI_PRESS_MS) {
    sleepArmed = false;
    wifiSetupArmed = true;
    setLeds(0, 255, 0);
  } else if (buttonReady && buttonStablePressed && !sleepArmed &&
             heldMs >= BUTTON_SLEEP_PRESS_MS) {
    sleepArmed = true;
    setLeds(0, 0, 0);
  }
}

static void stopNetworkServices() {
  if (otaStarted) {
    ArduinoOTA.end();
    MDNS.end();
    otaStarted = false;
    mdnsStarted = false;
  } else if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
}

static void enterDeepSleep() {
  setLeds(0, 0, 0);
  delay(40);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  gpio_pullup_en((gpio_num_t)PIN_BUTTON);
  gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
  const esp_err_t result = esp_deep_sleep_enable_gpio_wakeup(
    1ULL << PIN_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (result != ESP_OK) {
    sleepArmed = false;
    setState(STATE_ERROR);
    return;
  }

  server.stop();
  if (dnsServerStarted) {
    dnsServer.stop();
    dnsServerStarted = false;
  }
  stopNetworkServices();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  BLEDevice::deinit(true);
  bleStarted = false;

  ledcDetach(PIN_RED);
  ledcDetach(PIN_YELLOW);
  ledcDetach(PIN_GREEN);
  const uint8_t pins[] = {PIN_RED, PIN_YELLOW, PIN_GREEN};
  for (uint8_t pin : pins) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en((gpio_num_t)pin);
  }
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

static void buildInfoJson(char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) return;
  const IPAddress ip = WiFi.localIP();
  const IPAddress apIp = WiFi.softAPIP();
  snprintf(out, outSize,
           "{\"variant\":\"battery\",\"state\":\"%s\",\"ip\":\"%u.%u.%u.%u\","
           "\"wifi\":%s,\"ap\":%s,\"ap_ip\":\"%u.%u.%u.%u\","
           "\"ble\":%s,\"ota\":%s,\"brightness\":%u,\"button\":%s,"
           "\"sleepArmed\":%s,\"wifiSetupArmed\":%s,\"wake\":\"%s\",\"uptime\":%lu}",
           stateToText(currentState), ip[0], ip[1], ip[2], ip[3],
           WiFi.status() == WL_CONNECTED ? "true" : "false",
           configPortalActive ? "true" : "false",
           apIp[0], apIp[1], apIp[2], apIp[3],
           bleStarted ? "true" : "false", otaStarted ? "true" : "false",
           globalBrightness, buttonStablePressed ? "true" : "false",
           sleepArmed ? "true" : "false", wifiSetupArmed ? "true" : "false",
           wakeCauseToText(wakeupCause),
           (unsigned long)millis());
}

static bool parseUnsignedByte(const char *text, uint8_t *outValue) {
  if (text == nullptr || outValue == nullptr || *text == '\0') return false;
  char *endPtr = nullptr;
  const long value = strtol(text, &endPtr, 10);
  if (endPtr == text) return false;
  while (*endPtr != '\0' && isspace((unsigned char)*endPtr)) ++endPtr;
  if (*endPtr != '\0' || value < 0 || value > 255) return false;
  *outValue = (uint8_t)value;
  return true;
}

static void loadBrightness() {
  if (!wifiPreferences.begin("codinglight", true)) return;
  globalBrightness = wifiPreferences.getUChar("brightness", globalBrightness);
  wifiPreferences.end();
}

static void setBrightness(uint8_t value) {
  const uint32_t nowMs = millis();
  globalBrightness = value;
  if (wifiPreferences.begin("codinglight", false)) {
    wifiPreferences.putUChar("brightness", value);
    wifiPreferences.end();
  }
  if (currentState == STATE_IDLE) {
    stateStartedAtMs = nowMs;
  }
  renderAnimation(nowMs);
}

static bool processCommand(const char *command, char *response, size_t responseSize) {
  if (response == nullptr || responseSize == 0) return false;
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
  char *argument = buffer;
  while (*argument != '\0' && !isspace((unsigned char)*argument)) ++argument;
  if (*argument != '\0') {
    *argument++ = '\0';
    while (*argument != '\0' && isspace((unsigned char)*argument)) ++argument;
  }
  if (equalsIgnoreCase(buffer, "STATE")) {
    LightState nextState;
    if (parseStateName(argument, &nextState) && setState(nextState)) {
      snprintf(response, responseSize, "OK");
      return true;
    }
  } else if (equalsIgnoreCase(buffer, "BRIGHTNESS")) {
    uint8_t value;
    if (parseUnsignedByte(argument, &value)) {
      setBrightness(value);
      snprintf(response, responseSize, "OK");
      return true;
    }
  }
  snprintf(response, responseSize, "ERR");
  return false;
}

static void processSerialCommand() {
  serialCommandBuffer[serialCommandLength] = '\0';
  char response[RESPONSE_BUFFER_SIZE];
  processCommand(serialCommandBuffer, response, sizeof(response));
  Serial.println(response);
  serialCommandLength = 0;
}

static void serviceSerial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialCommandLength > 0) processSerialCommand();
      continue;
    }
    if (serialCommandLength < sizeof(serialCommandBuffer) - 1) {
      serialCommandBuffer[serialCommandLength++] = c;
    } else {
      serialCommandLength = 0;
      Serial.println("ERR");
    }
  }
}

class CodingLightServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *serverInstance) override {
    (void)serverInstance;
    bleClientConnected = true;
  }
  void onDisconnect(BLEServer *serverInstance) override {
    (void)serverInstance;
    bleClientConnected = false;
    BLEDevice::startAdvertising();
  }
};

class CodingLightRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() > 0) {
      handleBleCommandBytes((const uint8_t *)value.c_str(), value.length());
    }
  }
};

static CodingLightServerCallbacks bleServerCallbacks;
static CodingLightRxCallbacks bleRxCallbacks;
static BLE2902 bleTxClientConfigDescriptor;

static void queueBleResponse(const char *response) {
  if (response == nullptr) return;
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
  queueBleResponse(response);
}

static void handleBleCommandBytes(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0) return;
  for (size_t i = 0; i < length; ++i) {
    const char c = (char)data[i];
    if (c == '\r') continue;
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
      queueBleResponse("ERR");
    }
  }
}

static void serviceBleRx() {
  bool shouldFlush;
  portENTER_CRITICAL(&bleCommandMux);
  shouldFlush = bleCommandLength > 0 && millis() - bleLastRxMs >= 80UL;
  portEXIT_CRITICAL(&bleCommandMux);
  if (shouldFlush) processBleBufferedCommand();
}

static void serviceBleTx() {
  if (!bleStarted || !bleClientConnected || bleTxCharacteristic == nullptr) return;
  char response[RESPONSE_BUFFER_SIZE];
  portENTER_CRITICAL(&bleResponseMux);
  if (bleResponseCount == 0) {
    portEXIT_CRITICAL(&bleResponseMux);
    return;
  }
  strncpy(response, bleResponseQueue[bleResponseTail], sizeof(response) - 1);
  response[sizeof(response) - 1] = '\0';
  bleResponseTail = (uint8_t)((bleResponseTail + 1U) % BLE_RESPONSE_QUEUE_DEPTH);
  --bleResponseCount;
  portEXIT_CRITICAL(&bleResponseMux);
  const size_t totalLength = strlen(response);
  for (size_t offset = 0; offset < totalLength; offset += 20U) {
    const size_t remaining = totalLength - offset;
    const size_t chunkLength = remaining > 20U ? 20U : remaining;
    bleTxCharacteristic->setValue((uint8_t *)(response + offset), chunkLength);
    bleTxCharacteristic->notify();
  }
}

static void setupBle() {
  BLEDevice::init(DEVICE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(&bleServerCallbacks);
  BLEService *service = bleServer->createService(NUS_SERVICE_UUID);
  bleTxCharacteristic = service->createCharacteristic(
    NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(&bleTxClientConfigDescriptor);
  BLECharacteristic *rx = service->createCharacteristic(
    NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(&bleRxCallbacks);
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  bleStarted = true;
}

static void configureWifiRadio() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
}

static void startOtaIfNeeded() {
  if (otaStarted || WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname(MDNS_NAME);
  if (OTA_PASSWORD[0] != '\0') ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA
    .onStart([]() { setState(STATE_OTA); })
    .onError([](ota_error_t error) {
      (void)error;
      setState(STATE_ERROR);
    });
  ArduinoOTA.begin();
  otaStarted = true;
  MDNS.addService("http", "tcp", 80);
  mdnsStarted = true;
}

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
  if (ssid == nullptr || ssid[0] == '\0') return false;
  if (!wifiPreferences.begin("codinglight", false)) return false;

  const size_t savedSsid = wifiPreferences.putString("ssid", ssid);
  wifiPreferences.putString("pass", password != nullptr ? password : "");
  wifiPreferences.end();
  if (savedSsid == 0) return false;

  safeCopy(activeWifiSsid, sizeof(activeWifiSsid), ssid);
  safeCopy(activeWifiPassword, sizeof(activeWifiPassword), password != nullptr ? password : "");
  wifiCredentialsAvailable = true;
  return true;
}

static void startConfigPortal() {
  if (configPortalActive) return;

  WiFi.mode(WIFI_AP_STA);
  configureWifiRadio();
  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, gateway, subnet);

  if (WiFi.softAP(CONFIG_AP_SSID)) {
    configPortalActive = true;
    configPortalCloseAtMs = 0;
    dnsServerStarted = dnsServer.start(DNS_PORT, "*", apIp);
    stateBeforeConfigPortal = currentState;
    setState(STATE_WARNING);
    Serial.println("CONFIG_AP_STARTED");
    Serial.println(apIp);
  } else {
    setState(STATE_ERROR);
    Serial.println("CONFIG_AP_FAILED");
  }
}

static void stopConfigPortal() {
  if (!configPortalActive) return;
  const bool restoreState = currentState == STATE_WARNING;
  if (dnsServerStarted) {
    dnsServer.stop();
    dnsServerStarted = false;
  }
  WiFi.softAPdisconnect(true);
  configPortalActive = false;
  configPortalCloseAtMs = 0;
  if (WiFi.status() == WL_CONNECTED || wifiCredentialsAvailable) {
    WiFi.mode(WIFI_STA);
    configureWifiRadio();
  }
  if (restoreState) setState(stateBeforeConfigPortal);
  Serial.println("CONFIG_AP_STOPPED");
}

static void beginWifiAttempt(uint32_t nowMs) {
  lastWifiReconnectAttemptMs = nowMs;
  if (!wifiCredentialsAvailable) return;

  stopNetworkServices();
  WiFi.mode(configPortalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(false);
  configureWifiRadio();
  WiFi.disconnect(false, false);
  delay(100);
  configureWifiRadio();
  WiFi.begin(activeWifiSsid, activeWifiPassword);
}

static void setupWifi() {
  configureWifiRadio();
  WiFi.setHostname(DEVICE_NAME);
  if (loadWifiCredentials()) {
    beginWifiAttempt(millis());
  } else {
    startConfigPortal();
  }
}

static void serviceWifi(uint32_t nowMs) {
  if (dnsServerStarted) dnsServer.processNextRequest();

  if (WiFi.status() == WL_CONNECTED) {
    startOtaIfNeeded();
    if (configPortalCloseAtMs != 0 && nowMs >= configPortalCloseAtMs) {
      stopConfigPortal();
    }
    return;
  }

  stopNetworkServices();
  if (!wifiCredentialsAvailable) {
    startConfigPortal();
    return;
  }
  if (lastWifiReconnectAttemptMs == 0 ||
      nowMs - lastWifiReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    beginWifiAttempt(nowMs);
  }
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CodingLight Battery</title><style>
body{font-family:system-ui,sans-serif;margin:0;padding:20px;background:#f4f6f8;color:#111}
main{max-width:680px;margin:auto;background:#fff;border:1px solid #ccd3db;border-radius:8px;padding:20px}
h1{font-size:24px;margin:0 0 14px}.state{font-size:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:8px;margin:18px 0}
button{border:1px solid #aeb8c2;background:#fff;border-radius:6px;padding:11px 8px;font-weight:650}
button.active{background:#134e4a;color:#fff}.row{display:flex;justify-content:space-between}
input{width:100%}.small{color:#57606a;font-size:13px;margin-top:14px}
</style></head><body><main><h1>CodingLight Battery</h1>
<p class="state">Current State: <strong id="state">...</strong></p>
<div class="grid" id="buttons"></div><div class="row">
<label for="brightness">Brightness</label><span id="brightnessValue">...</span></div>
<input id="brightness" type="range" min="0" max="255" step="1">
<p class="small" id="meta"></p></main><script>
const states=["OFF","IDLE","THINKING","CODING","BUILD","SUCCESS","ERROR","WARNING","OTA"];
const buttons=document.getElementById("buttons"),stateEl=document.getElementById("state");
const slider=document.getElementById("brightness"),brightnessValue=document.getElementById("brightnessValue");
const meta=document.getElementById("meta");
for(const s of states){const b=document.createElement("button");b.textContent=s;b.dataset.state=s;
b.onclick=async()=>{await fetch("/api/state",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({state:s})});await refresh()};buttons.appendChild(b)}
let brightnessTimer=0;slider.oninput=()=>{brightnessValue.textContent=slider.value;clearTimeout(brightnessTimer);
brightnessTimer=setTimeout(async()=>{await fetch("/api/brightness",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({brightness:Number(slider.value)})});await refresh()},120)};
async function refresh(){const r=await fetch("/api/info",{cache:"no-store"});const info=await r.json();
stateEl.textContent=info.state;slider.value=info.brightness;brightnessValue.textContent=info.brightness;
meta.textContent=`IP ${info.ip} | WiFi ${info.wifi?"on":"off"} | BLE ${info.ble?"on":"off"} | OTA ${info.ota?"ready":"off"} | Wake ${info.wake}`;
for(const b of buttons.children)b.classList.toggle("active",b.dataset.state===info.state)}
refresh();setInterval(refresh,2000);</script></body></html>
)HTML";

static bool extractJsonStringValue(const char *body, const char *key, char *out, size_t outSize) {
  if (!body || !key || !out || outSize == 0) return false;
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *keyPos = strstr(body, pattern);
  if (!keyPos) return false;
  const char *colon = strchr(keyPos + strlen(pattern), ':');
  if (!colon) return false;
  const char *value = colon + 1;
  while (*value && isspace((unsigned char)*value)) ++value;
  if (*value++ != '"') return false;
  size_t len = 0;
  while (value[len] && value[len] != '"' && len < outSize - 1) out[len++] = value[len];
  if (value[len] != '"') return false;
  out[len] = '\0';
  return len > 0;
}

static bool extractJsonIntValue(const char *body, const char *key, long *out) {
  if (!body || !key || !out) return false;
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *keyPos = strstr(body, pattern);
  if (!keyPos) return false;
  const char *colon = strchr(keyPos + strlen(pattern), ':');
  if (!colon) return false;
  char *endPtr = nullptr;
  *out = strtol(colon + 1, &endPtr, 10);
  return endPtr != colon + 1;
}

static void sendPlain(uint16_t code, const char *text) {
  server.send(code, "text/plain", text);
}

static void sendConfigPage(const char *message, bool isError) {
  const IPAddress apIp = WiFi.softAPIP();
  const bool staConnected = WiFi.status() == WL_CONNECTED;
  char page[3200];
  snprintf(page, sizeof(page),
           "<!doctype html><html><head>"
           "<meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>CodingLight Battery WiFi Setup</title>"
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
           "<h1>CodingLight Battery WiFi Setup</h1>"
           "%s%s%s"
           "<form method=\"post\" action=\"/config\">"
           "<label for=\"ssid\">WiFi SSID</label>"
           "<input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required autocomplete=\"off\">"
           "<label for=\"password\">WiFi Password</label>"
           "<input id=\"password\" name=\"password\" maxlength=\"64\" type=\"password\" autocomplete=\"current-password\">"
           "<button type=\"submit\">Save and Connect</button>"
           "</form>"
           "<p class=\"meta\">Setup AP: %s<br>AP IP: %u.%u.%u.%u<br>Station: %s<br>"
           "Saved credentials remain unchanged until a new SSID is submitted.</p>"
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

static void handleRoot() {
  if (configPortalActive) {
    sendConfigPage("", false);
    return;
  }
  server.send_P(200, "text/html", INDEX_HTML);
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
  sendConfigPage("Saved. CodingLight Battery is connecting now.", false);
}

static void redirectToConfigPortal() {
  const IPAddress apIp = WiFi.softAPIP();
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
  server.arg("plain").toCharArray(body, sizeof(body));
  char stateText[24];
  LightState nextState;
  if (!extractJsonStringValue(body, "state", stateText, sizeof(stateText)) ||
      !parseStateName(stateText, &nextState) || !setState(nextState)) {
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
  server.arg("plain").toCharArray(body, sizeof(body));
  long value;
  if (!extractJsonIntValue(body, "brightness", &value) || value < 0 || value > 255) {
    sendPlain(400, "ERR");
    return;
  }
  setBrightness((uint8_t)value);
  sendPlain(200, "OK");
}

static void setupHttp() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/config", HTTP_GET, []() { sendConfigPage("", false); });
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/generate_204", HTTP_GET, redirectToConfigPortal);
  server.on("/hotspot-detect.html", HTTP_GET, redirectToConfigPortal);
  server.on("/connecttest.txt", HTTP_GET, redirectToConfigPortal);
  server.on("/fwlink", HTTP_GET, redirectToConfigPortal);
  server.on("/api/info", HTTP_GET, handleApiInfo);
  server.on("/api/state", HTTP_POST, handleApiState);
  server.on("/api/brightness", HTTP_POST, handleApiBrightness);
  server.onNotFound([]() {
    if (configPortalActive) redirectToConfigPortal();
    else sendPlain(404, "ERR");
  });
  server.begin();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  wakeupCause = esp_sleep_get_wakeup_cause();
  releaseLedHoldsAfterWake();
  setupLedPwm();
  setLeds(0, 0, 0);
  loadBrightness();
  setupButton();
  setState(STATE_IDLE);
  setupBle();
  setupWifi();
  setupHttp();
}

void loop() {
  const uint32_t nowMs = millis();
  renderAnimation(nowMs);
  serviceButton(nowMs);
  serviceSerial();
  serviceBleRx();
  serviceBleTx();
  serviceWifi(nowMs);
  if (otaStarted) ArduinoOTA.handle();
  server.handleClient();
  const uint32_t sleepCheckMs = millis();
  if (currentState == STATE_IDLE && sleepCheckMs - stateStartedAtMs >= IDLE_SLEEP_MS) {
    enterDeepSleep();
  }
  delay(1);
}
