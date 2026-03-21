/*
  Project: DuoiLedRGB
  File: DuoiLedRGB.ino
  Description:
    Smart LED bedside controller for ESP8266/ESP32. The sketch drives a
    NeoPixel strip, reads an ultrasonic sensor to adjust brightness, uses an
    ambient light input to gate output, and exposes a WiFi web UI for control
    and WiFi setup.

  Copyright (c) 2026 TranDangKhoaTechnology.
  All rights reserved.
*/

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
typedef ESP8266WebServer LedWebServer;
#elif defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
typedef WebServer LedWebServer;
#else
#error "This sketch needs ESP8266 or ESP32 WiFi support."
#endif

// ====== LED config ======
#define LED_PIN                 D5
#define LED_COUNT               8
#define LED_BUFFER_BYTES        (LED_COUNT * 3)
#define AUTO_BRIGHTNESS         20
#define LED_TYPE                (NEO_GRB + NEO_KHZ800)

// ====== Auto effect timing ======
#define SPEED_SLOW              120

// ====== GPIO0 button config ======
#define BUTTON_PIN              0
#define BUTTON_ACTIVE_LEVEL     LOW
#define BUTTON_DEBOUNCE_MS      40UL
#define BUTTON_HOLD_TRIGGER_MS  10000UL

// ====== Ultrasonic config ======
// Change these 2 pins if your sensor is wired elsewhere.
#define US_TRIG_PIN             D6
#define US_ECHO_PIN             D7
#define US_SAMPLE_INTERVAL_MS   35UL
#define US_TIMEOUT_US           16000UL
#define US_NEAR_CM              8.0f
#define US_FAR_CM               50.0f
#define US_VALID_MAX_CM         65.0f
#define US_READ_ATTEMPTS        2
#define US_RETRY_GAP_US         1500UL
#define US_FILTER_WINDOW_SIZE   5
#define US_DISTANCE_DEADBAND_CM 1.5f
#define US_FILTER_ALPHA         0.22f
#define US_BRIGHTNESS_DEADBAND  2
#define US_STEP_CONFIRM_CM      35.0f
#define US_STEP_CONFIRM_SAMPLES 2
#define US_CANDIDATE_TOLERANCE_CM 12.0f
#define BRIGHTNESS_STEP_UP_MAX  24
#define BRIGHTNESS_STEP_DOWN_MAX 30
#define BRIGHTNESS_UPDATE_INTERVAL_MS 35UL
#define BRIGHTNESS_TARGET_TOLERANCE   8
#define BRIGHTNESS_TARGET_SAMPLES     2
#define CUSTOM_MIN_BRIGHTNESS   1
#define DISTANCE_MIN_BRIGHTNESS 8
#define CUSTOM_MAX_BRIGHTNESS   255
#define CUSTOM_FIXED_BRIGHTNESS_DEFAULT  CUSTOM_MAX_BRIGHTNESS

// ====== Ambient light config ======
#define AMBIENT_LIGHT_PIN           D1
#define AMBIENT_LIGHT_BRIGHT_LEVEL  LOW

// ====== Device identity ======
#define DEVICE_DISPLAY_NAME     "Đèn ngủ Hành tinh thông minh"

// ====== WiFi / Web config ======
#define WIFI_AP_SSID            "HanhTinhThongMinh"
#define WIFI_AP_PASSWORD        "hanhtinh123"
#define WIFI_STA_CONNECT_MS     12000UL
#define WIFI_SETTINGS_EEPROM_SIZE 256
#define WIFI_SETTINGS_MAGIC     0x48545032UL
#define WIFI_STA_SSID_DEFAULT   ""
#define WIFI_STA_PASSWORD_DEFAULT ""
#define WIFI_STA_SSID_MAX_LEN   32
#define WIFI_STA_PASSWORD_MAX_LEN 64

// ====== Serial debug config ======
#define DEBUG_SERIAL_ENABLED    1
#define DEBUG_SERIAL_BAUD       115200
#define DEBUG_SENSOR_LOG_MS     400UL

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, LED_TYPE);
LedWebServer webServer(80);

// Core runtime modes and options shared by the renderer, button handler and web UI.
enum RunMode : uint8_t {
  MODE_AUTO_EFFECTS,
  MODE_CUSTOM
};

enum AutoEffect : uint8_t {
  EFFECT_SEQUENCE,
  EFFECT_COMET,
  EFFECT_THEATER,
  EFFECT_RAINBOW,
  EFFECT_LARSON,
  EFFECT_DUAL,
  EFFECT_COMET_SPARKLE,
  EFFECT_COLOR_WIPE,
  EFFECT_RANDOM_DOT,
  EFFECT_COUNT
};

enum LightGateMode : uint8_t {
  LIGHT_GATE_BRIGHT_ONLY,
  LIGHT_GATE_DARK_ONLY,
  LIGHT_GATE_ALWAYS_ON
};

enum LedColorOrder : uint8_t {
  LED_COLOR_GRB,
  LED_COLOR_RGB,
  LED_COLOR_BRG,
  LED_COLOR_GBR,
  LED_COLOR_RBG,
  LED_COLOR_BGR
};

// State containers centralize sampled inputs, render state and persisted settings.
struct ButtonState {
  bool rawPressed;
  bool stablePressed;
  bool longHoldHandled;
  uint32_t lastRawChangeMs;
  uint32_t pressStartMs;
};

struct DistanceState {
  bool hasReading;
  bool lastReadOk;
  uint32_t lastSampleMs;
  uint32_t lastSuccessMs;
  uint32_t lastBrightnessApplyMs;
  float rawCm;
  float medianCm;
  float filteredCm;
  float candidateCm;
  uint8_t candidateCount;
  uint8_t brightness;
  uint8_t targetBrightness;
  uint8_t pendingBrightness;
  uint8_t pendingBrightnessCount;
};

struct DebugState {
  uint32_t lastSensorLogMs;
};

struct AmbientLightState {
  bool isBright;
  bool outputAllowed;
  bool outputSuppressed;
};

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct StoredWiFiSettings {
  uint32_t magic;
  char apSsid[WIFI_STA_SSID_MAX_LEN + 1];
  char apPassword[WIFI_STA_PASSWORD_MAX_LEN + 1];
  char staSsid[WIFI_STA_SSID_MAX_LEN + 1];
  char staPassword[WIFI_STA_PASSWORD_MAX_LEN + 1];
};

const RgbColor CUSTOM_PALETTE[] = {
  {255, 0, 0},
  {0, 255, 0},
  {0, 0, 255},
  {255, 120, 0},
  {255, 0, 255},
  {0, 255, 255},
  {255, 255, 255},
  {255, 255, 0}
};

const uint8_t CUSTOM_PALETTE_COUNT = sizeof(CUSTOM_PALETTE) / sizeof(CUSTOM_PALETTE[0]);

// Global runtime state shared across sensor updates, LED rendering and HTTP handlers.
RunMode currentMode = MODE_CUSTOM;
AutoEffect selectedAutoEffect = EFFECT_SEQUENCE;
LightGateMode lightGateMode = LIGHT_GATE_DARK_ONLY;
ButtonState buttonState = {};
DistanceState distanceState = {
  false,
  false,
  0,
  0,
  0,
  -1.0f,
  US_FAR_CM,
  US_FAR_CM,
  US_FAR_CM,
  0,
  CUSTOM_MAX_BRIGHTNESS,
  CUSTOM_MAX_BRIGHTNESS,
  CUSTOM_MAX_BRIGHTNESS,
  0
};
DebugState debugState = {0};
AmbientLightState ambientLightState = {true, true, false};
uint8_t customColorIndex = 0;
RgbColor customPickerColor = {255, 255, 255};
bool useCustomPickerColor = false;
bool customModeNeedsRefresh = true;
bool effectModeNeedsRefresh = false;
bool distanceBrightnessEnabled = true;
uint8_t fixedCustomBrightness = CUSTOM_FIXED_BRIGHTNESS_DEFAULT;
LedColorOrder ledColorOrder = LED_COLOR_GRB;
StoredWiFiSettings wifiSettings = {};
bool accessPointRestartPending = false;
uint32_t accessPointRestartAtMs = 0;
float distanceFilterWindow[US_FILTER_WINDOW_SIZE] = {};
uint8_t distanceFilterCount = 0;
uint8_t distanceFilterIndex = 0;

// Forward declarations for helpers referenced before their definitions.
void updateAmbientLightState();
void updateDistanceFromSensor();
void requestCurrentModeRefresh();
RgbColor unpackColor(uint32_t color);
RgbColor scaleRgbColor(const RgbColor &color, uint8_t scale);
void loadStoredWiFiSettings();
bool saveStoredWiFiSettings();
void reconnectStationWiFi(bool waitForConnection, uint32_t timeoutMs = WIFI_STA_CONNECT_MS);
void clearStoredWiFiSettings();
String jsonEscape(const String &value);
const char *stationConnectionStatusText();
void restartAccessPoint();
void scheduleAccessPointRestart(uint16_t delayMs = 300);
void processPendingWiFiTasks();
bool restoreWiFiDefaults();

// ====== Shared helpers ======
static inline bool isMode(RunMode mode) {
  return currentMode == mode;
}

uint8_t currentBrightnessValue();
bool parseHexColor(const String &value, RgbColor &color);

static inline void showStrip() {
  updateAmbientLightState();
  if (!ambientLightState.outputAllowed) {
    return;
  }

  uint8_t brightness = currentBrightnessValue();
  uint8_t *pixels = strip.getPixels();
  uint8_t backup[LED_BUFFER_BYTES];

  memcpy(backup, pixels, sizeof(backup));

  if (brightness == 0) {
    memset(pixels, 0, sizeof(backup));
  } else if (brightness < 255) {
    for (uint16_t i = 0; i < sizeof(backup); i++) {
      pixels[i] = (uint16_t)backup[i] * brightness / 255;
    }
  }

  ambientLightState.outputSuppressed = false;
  strip.show();
  memcpy(pixels, backup, sizeof(backup));
}

static inline void clearStrip() {
  strip.clear();
  showStrip();
}

#if 0
const char *modeNameText(RunMode mode) {
  return mode == MODE_CUSTOM ? "Màu cố định" : "Hiệu ứng";
}

#endif

const char *modeNameText(RunMode mode) {
  (void)mode;
  return "Màu đơn sắc";
}

const char *autoEffectNameText(AutoEffect effect) {
  switch (effect) {
    case EFFECT_SEQUENCE:       return "Chuỗi tự động";
    case EFFECT_COMET:          return "Sao chổi";
    case EFFECT_THEATER:        return "Rượt đuổi";
    case EFFECT_RAINBOW:        return "Cầu vồng";
    case EFFECT_LARSON:         return "Quét Larson";
    case EFFECT_DUAL:           return "Đuổi đôi";
    case EFFECT_COMET_SPARKLE:  return "Lấp lánh";
    case EFFECT_COLOR_WIPE:     return "Lau màu";
    case EFFECT_RANDOM_DOT:     return "Chấm ngẫu nhiên";
    default:                    return "Không rõ";
  }
}

const char *autoEffectButtonLabel(AutoEffect effect) {
  switch (effect) {
    case EFFECT_SEQUENCE:       return "Tự động";
    case EFFECT_COMET:          return "Sao chổi";
    case EFFECT_THEATER:        return "Rượt đuổi";
    case EFFECT_RAINBOW:        return "Cầu vồng";
    case EFFECT_LARSON:         return "Quét";
    case EFFECT_DUAL:           return "Đôi";
    case EFFECT_COMET_SPARKLE:  return "Lấp lánh";
    case EFFECT_COLOR_WIPE:     return "Lau màu";
    case EFFECT_RANDOM_DOT:     return "Ngẫu nhiên";
    default:                    return "Không rõ";
  }
}

#if 0
const char *lightGateModeText(LightGateMode mode) {
  switch (mode) {
    case LIGHT_GATE_BRIGHT_ONLY:  return "Bật đèn khi sáng";
    case LIGHT_GATE_DARK_ONLY:    return "Bật đèn khi tối";
    case LIGHT_GATE_ALWAYS_ON:    return "Luôn bật";
    default:                      return "Không rõ";
  }
}

const char *lightGateModeButtonLabel(LightGateMode mode) {
  switch (mode) {
    case LIGHT_GATE_BRIGHT_ONLY:  return "Bật đèn khi sáng";
    case LIGHT_GATE_DARK_ONLY:    return "Bật đèn khi tối";
    case LIGHT_GATE_ALWAYS_ON:    return "Luôn bật";
    default:                      return "Không rõ";
  }
}

const char *lightLevelText(bool isBright) {
  return isBright ? "Sáng" : "Tối";
}
#endif

const char *lightGateModeText(LightGateMode mode) {
  switch (mode) {
    case LIGHT_GATE_BRIGHT_ONLY:  return "Bật đèn khi sáng";
    case LIGHT_GATE_DARK_ONLY:    return "Bật đèn khi tối";
    case LIGHT_GATE_ALWAYS_ON:    return "Luôn bật";
    default:                      return "Không rõ";
  }
}

const char *lightGateModeButtonLabel(LightGateMode mode) {
  return lightGateModeText(mode);
}

const char *lightLevelText(bool isBright) {
  return isBright ? "Sáng" : "Tối";
}

neoPixelType ledColorOrderPixelType(LedColorOrder order) {
  switch (order) {
    case LED_COLOR_RGB:  return (neoPixelType)(NEO_RGB + NEO_KHZ800);
    case LED_COLOR_BRG:  return (neoPixelType)(NEO_BRG + NEO_KHZ800);
    case LED_COLOR_GBR:  return (neoPixelType)(NEO_GBR + NEO_KHZ800);
    case LED_COLOR_RBG:  return (neoPixelType)(NEO_RBG + NEO_KHZ800);
    case LED_COLOR_BGR:  return (neoPixelType)(NEO_BGR + NEO_KHZ800);
    case LED_COLOR_GRB:
    default:             return (neoPixelType)(NEO_GRB + NEO_KHZ800);
  }
}

uint32_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) {
    return strip.Color(255 - pos * 3, 0, pos * 3);
  }
  if (pos < 170) {
    pos -= 85;
    return strip.Color(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return strip.Color(pos * 3, 255 - pos * 3, 0);
}

uint32_t makeColor(const RgbColor &color) {
  return strip.Color(color.r, color.g, color.b);
}

RgbColor currentCustomColor() {
  if (useCustomPickerColor) {
    return customPickerColor;
  }

  return CUSTOM_PALETTE[customColorIndex];
}

String colorToHex(const RgbColor &color) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
  return String(buffer);
}

bool parseHexColor(const String &value, RgbColor &color) {
  String hex = value;
  if (hex.length() == 0) {
    return false;
  }

  if (hex[0] == '#') {
    hex.remove(0, 1);
  }

  if (hex.length() != 6) {
    return false;
  }

  char *endPtr = nullptr;
  unsigned long packed = strtoul(hex.c_str(), &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0' || packed > 0xFFFFFFUL) {
    return false;
  }

  color.r = (packed >> 16) & 0xFF;
  color.g = (packed >> 8) & 0xFF;
  color.b = packed & 0xFF;
  return true;
}

uint8_t scaleChannelLinear(uint8_t channel, uint8_t scale) {
  return (uint16_t(channel) * scale + 127) / 255;
}

uint8_t scaleChannelForBrightness(uint8_t channel, uint8_t brightness) {
  return scaleChannelLinear(channel, brightness);
}

RgbColor scaleRgbColor(const RgbColor &color, uint8_t scale) {
  RgbColor scaled = color;
  scaled.r = scaleChannelForBrightness(scaled.r, scale);
  scaled.g = scaleChannelForBrightness(scaled.g, scale);
  scaled.b = scaleChannelForBrightness(scaled.b, scale);
  return scaled;
}

RgbColor unpackColor(uint32_t color) {
  RgbColor rgb = {
    (uint8_t)((color >> 16) & 0xFF),
    (uint8_t)((color >> 8) & 0xFF),
    (uint8_t)(color & 0xFF)
  };
  return rgb;
}

uint32_t scaleColor(uint32_t color, uint8_t scale) {
  RgbColor rgb = unpackColor(color);

  rgb.r = scaleChannelLinear(rgb.r, scale);
  rgb.g = scaleChannelLinear(rgb.g, scale);
  rgb.b = scaleChannelLinear(rgb.b, scale);

  return strip.Color(rgb.r, rgb.g, rgb.b);
}

RgbColor currentCustomOutputColor() {
  return scaleRgbColor(currentCustomColor(), currentBrightnessValue());
}

float absDistanceDelta(float a, float b) {
  return a >= b ? (a - b) : (b - a);
}

void pushDistanceSample(float cm) {
  distanceFilterWindow[distanceFilterIndex] = cm;
  distanceFilterIndex = (distanceFilterIndex + 1) % US_FILTER_WINDOW_SIZE;
  if (distanceFilterCount < US_FILTER_WINDOW_SIZE) {
    distanceFilterCount++;
  }
}

float medianDistanceSample() {
  if (distanceFilterCount == 0) {
    return US_FAR_CM;
  }

  float sorted[US_FILTER_WINDOW_SIZE];
  for (uint8_t i = 0; i < distanceFilterCount; i++) {
    sorted[i] = distanceFilterWindow[i];
  }

  for (uint8_t i = 1; i < distanceFilterCount; i++) {
    float value = sorted[i];
    int8_t j = i - 1;
    while (j >= 0 && sorted[j] > value) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = value;
  }

  uint8_t mid = distanceFilterCount / 2;
  if ((distanceFilterCount & 1U) != 0) {
    return sorted[mid];
  }

  return (sorted[mid - 1] + sorted[mid]) * 0.5f;
}

void fillStripColor(uint32_t color) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color);
  }
}

void fillStripRgb(const RgbColor &color) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color.r, color.g, color.b);
  }
}

uint16_t nextDelaySlice(uint32_t startMs, uint16_t totalMs) {
  uint32_t elapsed = millis() - startMs;
  if (elapsed >= totalMs) {
    return 0;
  }

  uint32_t remaining = totalMs - elapsed;
  return remaining > 10 ? 10 : (uint16_t)remaining;
}

void copySettingText(char *dest, size_t destSize, const String &value) {
  if (destSize == 0) {
    return;
  }

  String trimmed = value;
  trimmed.trim();
  trimmed.toCharArray(dest, destSize);
  dest[destSize - 1] = '\0';
}

void clearStoredWiFiSettings() {
  memset(&wifiSettings, 0, sizeof(wifiSettings));
  wifiSettings.magic = WIFI_SETTINGS_MAGIC;
  strncpy(wifiSettings.apSsid, WIFI_AP_SSID, WIFI_STA_SSID_MAX_LEN);
  strncpy(wifiSettings.apPassword, WIFI_AP_PASSWORD, WIFI_STA_PASSWORD_MAX_LEN);
  strncpy(wifiSettings.staSsid, WIFI_STA_SSID_DEFAULT, WIFI_STA_SSID_MAX_LEN);
  strncpy(wifiSettings.staPassword, WIFI_STA_PASSWORD_DEFAULT, WIFI_STA_PASSWORD_MAX_LEN);
  wifiSettings.apSsid[WIFI_STA_SSID_MAX_LEN] = '\0';
  wifiSettings.apPassword[WIFI_STA_PASSWORD_MAX_LEN] = '\0';
  wifiSettings.staSsid[WIFI_STA_SSID_MAX_LEN] = '\0';
  wifiSettings.staPassword[WIFI_STA_PASSWORD_MAX_LEN] = '\0';
}

void loadStoredWiFiSettings() {
  EEPROM.begin(WIFI_SETTINGS_EEPROM_SIZE);
  EEPROM.get(0, wifiSettings);

  if (wifiSettings.magic != WIFI_SETTINGS_MAGIC) {
    clearStoredWiFiSettings();
    saveStoredWiFiSettings();
    return;
  }

  wifiSettings.apSsid[WIFI_STA_SSID_MAX_LEN] = '\0';
  wifiSettings.apPassword[WIFI_STA_PASSWORD_MAX_LEN] = '\0';
  wifiSettings.staSsid[WIFI_STA_SSID_MAX_LEN] = '\0';
  wifiSettings.staPassword[WIFI_STA_PASSWORD_MAX_LEN] = '\0';
}

bool saveStoredWiFiSettings() {
  wifiSettings.magic = WIFI_SETTINGS_MAGIC;
  EEPROM.put(0, wifiSettings);
  return EEPROM.commit();
}

bool hasStationCredentials() {
  return wifiSettings.staSsid[0] != '\0';
}

String stationIpText() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
}

const char *stationConnectionStatusText() {
  if (!hasStationCredentials()) {
    return "Chưa cấu hình";
  }

  switch (WiFi.status()) {
    case WL_CONNECTED:
      return "Đã kết nối";
    case WL_NO_SSID_AVAIL:
      return "Không tìm thấy SSID";
    case WL_CONNECT_FAILED:
      return "Kết nối thất bại";
    case WL_IDLE_STATUS:
      return "Đang chờ kết nối";
    case WL_DISCONNECTED:
      return "Chưa kết nối";
#ifdef WL_WRONG_PASSWORD
    case WL_WRONG_PASSWORD:
      return "Sai mật khẩu";
#endif
    default:
      return "Đang kết nối";
  }
}

void reconnectStationWiFi(bool waitForConnection, uint32_t timeoutMs) {
  WiFi.disconnect();
  delay(100);

  if (!hasStationCredentials()) {
    return;
  }

  WiFi.begin(wifiSettings.staSsid, wifiSettings.staPassword);
  if (!waitForConnection) {
    return;
  }

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < timeoutMs) {
    delay(250);
    yield();
  }
}

void restartAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPdisconnect(false);
  delay(100);
  WiFi.softAP(wifiSettings.apSsid, wifiSettings.apPassword);
}

void scheduleAccessPointRestart(uint16_t delayMs) {
  accessPointRestartPending = true;
  accessPointRestartAtMs = millis() + delayMs;
}

void processPendingWiFiTasks() {
  if (!accessPointRestartPending) {
    return;
  }

  if ((int32_t)(millis() - accessPointRestartAtMs) < 0) {
    return;
  }

  accessPointRestartPending = false;
  restartAccessPoint();
}

bool restoreWiFiDefaults() {
  clearStoredWiFiSettings();
  if (!saveStoredWiFiSettings()) {
    return false;
  }

  WiFi.disconnect();
  scheduleAccessPointRestart();
  return true;
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '\\': escaped += F("\\\\"); break;
      case '"':  escaped += F("\\\""); break;
      case '\n': escaped += F("\\n"); break;
      case '\r': escaped += F("\\r"); break;
      case '\t': escaped += F("\\t"); break;
      default:
        if ((uint8_t)c >= 0x20) {
          escaped += c;
        }
        break;
    }
  }

  return escaped;
}

uint8_t currentBrightnessValue() {
  if (!distanceBrightnessEnabled) {
    return isMode(MODE_CUSTOM) ? fixedCustomBrightness : AUTO_BRIGHTNESS;
  }

  if (!distanceState.hasReading) {
    return 0;
  }

  return distanceState.brightness;
}

#if 0
const char *distanceSensorStateText() {
  if (!distanceState.hasReading) {
    return "Chưa có dữ liệu";
  }

  return distanceState.lastReadOk ? "Ổn định" : "Mất tín hiệu";
}

const char *brightnessControlModeText() {
  return distanceBrightnessEnabled ? "Theo khoảng cách" : "Cố định";
}
#endif

const char *distanceSensorStateText() {
  if (!distanceState.hasReading) {
    return "Chưa có dữ liệu";
  }

  return distanceState.lastReadOk ? "Ổn định" : "Mất tín hiệu";
}

const char *brightnessControlModeText() {
  return distanceBrightnessEnabled ? "Theo khoảng cách" : "Cố định";
}

// Ambient light input can suppress LED output without destroying the current frame.
bool readAmbientLightBright() {
  return digitalRead(AMBIENT_LIGHT_PIN) == AMBIENT_LIGHT_BRIGHT_LEVEL;
}

bool evaluateLightOutputAllowed(bool isBright) {
  switch (lightGateMode) {
    case LIGHT_GATE_BRIGHT_ONLY:
      return isBright;

    case LIGHT_GATE_DARK_ONLY:
      return !isBright;

    case LIGHT_GATE_ALWAYS_ON:
    default:
      return true;
  }
}

void requestCurrentModeRefresh() {
  if (isMode(MODE_CUSTOM)) {
    customModeNeedsRefresh = true;
  } else {
    effectModeNeedsRefresh = true;
  }
}

void suppressStripOutput() {
  if (ambientLightState.outputSuppressed) {
    return;
  }

  uint32_t colors[LED_COUNT];
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    colors[i] = strip.getPixelColor(i);
  }

  strip.clear();
  strip.show();

  for (uint8_t i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, colors[i]);
  }

  ambientLightState.outputSuppressed = true;
}

void updateAmbientLightState() {
  bool wasAllowed = ambientLightState.outputAllowed;

  ambientLightState.isBright = readAmbientLightBright();
  ambientLightState.outputAllowed = evaluateLightOutputAllowed(ambientLightState.isBright);

  if (!ambientLightState.outputAllowed) {
    suppressStripOutput();
    return;
  }

  if (!wasAllowed) {
    requestCurrentModeRefresh();
  }

  ambientLightState.outputSuppressed = false;
}

// ====== Serial debug helpers ======
// Debug logging stays isolated here so it can be disabled with one compile-time flag.
bool isDebugSerialEnabled() {
#if DEBUG_SERIAL_ENABLED
  return true;
#else
  return false;
#endif
}

const char *modeName(RunMode mode) {
  return modeNameText(mode);
}

void debugLog(const __FlashStringHelper *message) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.println(message);
}

void debugLogBoot() {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.println();
  Serial.print(F("[BOOT] "));
  Serial.print(DEVICE_DISPLAY_NAME);
  Serial.println(F(" khởi động"));
  Serial.print(F("[BOOT] Chân nút: GPIO"));
  Serial.println(BUTTON_PIN);
  Serial.print(F("[BOOT] Siêu âm trig/echo: "));
  Serial.print(US_TRIG_PIN);
  Serial.print(F("/"));
  Serial.println(US_ECHO_PIN);
  Serial.print(F("[BOOT] Chân cảm biến sáng: "));
  Serial.println(AMBIENT_LIGHT_PIN);
  Serial.print(F("[BOOT] Quy tắc cảm biến sáng: "));
  Serial.println(lightGateModeText(lightGateMode));
  Serial.print(F("[BOOT] Mức sáng môi trường: "));
  Serial.println(lightLevelText(ambientLightState.isBright));
  Serial.print(F("[BOOT] Thời gian giữ nút ms: "));
  Serial.println(BUTTON_HOLD_TRIGGER_MS);
  Serial.print(F("[BOOT] Khoảng cách gần/xa cm: "));
  Serial.print(US_NEAR_CM, 1);
  Serial.print(F("/"));
  Serial.println(US_FAR_CM, 1);
  Serial.print(F("[BOOT] Kiểu điều khiển sáng: "));
  Serial.println(brightnessControlModeText());
  Serial.print(F("[WIFI] AP SSID: "));
  Serial.println(wifiSettings.apSsid);
  Serial.print(F("[WIFI] Mật khẩu AP: "));
  Serial.println(wifiSettings.apPassword);
  Serial.print(F("[WIFI] AP IP: "));
  Serial.println(WiFi.softAPIP());
  Serial.print(F("[WIFI] SSID WiFi nhà đã lưu: "));
  Serial.println(hasStationCredentials() ? wifiSettings.staSsid : "(chưa có)");
  Serial.print(F("[WIFI] Trạng thái WiFi nhà: "));
  Serial.println(stationConnectionStatusText());

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WIFI] IP WiFi nhà: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[WIFI] WiFi nhà chưa kết nối"));
  }

  Serial.println(F("[WEB] Mở http://192.168.4.1 khi dùng chế độ AP"));
}

void debugLogModeChange(RunMode mode) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.print(F("[MODE] -> "));
  Serial.println(modeName(mode));
}

void debugLogEffectChange(AutoEffect effect) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.print(F("[EFFECT] -> "));
  Serial.println(autoEffectNameText(effect));
}

void debugLogColorChange(uint8_t colorIndex) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.print(F("[COLOR] -> index "));
  Serial.print(colorIndex);
  Serial.print(F(" "));
  Serial.println(colorToHex(currentCustomColor()));
}

void debugLogButtonPressed() {
  debugLog(F("[BTN] pressed"));
}

void debugLogButtonReleased(uint32_t heldMs) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.print(F("[BTN] released, held ms = "));
  Serial.println(heldMs);
}

void debugLogLongHold(uint32_t heldMs) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  Serial.print(F("[BTN] hold reached, held ms = "));
  Serial.println(heldMs);
}

void debugLogSensor(float measuredCm, float filteredCm, uint8_t brightness) {
  if (!isDebugSerialEnabled()) {
    return;
  }

  uint32_t now = millis();
  if ((now - debugState.lastSensorLogMs) < DEBUG_SENSOR_LOG_MS) {
    return;
  }

  debugState.lastSensorLogMs = now;

  Serial.print(F("[SENSOR] measured="));
  Serial.print(measuredCm, 1);
  Serial.print(F("cm, filtered="));
  Serial.print(filteredCm, 1);
  Serial.print(F("cm, brightness="));
  Serial.println(brightness);
}

void debugLogSensorTimeout() {
  if (!isDebugSerialEnabled()) {
    return;
  }

  uint32_t now = millis();
  if ((now - debugState.lastSensorLogMs) < DEBUG_SENSOR_LOG_MS) {
    return;
  }

  debugState.lastSensorLogMs = now;
  Serial.println(F("[SENSOR] timeout or no echo"));
}

// ====== Background tasks ======
// These tasks keep the web UI, deferred WiFi work and sensor loop responsive.
void handleBackgroundTasks() {
  updateAmbientLightState();
  updateDistanceFromSensor();
  processPendingWiFiTasks();
  if (effectModeNeedsRefresh && isMode(MODE_AUTO_EFFECTS)) {
    effectModeNeedsRefresh = false;
    showStrip();
  }
  webServer.handleClient();
  yield();
}

bool shouldAbortWait(RunMode expectedMode, AutoEffect expectedAutoEffect) {
  if (!isMode(expectedMode)) {
    return true;
  }

  if (expectedMode == MODE_AUTO_EFFECTS && selectedAutoEffect != expectedAutoEffect) {
    return true;
  }

  return false;
}

// ====== Mode + button control ======
// GPIO0 short presses cycle colors; a long hold restores stored WiFi defaults.
bool readButtonRaw() {
  return digitalRead(BUTTON_PIN) == BUTTON_ACTIVE_LEVEL;
}

void resetCustomModeState() {
  if (!distanceState.hasReading) {
    distanceState.hasReading = false;
    distanceState.lastReadOk = false;
    distanceState.lastSampleMs = 0;
    distanceState.lastSuccessMs = 0;
    distanceState.lastBrightnessApplyMs = 0;
    distanceState.rawCm = -1.0f;
    distanceState.medianCm = US_FAR_CM;
    distanceState.filteredCm = US_FAR_CM;
    distanceState.candidateCm = US_FAR_CM;
    distanceState.candidateCount = 0;
    distanceState.brightness = CUSTOM_MAX_BRIGHTNESS;
    distanceState.targetBrightness = CUSTOM_MAX_BRIGHTNESS;
    distanceState.pendingBrightness = CUSTOM_MAX_BRIGHTNESS;
    distanceState.pendingBrightnessCount = 0;
    distanceFilterCount = 0;
    distanceFilterIndex = 0;
  }

  customModeNeedsRefresh = true;
}

void enterCustomMode() {
  currentMode = MODE_CUSTOM;
  strip.clear();
  resetCustomModeState();
  showStrip();
  debugLogModeChange(currentMode);
}

void exitCustomMode() {
  currentMode = MODE_AUTO_EFFECTS;
  strip.clear();
  strip.setBrightness(CUSTOM_MAX_BRIGHTNESS);
  showStrip();
  debugLogModeChange(currentMode);
}

void setRunMode(RunMode mode) {
  if (mode != MODE_CUSTOM) {
    if (!isMode(MODE_CUSTOM)) {
      enterCustomMode();
    }
    return;
  }

  if (!isMode(MODE_CUSTOM)) {
    enterCustomMode();
  }
}

void setAutoEffect(AutoEffect effect) {
  if (selectedAutoEffect != effect) {
    selectedAutoEffect = effect;
    debugLogEffectChange(effect);
  }
  setRunMode(MODE_AUTO_EFFECTS);
}

void setLightGateMode(LightGateMode mode) {
  if (lightGateMode == mode) {
    return;
  }

  lightGateMode = mode;
  updateAmbientLightState();
}

void setDistanceBrightnessEnabled(bool enabled) {
  if (distanceBrightnessEnabled == enabled) {
    return;
  }

  distanceBrightnessEnabled = enabled;
  customModeNeedsRefresh = true;
}

void setFixedCustomBrightness(uint8_t brightness) {
  if (fixedCustomBrightness == brightness) {
    return;
  }

  fixedCustomBrightness = brightness;
  customModeNeedsRefresh = true;
}

void setCustomColorIndex(uint8_t index) {
  if (index >= CUSTOM_PALETTE_COUNT) {
    return;
  }

  useCustomPickerColor = false;
  customColorIndex = index;
  customModeNeedsRefresh = true;
  debugLogColorChange(customColorIndex);
}

void setCustomPickerColor(const RgbColor &color) {
  customPickerColor = color;
  useCustomPickerColor = true;
  customModeNeedsRefresh = true;
}

void handleCustomShortPress() {
  if (!isMode(MODE_CUSTOM)) {
    return;
  }

  useCustomPickerColor = false;
  uint8_t nextIndex = customColorIndex + 1;
  if (nextIndex >= CUSTOM_PALETTE_COUNT) {
    nextIndex = 0;
  }
  setCustomColorIndex(nextIndex);
}

void updateButtonState() {
  uint32_t now = millis();
  bool rawPressed = readButtonRaw();

  if (rawPressed != buttonState.rawPressed) {
    buttonState.rawPressed = rawPressed;
    buttonState.lastRawChangeMs = now;
  }

  if ((now - buttonState.lastRawChangeMs) >= BUTTON_DEBOUNCE_MS &&
      rawPressed != buttonState.stablePressed) {
    buttonState.stablePressed = rawPressed;

    if (buttonState.stablePressed) {
      buttonState.pressStartMs = now;
      buttonState.longHoldHandled = false;
      debugLogButtonPressed();
    } else {
      uint32_t heldMs = now - buttonState.pressStartMs;
      debugLogButtonReleased(heldMs);
      if (!buttonState.longHoldHandled && heldMs < BUTTON_HOLD_TRIGGER_MS) {
        handleCustomShortPress();
      }
    }
  }

  if (buttonState.stablePressed && !buttonState.longHoldHandled) {
    uint32_t heldMs = now - buttonState.pressStartMs;
    if (heldMs >= BUTTON_HOLD_TRIGGER_MS) {
      buttonState.longHoldHandled = true;
      debugLogLongHold(heldMs);
      restoreWiFiDefaults();
    }
  }
}

bool controlledDelay(uint16_t waitMs, RunMode expectedMode, AutoEffect expectedAutoEffect = EFFECT_SEQUENCE) {
  uint32_t startMs = millis();

  while (true) {
    handleBackgroundTasks();
    updateButtonState();

    if (shouldAbortWait(expectedMode, expectedAutoEffect)) {
      return true;
    }

    uint16_t sliceMs = nextDelaySlice(startMs, waitMs);
    if (sliceMs == 0) {
      return false;
    }

    delay(sliceMs);
  }
}

// ====== Ultrasonic brightness control ======
// Distance sampling uses retry, filtering and hysteresis before brightness changes.
float clampDistance(float cm) {
  if (cm < US_NEAR_CM) {
    return US_NEAR_CM;
  }
  if (cm > US_FAR_CM) {
    return US_FAR_CM;
  }
  return cm;
}

bool isValidMeasuredDistance(float cm) {
  return cm > 0.0f && cm <= US_VALID_MAX_CM;
}

float readDistanceOnceCm() {
  digitalWrite(US_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(US_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG_PIN, LOW);

  unsigned long pulseUs = pulseIn(US_ECHO_PIN, HIGH, US_TIMEOUT_US);
  if (pulseUs == 0) {
    return -1.0f;
  }

  return (pulseUs * 0.0343f) * 0.5f;
}

float readDistanceCm() {
  float readings[US_READ_ATTEMPTS];
  uint8_t validCount = 0;

  for (uint8_t i = 0; i < US_READ_ATTEMPTS; i++) {
    float cm = readDistanceOnceCm();
    if (isValidMeasuredDistance(cm)) {
      readings[validCount++] = cm;
    }

    if ((i + 1) < US_READ_ATTEMPTS) {
      delayMicroseconds(US_RETRY_GAP_US);
    }
  }

  if (validCount == 0) {
    return -1.0f;
  }

  if (validCount == 1) {
    return readings[0];
  }

  if (validCount == 2) {
    return (readings[0] + readings[1]) * 0.5f;
  }

  if (readings[0] > readings[1]) {
    float t = readings[0];
    readings[0] = readings[1];
    readings[1] = t;
  }
  if (readings[1] > readings[2]) {
    float t = readings[1];
    readings[1] = readings[2];
    readings[2] = t;
  }
  if (readings[0] > readings[1]) {
    float t = readings[0];
    readings[0] = readings[1];
    readings[1] = t;
  }

  return readings[1];
}

uint8_t mapDistanceToBrightness(float cm) {
  float clamped = clampDistance(cm);
  float ratio = (US_FAR_CM - clamped) / (US_FAR_CM - US_NEAR_CM);
  float brightness = DISTANCE_MIN_BRIGHTNESS +
                     ratio * (CUSTOM_MAX_BRIGHTNESS - DISTANCE_MIN_BRIGHTNESS);
  return (uint8_t)(brightness + 0.5f);
}

float smoothDistance(float currentCm, float newCm, bool hasReading) {
  if (!hasReading) {
    return newCm;
  }

  float candidate = newCm;
  if (absDistanceDelta(currentCm, newCm) < US_DISTANCE_DEADBAND_CM) {
    candidate = currentCm;
  }

  return currentCm * (1.0f - US_FILTER_ALPHA) + candidate * US_FILTER_ALPHA;
}

uint8_t smoothBrightness(uint8_t currentBrightness, uint8_t targetBrightness) {
  int delta = (int)targetBrightness - (int)currentBrightness;
  if (delta == 0) {
    return currentBrightness;
  }

  int limitedDelta = delta;
  if (delta > 0 && delta > BRIGHTNESS_STEP_UP_MAX) {
    limitedDelta = BRIGHTNESS_STEP_UP_MAX;
  } else if (delta < 0 && -delta > BRIGHTNESS_STEP_DOWN_MAX) {
    limitedDelta = -BRIGHTNESS_STEP_DOWN_MAX;
  }

  int next = (int)currentBrightness + limitedDelta;
  if (next < CUSTOM_MIN_BRIGHTNESS) {
    next = CUSTOM_MIN_BRIGHTNESS;
  } else if (next > CUSTOM_MAX_BRIGHTNESS) {
    next = CUSTOM_MAX_BRIGHTNESS;
  }

  return (uint8_t)next;
}

bool updateAppliedBrightness(uint8_t newTargetBrightness, uint32_t now, bool hadReading) {
  distanceState.targetBrightness = newTargetBrightness;

  if (!hadReading) {
    distanceState.pendingBrightness = newTargetBrightness;
    distanceState.pendingBrightnessCount = 0;
    distanceState.lastBrightnessApplyMs = now;
    distanceState.brightness = newTargetBrightness;
    return true;
  }

  if (abs((int)newTargetBrightness - (int)distanceState.pendingBrightness) <= BRIGHTNESS_TARGET_TOLERANCE) {
    if (distanceState.pendingBrightnessCount < 255) {
      distanceState.pendingBrightnessCount++;
    }
  } else {
    distanceState.pendingBrightness = newTargetBrightness;
    distanceState.pendingBrightnessCount = 1;
  }

  if (distanceState.pendingBrightnessCount < BRIGHTNESS_TARGET_SAMPLES) {
    return false;
  }

  if ((now - distanceState.lastBrightnessApplyMs) < BRIGHTNESS_UPDATE_INTERVAL_MS) {
    return false;
  }

  distanceState.lastBrightnessApplyMs = now;
  distanceState.pendingBrightnessCount = 0;

  uint8_t nextBrightness = smoothBrightness(distanceState.brightness, distanceState.pendingBrightness);
  if (nextBrightness == distanceState.brightness) {
    return false;
  }

  distanceState.brightness = nextBrightness;
  return true;
}

void updateDistanceFromSensor() {
  uint32_t now = millis();
  if ((now - distanceState.lastSampleMs) < US_SAMPLE_INTERVAL_MS) {
    return;
  }

  distanceState.lastSampleMs = now;
  bool hadReading = distanceState.hasReading;

  float measuredCm = readDistanceCm();
  distanceState.rawCm = measuredCm;
  distanceState.lastReadOk = isValidMeasuredDistance(measuredCm);

  if (!distanceState.lastReadOk) {
    debugLogSensorTimeout();
    return;
  }

  if (distanceState.hasReading) {
    float delta = absDistanceDelta(distanceState.filteredCm, measuredCm);
    if (delta >= US_STEP_CONFIRM_CM) {
      if (absDistanceDelta(distanceState.candidateCm, measuredCm) <= US_CANDIDATE_TOLERANCE_CM) {
        if (distanceState.candidateCount < 255) {
          distanceState.candidateCount++;
        }
      } else {
        distanceState.candidateCm = measuredCm;
        distanceState.candidateCount = 1;
      }

      if (distanceState.candidateCount < US_STEP_CONFIRM_SAMPLES) {
        return;
      }
    } else {
      distanceState.candidateCm = measuredCm;
      distanceState.candidateCount = 0;
    }
  } else {
    distanceState.candidateCm = measuredCm;
    distanceState.candidateCount = 0;
  }

  distanceState.lastSuccessMs = now;
  pushDistanceSample(measuredCm);
  distanceState.medianCm = medianDistanceSample();
  distanceState.filteredCm = smoothDistance(
    distanceState.filteredCm,
    distanceState.medianCm,
    distanceState.hasReading
  );
  distanceState.hasReading = true;

  uint8_t previousBrightness = distanceState.brightness;
  bool brightnessApplied = updateAppliedBrightness(
    mapDistanceToBrightness(distanceState.filteredCm),
    now,
    hadReading
  );
  bool brightnessChanged = !hadReading ||
    abs((int)distanceState.brightness - (int)previousBrightness) >= US_BRIGHTNESS_DEADBAND;

  if (distanceBrightnessEnabled && brightnessApplied && brightnessChanged) {
    if (isMode(MODE_CUSTOM)) {
      customModeNeedsRefresh = true;
    }
    if (isMode(MODE_AUTO_EFFECTS)) {
      effectModeNeedsRefresh = true;
    }
  }

  debugLogSensor(measuredCm, distanceState.filteredCm, distanceState.brightness);
}

void renderCustomMode() {
  if (!customModeNeedsRefresh || !isMode(MODE_CUSTOM)) {
    return;
  }

  if (distanceBrightnessEnabled && !distanceState.hasReading) {
    clearStrip();
    customModeNeedsRefresh = false;
    return;
  }

  RgbColor color = currentCustomColor();

  strip.clear();
  strip.setBrightness(CUSTOM_MAX_BRIGHTNESS);
  fillStripRgb(color);
  showStrip();
  customModeNeedsRefresh = false;
}

void runCustomMode() {
  updateButtonState();
  renderCustomMode();
  controlledDelay(20, MODE_CUSTOM);
}

// ====== Effect 1: slow comet ======
// Animation helpers below are separated so each effect can be tuned independently.
bool fxCometSlow(uint32_t color, uint8_t tail, uint16_t waitMs, uint16_t cycles, AutoEffect expectedAutoEffect) {
  for (uint16_t c = 0; c < cycles; c++) {
    for (int i = 0; i < LED_COUNT; i++) {
      strip.clear();
      strip.setPixelColor(i, color);

      for (int t = 1; t <= tail; t++) {
        int idx = i - t;
        if (idx < 0) {
          idx += LED_COUNT;
        }

        uint8_t fade = 255 / (t + 1);
        strip.setPixelColor(idx, scaleColor(color, fade));
      }

      showStrip();
      if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
        return false;
      }
    }
  }

  return true;
}

// ====== Effect 2: slow theater chase ======
bool fxTheaterChaseSlow(uint32_t color, uint16_t waitMs, uint16_t cycles, AutoEffect expectedAutoEffect) {
  for (uint16_t j = 0; j < cycles; j++) {
    for (uint8_t q = 0; q < 3; q++) {
      strip.clear();

      for (int i = q; i < LED_COUNT; i += 3) {
        strip.setPixelColor(i, color);
      }

      showStrip();
      if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
        return false;
      }
    }
  }

  return true;
}

// ====== Effect 3: slow rainbow ======
bool fxRainbowSlow(uint16_t waitMs, uint16_t cycles, AutoEffect expectedAutoEffect) {
  for (uint16_t j = 0; j < 256 * cycles; j++) {
    for (int i = 0; i < LED_COUNT; i++) {
      strip.setPixelColor(i, wheel((i * 256 / LED_COUNT + j) & 255));
    }

    showStrip();
    if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
      return false;
    }
  }

  return true;
}

// ====== Effect 4: slow Larson scanner ======
bool fxLarsonSlow(uint32_t color, uint8_t tail, uint16_t waitMs, uint16_t cycles, AutoEffect expectedAutoEffect) {
  for (uint16_t c = 0; c < cycles; c++) {
    for (int i = 0; i < LED_COUNT; i++) {
      strip.clear();
      strip.setPixelColor(i, color);

      for (int t = 1; t <= tail; t++) {
        int left = i - t;
        int right = i + t;
        uint8_t fade = 255 / (t + 1);

        if (left >= 0) {
          strip.setPixelColor(left, scaleColor(color, fade));
        }
        if (right < LED_COUNT) {
          strip.setPixelColor(right, scaleColor(color, fade));
        }
      }

      showStrip();
      if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
        return false;
      }
    }

    for (int i = LED_COUNT - 1; i >= 0; i--) {
      strip.clear();
      strip.setPixelColor(i, color);

      for (int t = 1; t <= tail; t++) {
        int left = i - t;
        int right = i + t;
        uint8_t fade = 255 / (t + 1);

        if (left >= 0) {
          strip.setPixelColor(left, scaleColor(color, fade));
        }
        if (right < LED_COUNT) {
          strip.setPixelColor(right, scaleColor(color, fade));
        }
      }

      showStrip();
      if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
        return false;
      }
    }
  }

  return true;
}

// ====== Effect 5: dual chase ======
bool fxDualChaseSlow(uint32_t c1, uint32_t c2, uint16_t waitMs, uint16_t cycles, AutoEffect expectedAutoEffect) {
  for (uint16_t k = 0; k < cycles; k++) {
    for (int i = 0; i < LED_COUNT; i++) {
      strip.clear();

      int j = LED_COUNT - 1 - i;
      strip.setPixelColor(i, c1);
      strip.setPixelColor(j, c2);

      showStrip();
      if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
        return false;
      }
    }
  }

  return true;
}

// ====== Effect 6: comet + sparkle ======
bool fxCometSparkleSlow(uint32_t color, uint8_t tail, uint8_t sparkleCount, uint16_t waitMs, uint16_t cycles, AutoEffect expectedAutoEffect) {
  for (uint16_t c = 0; c < cycles; c++) {
    for (int i = 0; i < LED_COUNT; i++) {
      strip.clear();
      strip.setPixelColor(i, color);

      for (int t = 1; t <= tail; t++) {
        int idx = i - t;
        if (idx < 0) {
          idx += LED_COUNT;
        }

        strip.setPixelColor(idx, scaleColor(color, 255 / (t + 1)));
      }

      for (uint8_t s = 0; s < sparkleCount; s++) {
        int p = random(0, LED_COUNT);
        strip.setPixelColor(p, strip.Color(180, 180, 180));
      }

      showStrip();
      if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
        return false;
      }
    }
  }

  return true;
}

// ====== Effect 7: slow color wipe ======
bool fxColorWipeSlow(uint32_t color, uint16_t waitMs, AutoEffect expectedAutoEffect) {
  strip.clear();

  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color);
    showStrip();

    if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
      return false;
    }
  }

  return true;
}

// ====== Effect 8: slow random dot ======
bool fxRandomDotSlow(uint16_t waitMs, uint16_t steps, AutoEffect expectedAutoEffect) {
  strip.clear();

  for (uint16_t k = 0; k < steps; k++) {
    for (int i = 0; i < LED_COUNT; i++) {
      uint32_t color = strip.getPixelColor(i);
      strip.setPixelColor(i, scaleColor(color, 210));
    }

    int p = random(0, LED_COUNT);
    strip.setPixelColor(p, wheel(random(0, 255)));

    showStrip();
    if (controlledDelay(waitMs, MODE_AUTO_EFFECTS, expectedAutoEffect)) {
      return false;
    }
  }

  return true;
}

bool runAutoSequence() {
  const AutoEffect expected = EFFECT_SEQUENCE;
  strip.setBrightness(CUSTOM_MAX_BRIGHTNESS);

  if (!fxCometSlow(strip.Color(255, 0, 0), 5, SPEED_SLOW, 2, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxTheaterChaseSlow(strip.Color(0, 255, 0), SPEED_SLOW + 80, 10, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxRainbowSlow(40, 2, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxLarsonSlow(strip.Color(0, 0, 255), 4, SPEED_SLOW, 2, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxDualChaseSlow(strip.Color(255, 0, 255), strip.Color(0, 255, 255), SPEED_SLOW, 3, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxCometSparkleSlow(strip.Color(255, 120, 0), 5, 1, SPEED_SLOW, 2, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxColorWipeSlow(strip.Color(255, 255, 0), 150, expected)) {
    return false;
  }
  if (controlledDelay(500, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxColorWipeSlow(strip.Color(0, 0, 0), 120, expected)) {
    return false;
  }
  if (controlledDelay(300, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }

  if (!fxRandomDotSlow(SPEED_SLOW, 120, expected)) {
    return false;
  }

  if (controlledDelay(500, MODE_AUTO_EFFECTS, expected)) {
    return false;
  }
  return true;
}

bool runSelectedAutoEffect() {
  strip.setBrightness(CUSTOM_MAX_BRIGHTNESS);

  switch (selectedAutoEffect) {
    case EFFECT_COMET:
      return fxCometSlow(strip.Color(255, 0, 0), 5, SPEED_SLOW, 2, EFFECT_COMET);

    case EFFECT_THEATER:
      return fxTheaterChaseSlow(strip.Color(0, 255, 0), SPEED_SLOW + 80, 10, EFFECT_THEATER);

    case EFFECT_RAINBOW:
      return fxRainbowSlow(40, 2, EFFECT_RAINBOW);

    case EFFECT_LARSON:
      return fxLarsonSlow(strip.Color(0, 0, 255), 4, SPEED_SLOW, 2, EFFECT_LARSON);

    case EFFECT_DUAL:
      return fxDualChaseSlow(strip.Color(255, 0, 255), strip.Color(0, 255, 255), SPEED_SLOW, 3, EFFECT_DUAL);

    case EFFECT_COMET_SPARKLE:
      return fxCometSparkleSlow(strip.Color(255, 120, 0), 5, 1, SPEED_SLOW, 2, EFFECT_COMET_SPARKLE);

    case EFFECT_COLOR_WIPE:
      if (!fxColorWipeSlow(strip.Color(255, 255, 0), 150, EFFECT_COLOR_WIPE)) {
        return false;
      }
      return fxColorWipeSlow(strip.Color(0, 0, 0), 120, EFFECT_COLOR_WIPE);

    case EFFECT_RANDOM_DOT:
      return fxRandomDotSlow(SPEED_SLOW, 120, EFFECT_RANDOM_DOT);

    case EFFECT_SEQUENCE:
    default:
      return runAutoSequence();
  }
}

void runAutoEffects() {
  runSelectedAutoEffect();
}

// ====== Web server helpers ======
// HTTP handlers expose live state plus the onboard control and WiFi settings pages.
void sendBadRequest(const String &message) {
  webServer.send(400, "text/plain; charset=utf-8", message);
}

#if 0
String buildStatusJson() {
  String json;
  json.reserve(940);
  uint32_t now = millis();

  updateAmbientLightState();

  json += F("{\"mode\":\"");
  json += modeNameText(currentMode);
  json += F("\",\"autoEffect\":\"");
  json += autoEffectNameText(selectedAutoEffect);
  json += F("\",\"autoEffectIndex\":");
  json += (int)selectedAutoEffect;
  json += F(",\"colorIndex\":");
  json += useCustomPickerColor ? -1 : (int)customColorIndex;
  json += F(",\"usingCustomColor\":");
  json += useCustomPickerColor ? F("true") : F("false");
  json += F(",\"selectedColorHex\":\"");
  json += colorToHex(currentCustomColor());
  json += F("\",\"colorHex\":\"");
  json += colorToHex(currentCustomOutputColor());
  json += F("\",\"brightness\":");
  json += currentBrightnessValue();
  json += F(",\"distanceCm\":");

  if (distanceState.hasReading) {
    json += String(distanceState.filteredCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceRawCm\":");
  if (distanceState.lastReadOk) {
    json += String(distanceState.rawCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceMedianCm\":");
  if (distanceState.hasReading) {
    json += String(distanceState.medianCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceFilteredCm\":");
  if (distanceState.hasReading) {
    json += String(distanceState.filteredCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceState\":\"");
  json += distanceSensorStateText();
  json += F("\",\"distanceLastSampleAgeMs\":");
  if (distanceState.lastSampleMs > 0) {
    json += String(now - distanceState.lastSampleMs);
  } else {
    json += F("null");
  }

  json += F(",\"distanceLastSuccessAgeMs\":");
  if (distanceState.hasReading) {
    json += String(now - distanceState.lastSuccessMs);
  } else {
    json += F("null");
  }

  json += F(",\"distanceTargetBrightness\":");
  json += distanceState.targetBrightness;
  json += F(",\"distanceBrightness\":");
  json += distanceState.brightness;
  json += F(",\"distanceBrightnessEnabled\":");
  json += distanceBrightnessEnabled ? F("true") : F("false");
  json += F(",\"brightnessControlMode\":\"");
  json += brightnessControlModeText();
  json += F("\",\"fixedCustomBrightness\":");
  json += fixedCustomBrightness;
  json += F(",\"distanceNearCm\":");
  json += String(US_NEAR_CM, 1);
  json += F(",\"distanceFarCm\":");
  json += String(US_FAR_CM, 1);
  json += F(",\"lightSensorRaw\":");
  json += ambientLightState.isBright ? 1 : 0;
  json += F(",\"lightLevel\":\"");
  json += lightLevelText(ambientLightState.isBright);
  json += F("\",\"lightControlMode\":\"");
  json += lightGateModeText(lightGateMode);
  json += F("\",\"lightOutputAllowed\":");
  json += ambientLightState.outputAllowed ? F("true") : F("false");
  json += F(",\"apIp\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"staIp\":\"");
  json += stationIpText();
  json += F("\"}");

  return json;
}
#endif

String buildStatusJson() {
  String json;
  json.reserve(1180);
  uint32_t now = millis();

  updateAmbientLightState();

  json += F("{\"deviceName\":\"");
  json += jsonEscape(String(DEVICE_DISPLAY_NAME));
  json += F("\",\"mode\":\"");
  json += modeNameText(MODE_CUSTOM);
  json += F("\",\"colorIndex\":");
  json += useCustomPickerColor ? -1 : (int)customColorIndex;
  json += F(",\"usingCustomColor\":");
  json += useCustomPickerColor ? F("true") : F("false");
  json += F(",\"selectedColorHex\":\"");
  json += colorToHex(currentCustomColor());
  json += F("\",\"colorHex\":\"");
  json += colorToHex(currentCustomOutputColor());
  json += F("\",\"brightness\":");
  json += currentBrightnessValue();
  json += F(",\"distanceCm\":");

  if (distanceState.hasReading) {
    json += String(distanceState.filteredCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceRawCm\":");
  if (distanceState.lastReadOk) {
    json += String(distanceState.rawCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceMedianCm\":");
  if (distanceState.hasReading) {
    json += String(distanceState.medianCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceFilteredCm\":");
  if (distanceState.hasReading) {
    json += String(distanceState.filteredCm, 1);
  } else {
    json += F("null");
  }

  json += F(",\"distanceState\":\"");
  json += distanceSensorStateText();
  json += F("\",\"distanceLastSampleAgeMs\":");
  if (distanceState.lastSampleMs > 0) {
    json += String(now - distanceState.lastSampleMs);
  } else {
    json += F("null");
  }

  json += F(",\"distanceLastSuccessAgeMs\":");
  if (distanceState.hasReading) {
    json += String(now - distanceState.lastSuccessMs);
  } else {
    json += F("null");
  }

  json += F(",\"distanceTargetBrightness\":");
  json += distanceState.targetBrightness;
  json += F(",\"distanceBrightness\":");
  json += distanceState.brightness;
  json += F(",\"distanceBrightnessEnabled\":");
  json += distanceBrightnessEnabled ? F("true") : F("false");
  json += F(",\"brightnessControlMode\":\"");
  json += brightnessControlModeText();
  json += F("\",\"fixedCustomBrightness\":");
  json += fixedCustomBrightness;
  json += F(",\"distanceNearCm\":");
  json += String(US_NEAR_CM, 1);
  json += F(",\"distanceFarCm\":");
  json += String(US_FAR_CM, 1);
  json += F(",\"lightSensorRaw\":");
  json += ambientLightState.isBright ? 1 : 0;
  json += F(",\"lightLevel\":\"");
  json += lightLevelText(ambientLightState.isBright);
  json += F("\",\"lightControlMode\":\"");
  json += lightGateModeText(lightGateMode);
  json += F("\",\"lightOutputAllowed\":");
  json += ambientLightState.outputAllowed ? F("true") : F("false");
  json += F(",\"wifiStaConfigured\":");
  json += hasStationCredentials() ? F("true") : F("false");
  json += F(",\"wifiStaSsid\":\"");
  json += jsonEscape(String(wifiSettings.staSsid));
  json += F("\",\"wifiStaStatus\":\"");
  json += stationConnectionStatusText();
  json += F("\",\"apSsid\":\"");
  json += jsonEscape(String(wifiSettings.apSsid));
  json += F("\",\"apPassword\":\"");
  json += jsonEscape(String(wifiSettings.apPassword));
  json += F("\",\"apIp\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"staIp\":\"");
  json += stationIpText();
  json += F("\"}");

  return json;
}

void sendStatusJson() {
  webServer.send(200, "application/json; charset=utf-8", buildStatusJson());
}

#if 0
// The main page is self-contained so the board can serve it without external assets.
String buildControlPage() {
  String page;
  page.reserve(13200);

  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Đèn ngủ thông minh</title>"
    "<style>"
    "body{margin:0;padding:18px;font-family:Verdana,sans-serif;background:#0f172a;color:#e2e8f0;}"
    ".wrap{max-width:960px;margin:0 auto;display:grid;gap:14px;}"
    ".card{background:#111827;border:1px solid #334155;border-radius:16px;padding:16px;}"
    "h1,h2{margin:0 0 10px;}p{margin:0;line-height:1.5;color:#cbd5e1;}"
    ".row{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px;}"
    "button{border:0;border-radius:12px;padding:12px 14px;background:#2563eb;color:#fff;font-weight:700;cursor:pointer;}"
    "button.secondary{background:#475569;}"
    "button.swatch{width:42px;height:42px;padding:0;border:2px solid rgba(255,255,255,.22);border-radius:999px;}"
    "input{width:140px;border:1px solid #475569;border-radius:12px;padding:12px 14px;background:#0b1220;color:#e2e8f0;}"
    ".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-top:10px;}"
    ".chip{background:#0b1220;border-radius:12px;padding:12px;}"
    ".label{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.06em;}"
    ".value{margin-top:6px;font-size:16px;font-weight:700;}"
    ".preview{padding:8px 10px;border-radius:10px;}"
    ".small{font-size:13px;color:#94a3b8;margin-top:8px;}"
    "</style></head><body><div class='wrap'>"
  );

  page += F("<div class='card'><h1>Đèn ngủ thông minh</h1><p>");
  page += F("Tên WiFi AP: <b>");
  page += WIFI_AP_SSID;
  page += F("</b> | Mật khẩu AP: <b>");
  page += WIFI_AP_PASSWORD;
  page += F("</b><br>Mở <b>http://");
  page += WiFi.softAPIP().toString();
  page += F("</b> sau khi kết nối vào WiFi của mạch.</p>");

  if (WiFi.status() == WL_CONNECTED) {
    page += F("<p class='small'>IP WiFi nhà: ");
    page += WiFi.localIP().toString();
    page += F("</p>");
  }

  page += F("</div>");

  page += F(
    "<div class='card'><h2>Chế độ</h2><div class='row'>"
    "<button onclick='setMode(0)'>Hiệu ứng</button>"
    "<button class='secondary' onclick='setMode(1)'>Màu cố định</button>"
    "</div><p class='small'>Màu cố định = chỉ hiện một màu, độ sáng theo cảm biến hoặc theo mức cố định. Hiệu ứng = chạy các kiểu nháy và đuổi LED.</p></div>"
  );

  page += F("<div class='card'><h2>Hiệu ứng</h2><div class='row'>");
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    page += F("<button class='secondary' onclick='setEffect(");
    page += i;
    page += F(")'>");
    page += autoEffectButtonLabel((AutoEffect)i);
    page += F("</button>");
  }
  page += F("</div></div>");

  page += F("<div class='card'><h2>Màu đơn sắc</h2><div class='row'>");
  for (uint8_t i = 0; i < CUSTOM_PALETTE_COUNT; i++) {
    page += F("<button class='swatch' title='Màu ");
    page += i;
    page += F("' style='background:");
    page += colorToHex(CUSTOM_PALETTE[i]);
    page += F(";' onclick='setColor(");
    page += i;
    page += F(")'></button>");
  }
  page += F("</div><div class='row'>"
            "<button class='secondary' onclick='openCustomColorPicker()'>Màu tự chọn</button>"
            "<input id='customColorPicker' type='color' value='#FFFFFF' style='width:56px;padding:4px;border-radius:12px;' onchange='saveCustomColor(this.value)'>"
            "</div><p class='small'>Chế độ màu cố định chỉ dùng một màu. Khoảng cách có thể bật hoặc tắt để điều khiển độ sáng.</p></div>");

  page += F(
    "<div class='card'><h2>Cài đặt</h2>"
    "<div class='row'>"
    "<button onclick='setDistanceBrightness(1)'>Bật sáng theo khoảng cách</button>"
    "<button class='secondary' onclick='setDistanceBrightness(0)'>Tắt sáng theo khoảng cách</button>"
    "</div>"
    "<div class='row'>"
    "<input id='fixedBrightnessInput' type='number' min='1' max='255' value='255'>"
    "<button class='secondary' onclick='saveFixedBrightness()'>Lưu độ sáng cố định</button>"
    "</div>"
    "<p class='small'>Khoảng gần đang cố định là 8 cm, khoảng xa là 80 cm. Nếu tắt điều chỉnh sáng theo khoảng cách thì chế độ màu cố định sẽ dùng độ sáng cố định.</p></div>"
  );

  page += F("<div class='card'><h2>Cảm biến sáng tối (D1)</h2><div class='row'>");
  page += F("<button class='secondary' onclick='setLightMode(1)'>");
  page += lightGateModeButtonLabel(LIGHT_GATE_DARK_ONLY);
  page += F("</button>");
  page += F("<button class='secondary' onclick='setLightMode(0)'>");
  page += lightGateModeButtonLabel(LIGHT_GATE_BRIGHT_ONLY);
  page += F("</button>");
  page += F("<button class='secondary' onclick='setLightMode(2)'>");
  page += lightGateModeButtonLabel(LIGHT_GATE_ALWAYS_ON);
  page += F("</button>");
  page += F("</div><p class='small'>Logic chân D1: 0 = sáng, 1 = tối. Mặc định đèn sẽ bật khi trời tối.</p></div>");

  page += F(
    "<div class='card'><h2>Trạng thái</h2>"
    "<div class='status'>"
    "<div class='chip'><div class='label'>Chế độ</div><div id='mode' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Hiệu ứng</div><div id='effect' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Màu đang xuất</div><div id='color' class='value preview'>-</div></div>"
    "<div class='chip'><div class='label'>Màu đã chọn</div><div id='selectedcolor' class='value preview'>-</div></div>"
    "<div class='chip'><div class='label'>Độ sáng</div><div id='brightness' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Khoảng cách lọc</div><div id='distance' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Khoảng cách thô</div><div id='distanceRaw' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Khoảng cách median</div><div id='distanceMedian' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Siêu âm</div><div id='distanceState' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Lần đọc gần nhất</div><div id='distanceAge' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Lần đọc tốt gần nhất</div><div id='distanceGoodAge' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Kiểu điều khiển sáng</div><div id='brightnessctl' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Dải khoảng cách</div><div id='distancerange' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Độ sáng theo khoảng cách</div><div id='targetbrightness' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Độ sáng cố định</div><div id='fixedbrightness' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>D1 sáng tối</div><div id='light' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Quy tắc D1</div><div id='lightmode' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>LED đang bật</div><div id='output' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>IP AP</div><div id='apip' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>IP WiFi nhà</div><div id='staip' class='value'>-</div></div>"
    "</div></div>"
  );

  page += F(
    "<script>"
    "async function api(url){try{await fetch(url,{cache:'no-store'});await refreshStatus();}catch(e){console.log(e);}}"
    "function setMode(v){api('/api/mode?value='+v);}"
    "function setEffect(v){api('/api/effect?value='+v);}"
    "function setColor(v){api('/api/color?value='+v);}"
    "function openCustomColorPicker(){document.getElementById('customColorPicker').click();}"
    "function saveCustomColor(v){api('/api/custom-color?value='+encodeURIComponent(v));}"
    "function setLightMode(v){api('/api/light-mode?value='+v);}"
    "function setDistanceBrightness(v){api('/api/distance-brightness?value='+v);}"
    "function saveFixedBrightness(){const input=document.getElementById('fixedBrightnessInput');api('/api/fixed-brightness?value='+encodeURIComponent(input.value));}"
    "function applyPreview(id,color){"
    "const el=document.getElementById(id);"
    "el.style.backgroundColor=color;"
    "el.style.border='1px solid rgba(255,255,255,.18)';"
    "const r=parseInt(color.slice(1,3),16),g=parseInt(color.slice(3,5),16),b=parseInt(color.slice(5,7),16);"
    "const lum=(r*299+g*587+b*114)/1000;"
    "el.style.color=lum>160?'#111827':'#fff';"
    "}"
    "async function refreshStatus(){"
    "const res=await fetch('/api/status',{cache:'no-store'});"
    "const s=await res.json();"
    "document.getElementById('mode').textContent=s.mode;"
    "document.getElementById('effect').textContent=s.autoEffect;"
    "document.getElementById('color').textContent=s.usingCustomColor?s.colorHex+' (Tự chọn)':s.colorHex+' (#'+s.colorIndex+')';"
    "document.getElementById('selectedcolor').textContent=s.usingCustomColor?s.selectedColorHex+' (Tự chọn)':s.selectedColorHex+' (#'+s.colorIndex+')';"
    "applyPreview('color',s.colorHex);"
    "applyPreview('selectedcolor',s.selectedColorHex);"
    "document.getElementById('brightness').textContent=s.brightness;"
    "document.getElementById('distance').textContent=s.distanceFilteredCm===null?'Không có':s.distanceFilteredCm+' cm';"
    "document.getElementById('distanceRaw').textContent=s.distanceRawCm===null?'Không có':s.distanceRawCm+' cm';"
    "document.getElementById('distanceMedian').textContent=s.distanceMedianCm===null?'Không có':s.distanceMedianCm+' cm';"
    "document.getElementById('distanceState').textContent=s.distanceState;"
    "document.getElementById('distanceAge').textContent=s.distanceLastSampleAgeMs===null?'Không có':s.distanceLastSampleAgeMs+' ms';"
    "document.getElementById('distanceGoodAge').textContent=s.distanceLastSuccessAgeMs===null?'Không có':s.distanceLastSuccessAgeMs+' ms';"
    "document.getElementById('brightnessctl').textContent=s.brightnessControlMode;"
    "document.getElementById('distancerange').textContent=s.distanceNearCm+' - '+s.distanceFarCm+' cm';"
    "document.getElementById('targetbrightness').textContent=s.distanceTargetBrightness+' -> '+s.distanceBrightness;"
    "document.getElementById('fixedbrightness').textContent=s.fixedCustomBrightness;"
    "document.getElementById('light').textContent=s.lightLevel+' ('+s.lightSensorRaw+')';"
    "document.getElementById('lightmode').textContent=s.lightControlMode;"
    "document.getElementById('output').textContent=s.lightOutputAllowed?'Bật':'Tắt';"
    "document.getElementById('apip').textContent=s.apIp;"
    "document.getElementById('staip').textContent=s.staIp||'Không có';"
    "const fixedInput=document.getElementById('fixedBrightnessInput');"
    "if(document.activeElement!==fixedInput){fixedInput.value=s.fixedCustomBrightness;}"
    "const colorInput=document.getElementById('customColorPicker');"
    "if(document.activeElement!==colorInput){colorInput.value=s.selectedColorHex;}"
    "}"
    "refreshStatus();"
    "setInterval(refreshStatus,300);"
    "</script>"
  );

  page += F("</div></body></html>");
  return page;
}
#endif

String buildControlPage() {
  String page;
  page.reserve(12400);

  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:18px;font-family:Verdana,sans-serif;background:#0f172a;color:#e2e8f0;}"
    ".wrap{max-width:1080px;margin:0 auto;display:grid;gap:14px;}"
    ".card{background:#111827;border:1px solid #334155;border-radius:16px;padding:16px;}"
    "h1,h2{margin:0 0 10px;}p{margin:0;line-height:1.5;color:#cbd5e1;}"
    ".row{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px;align-items:center;}"
    "button,a.btn{border:0;border-radius:12px;padding:12px 14px;background:#2563eb;color:#fff;font-weight:700;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;justify-content:center;}"
    "button.secondary,a.btn.secondary{background:#475569;}"
    "button.swatch{width:42px;height:42px;padding:0;border:2px solid rgba(255,255,255,.22);border-radius:999px;}"
    "input{width:180px;border:1px solid #475569;border-radius:12px;padding:12px 14px;background:#0b1220;color:#e2e8f0;}"
    ".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-top:10px;}"
    ".chip{background:#0b1220;border-radius:12px;padding:12px;}"
    ".label{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.06em;}"
    ".value{margin-top:6px;font-size:16px;font-weight:700;}"
    ".preview{padding:8px 10px;border-radius:10px;}"
    ".small{font-size:13px;color:#94a3b8;margin-top:8px;}"
    "</style></head><body><div class='wrap'>"
  );

  page += F("<div class='card'><div class='row' style='justify-content:space-between;'>");
  page += F("<div><h1>");
  page += DEVICE_DISPLAY_NAME;
  page += F("</h1><p>Mô hình Trái Đất / Sao Thổ / Mặt Trăng từ vật liệu tái chế. Trời tối thì tự phát sáng, lại gần thì sáng hơn.</p></div>");
  page += F("<a class='btn secondary' href='/settings'>Cài đặt WiFi</a></div>");
  page += F("<p class='small'>WiFi AP: <b>");
  page += wifiSettings.apSsid;
  page += F("</b> | Mật khẩu AP: <b>");
  page += wifiSettings.apPassword;
  page += F("</b> | Mo <b>http://");
  page += WiFi.softAPIP().toString();
  page += F("</b> để điều khiển đèn. Nếu quên mật khẩu, giữ nút FLASH 10 giây để reset WiFi về mặc định.</p></div>");

  page += F("<div class='card'><h2>Màu hành tinh</h2><div class='row'>");
  for (uint8_t i = 0; i < CUSTOM_PALETTE_COUNT; i++) {
    page += F("<button class='swatch' title='Màu ");
    page += i;
    page += F("' style='background:");
    page += colorToHex(CUSTOM_PALETTE[i]);
    page += F(";' onclick='setColor(");
    page += i;
    page += F(")'></button>");
  }
  page += F("</div><div class='row'>"
            "<button class='secondary' onclick='openCustomColorPicker()'>Màu tùy chọn</button>"
            "<input id='customColorPicker' type='color' value='#FFFFFF' style='width:56px;padding:4px;border-radius:12px;' onchange='saveCustomColor(this.value)'>"
            "</div><p class='small'>Nhấn nút ngắn trên mạch để đổi màu. LED luôn chạy màu đơn sắc và không còn hiệu ứng nháy.</p></div>");

  page += F(
    "<div class='card'><h2>Độ sáng thông minh</h2>"
    "<div class='row'>"
    "<button onclick='setDistanceBrightness(1)'>Bật sáng theo khoảng cách</button>"
    "<button class='secondary' onclick='setDistanceBrightness(0)'>Dừng sáng theo khoảng cách</button>"
    "</div>"
    "<div class='row'>"
    "<input id='fixedBrightnessInput' type='number' min='1' max='255' value='255'>"
    "<button class='secondary' onclick='saveFixedBrightness()'>Lưu độ sáng cố định</button>"
    "</div>"
    "<p class='small'>Cảm biến siêu âm làm đèn sáng hơn khi bạn lại gần. Nếu tắt tính năng này thì đèn dùng mức sáng cố định.</p></div>"
  );

  page += F("<div class='card'><h2>Cảm biến trời tối (D1)</h2><div class='row'>");
  page += F("<button class='secondary' onclick='setLightMode(1)'>");
  page += lightGateModeButtonLabel(LIGHT_GATE_DARK_ONLY);
  page += F("</button>");
  page += F("<button class='secondary' onclick='setLightMode(0)'>");
  page += lightGateModeButtonLabel(LIGHT_GATE_BRIGHT_ONLY);
  page += F("</button>");
  page += F("<button class='secondary' onclick='setLightMode(2)'>");
  page += lightGateModeButtonLabel(LIGHT_GATE_ALWAYS_ON);
  page += F("</button>");
  page += F("</div><p class='small'>Mặc định: trời tối thì đèn bật. Logic D1 hiện tại là 0 = sáng, 1 = tối.</p></div>");

  page += F(
    "<div class='card'><h2>Trạng thái</h2>"
    "<div class='status'>"
    "<div class='chip'><div class='label'>Chế độ</div><div id='mode' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Màu đang xuất</div><div id='color' class='value preview'>-</div></div>"
    "<div class='chip'><div class='label'>Màu đã chọn</div><div id='selectedcolor' class='value preview'>-</div></div>"
    "<div class='chip'><div class='label'>Độ sáng</div><div id='brightness' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Khoảng cách lọc</div><div id='distance' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Siêu âm</div><div id='distanceState' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Kiểu điều khiển sáng</div><div id='brightnessctl' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Dải khoảng cách</div><div id='distancerange' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Độ sáng theo khoảng cách</div><div id='targetbrightness' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Độ sáng cố định</div><div id='fixedbrightness' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>D1 sáng tối</div><div id='light' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Quy tắc D1</div><div id='lightmode' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>LED đang bật</div><div id='output' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>WiFi nhà</div><div id='wifistat' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>SSID nhà đã lưu</div><div id='wifissid' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>IP AP</div><div id='apip' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>IP WiFi nhà</div><div id='staip' class='value'>-</div></div>"
    "</div></div>"
  );

  page += F(
    "<script>"
    "async function api(url){try{await fetch(url,{cache:'no-store'});await refreshStatus();}catch(e){console.log(e);}}"
    "function setColor(v){api('/api/color?value='+v);}"
    "function openCustomColorPicker(){document.getElementById('customColorPicker').click();}"
    "function saveCustomColor(v){api('/api/custom-color?value='+encodeURIComponent(v));}"
    "function setLightMode(v){api('/api/light-mode?value='+v);}"
    "function setDistanceBrightness(v){api('/api/distance-brightness?value='+v);}"
    "function saveFixedBrightness(){const input=document.getElementById('fixedBrightnessInput');api('/api/fixed-brightness?value='+encodeURIComponent(input.value));}"
    "function applyPreview(id,color){"
    "const el=document.getElementById(id);"
    "el.style.backgroundColor=color;"
    "el.style.border='1px solid rgba(255,255,255,.18)';"
    "const r=parseInt(color.slice(1,3),16),g=parseInt(color.slice(3,5),16),b=parseInt(color.slice(5,7),16);"
    "const lum=(r*299+g*587+b*114)/1000;"
    "el.style.color=lum>160?'#111827':'#fff';"
    "}"
    "async function refreshStatus(){"
    "const res=await fetch('/api/status',{cache:'no-store'});"
    "const s=await res.json();"
    "document.title=s.deviceName;"
    "document.getElementById('mode').textContent=s.mode;"
    "document.getElementById('color').textContent=s.usingCustomColor?s.colorHex+' (Tự chọn)':s.colorHex+' (#'+s.colorIndex+')';"
    "document.getElementById('selectedcolor').textContent=s.usingCustomColor?s.selectedColorHex+' (Tự chọn)':s.selectedColorHex+' (#'+s.colorIndex+')';"
    "applyPreview('color',s.colorHex);"
    "applyPreview('selectedcolor',s.selectedColorHex);"
    "document.getElementById('brightness').textContent=s.brightness;"
    "document.getElementById('distance').textContent=s.distanceFilteredCm===null?'Không có':s.distanceFilteredCm+' cm';"
    "document.getElementById('distanceState').textContent=s.distanceState;"
    "document.getElementById('brightnessctl').textContent=s.brightnessControlMode;"
    "document.getElementById('distancerange').textContent=s.distanceNearCm+' - '+s.distanceFarCm+' cm';"
    "document.getElementById('targetbrightness').textContent=s.distanceTargetBrightness+' -> '+s.distanceBrightness;"
    "document.getElementById('fixedbrightness').textContent=s.fixedCustomBrightness;"
    "document.getElementById('light').textContent=s.lightLevel+' ('+s.lightSensorRaw+')';"
    "document.getElementById('lightmode').textContent=s.lightControlMode;"
    "document.getElementById('output').textContent=s.lightOutputAllowed?'Bật':'Tắt';"
    "document.getElementById('wifistat').textContent=s.wifiStaStatus;"
    "document.getElementById('wifissid').textContent=s.wifiStaSsid||'Chưa cấu hình';"
    "document.getElementById('apip').textContent=s.apIp;"
    "document.getElementById('staip').textContent=s.staIp||'Không có';"
    "const fixedInput=document.getElementById('fixedBrightnessInput');"
    "if(document.activeElement!==fixedInput){fixedInput.value=s.fixedCustomBrightness;}"
    "const colorInput=document.getElementById('customColorPicker');"
    "if(document.activeElement!==colorInput){colorInput.value=s.selectedColorHex;}"
    "}"
    "refreshStatus();"
    "setInterval(refreshStatus,400);"
    "</script>"
  );

  page += F("</div></body></html>");
  return page;
}

// WiFi settings live on a separate page to keep the primary control UI simpler.
String buildSettingsPage() {
  String page;
  page.reserve(7600);

  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:18px;font-family:Verdana,sans-serif;background:#0f172a;color:#e2e8f0;}"
    ".wrap{max-width:860px;margin:0 auto;display:grid;gap:14px;}"
    ".card{background:#111827;border:1px solid #334155;border-radius:16px;padding:16px;}"
    "h1,h2{margin:0 0 10px;}p{margin:0;line-height:1.5;color:#cbd5e1;}"
    ".row{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px;align-items:center;}"
    "button,a.btn{border:0;border-radius:12px;padding:12px 14px;background:#2563eb;color:#fff;font-weight:700;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;justify-content:center;}"
    "button.secondary,a.btn.secondary{background:#475569;}"
    "input{width:240px;border:1px solid #475569;border-radius:12px;padding:12px 14px;background:#0b1220;color:#e2e8f0;}"
    ".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-top:10px;}"
    ".chip{background:#0b1220;border-radius:12px;padding:12px;}"
    ".label{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.06em;}"
    ".value{margin-top:6px;font-size:16px;font-weight:700;}"
    ".small{font-size:13px;color:#94a3b8;margin-top:8px;}"
    ".msg{min-height:20px;margin-top:10px;color:#93c5fd;}"
    "</style></head><body><div class='wrap'>"
  );

  page += F("<div class='card'><div class='row' style='justify-content:space-between;'>");
  page += F("<div><h1>");
  page += DEVICE_DISPLAY_NAME;
  page += F(" - Cài đặt WiFi</h1><p>Nhập WiFi nhà để board tự động kết nối thêm ở chế độ STA. WiFi AP của đèn vẫn giữ để điều khiển.</p></div>");
  page += F("<a class='btn secondary' href='/'>Quay lại trang chính</a></div></div>");

  page += F(
    "<div class='card'><h2>WiFi AP của đèn</h2>"
    "<div class='row'><input id='apSsidInput' placeholder='SSID AP'></div>"
    "<div class='row'><input id='apPasswordInput' type='password' placeholder='Mật khẩu AP'></div>"
    "<div class='row'>"
    "<button onclick='saveApSettings()'>Lưu WiFi AP</button>"
    "</div>"
    "<div class='status'>"
    "<div class='chip'><div class='label'>SSID AP đang dùng</div><div id='apssid' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Mật khẩu AP đang dùng</div><div id='appassword' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>IP AP</div><div id='apip' class='value'>-</div></div>"
    "</div><p class='small'>Nếu đổi SSID hoặc mật khẩu AP, kết nối hiện tại có thể bị ngắt. Nếu quên mật khẩu, giữ nút FLASH 10 giây để về lại mặc định.</p></div>"
  );

  page += F(
    "<div class='card'><h2>WiFi nhà</h2>"
    "<div class='row'><input id='wifiSsidInput' placeholder='SSID WiFi nhà'></div>"
    "<div class='row'><input id='wifiPasswordInput' type='password' placeholder='Mật khẩu WiFi nhà'></div>"
    "<div class='row'>"
    "<button onclick='saveWifiSettings()'>Lưu WiFi</button>"
    "<button class='secondary' onclick='clearWifiSettings()'>Xóa WiFi đã lưu</button>"
    "</div>"
    "<p class='small'>Nếu giữ nguyên SSID và để trống mật khẩu, board sẽ giữ lại mật khẩu đã lưu cũ. Sau khi lưu, board sẽ thử kết nối lại. Nhấn giữ FLASH 10 giây sẽ xóa WiFi nhà và reset AP về mặc định.</p>"
    "<div id='settingsMessage' class='msg'></div></div>"
  );

  page += F(
    "<div class='card'><h2>Trạng thái kết nối</h2>"
    "<div class='status'>"
    "<div class='chip'><div class='label'>SSID nhà đã lưu</div><div id='savedssid' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>Trạng thái</div><div id='wifistat' class='value'>-</div></div>"
    "<div class='chip'><div class='label'>IP WiFi nhà</div><div id='staip' class='value'>-</div></div>"
    "</div></div>"
  );

  page += F(
    "<script>"
    "async function refreshSettings(){"
    "const res=await fetch('/api/status',{cache:'no-store'});"
    "const s=await res.json();"
    "document.title=s.deviceName+' - Cài đặt WiFi';"
    "document.getElementById('apssid').textContent=s.apSsid;"
    "document.getElementById('appassword').textContent=s.apPassword;"
    "document.getElementById('apip').textContent=s.apIp;"
    "document.getElementById('savedssid').textContent=s.wifiStaSsid||'Chưa cấu hình';"
    "document.getElementById('wifistat').textContent=s.wifiStaStatus;"
    "document.getElementById('staip').textContent=s.staIp||'Không có';"
    "const apSsidInput=document.getElementById('apSsidInput');"
    "if(document.activeElement!==apSsidInput){apSsidInput.value=s.apSsid||'';}"
    "const apPasswordInput=document.getElementById('apPasswordInput');"
    "if(document.activeElement!==apPasswordInput){apPasswordInput.value=s.apPassword||'';}"
    "const ssidInput=document.getElementById('wifiSsidInput');"
    "if(document.activeElement!==ssidInput){ssidInput.value=s.wifiStaSsid||'';}"
    "}"
    "async function saveApSettings(){"
    "const ssid=document.getElementById('apSsidInput').value;"
    "const password=document.getElementById('apPasswordInput').value;"
    "const res=await fetch('/api/ap-settings/save?ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password),{cache:'no-store'});"
    "const text=await res.text();"
    "document.getElementById('settingsMessage').textContent=text;"
    "setTimeout(refreshSettings,800);"
    "}"
    "async function saveWifiSettings(){"
    "const ssid=document.getElementById('wifiSsidInput').value;"
    "const password=document.getElementById('wifiPasswordInput').value;"
    "const res=await fetch('/api/wifi-settings/save?ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password),{cache:'no-store'});"
    "const text=await res.text();"
    "document.getElementById('settingsMessage').textContent=text;"
    "document.getElementById('wifiPasswordInput').value='';"
    "setTimeout(refreshSettings,500);"
    "}"
    "async function clearWifiSettings(){"
    "const res=await fetch('/api/wifi-settings/clear',{cache:'no-store'});"
    "const text=await res.text();"
    "document.getElementById('settingsMessage').textContent=text;"
    "document.getElementById('wifiPasswordInput').value='';"
    "setTimeout(refreshSettings,500);"
    "}"
    "refreshSettings();"
    "setInterval(refreshSettings,1200);"
    "</script>"
  );

  page += F("</div></body></html>");
  return page;
}

void handleRoot() {
  webServer.send(200, "text/html; charset=utf-8", buildControlPage());
}

void handleSettingsPage() {
  webServer.send(200, "text/html; charset=utf-8", buildSettingsPage());
}

void handleApiMode() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing mode value"));
    return;
  }

  int value = webServer.arg("value").toInt();
  if (value == 0) {
    setRunMode(MODE_AUTO_EFFECTS);
  } else if (value == 1) {
    setRunMode(MODE_CUSTOM);
  } else {
    sendBadRequest(F("Invalid mode value"));
    return;
  }

  sendStatusJson();
}

void handleApiEffect() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing effect value"));
    return;
  }

  int value = webServer.arg("value").toInt();
  if (value < 0 || value >= EFFECT_COUNT) {
    sendBadRequest(F("Invalid effect value"));
    return;
  }

  setAutoEffect((AutoEffect)value);
  sendStatusJson();
}

void handleApiColor() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing color value"));
    return;
  }

  int value = webServer.arg("value").toInt();
  if (value < 0 || value >= CUSTOM_PALETTE_COUNT) {
    sendBadRequest(F("Invalid color value"));
    return;
  }

  setCustomColorIndex((uint8_t)value);
  setRunMode(MODE_CUSTOM);
  customModeNeedsRefresh = true;
  sendStatusJson();
}

void handleApiCustomColor() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing custom color value"));
    return;
  }

  RgbColor color = {0, 0, 0};
  if (!parseHexColor(webServer.arg("value"), color)) {
    sendBadRequest(F("Invalid custom color value"));
    return;
  }

  setCustomPickerColor(color);
  setRunMode(MODE_CUSTOM);
  customModeNeedsRefresh = true;
  sendStatusJson();
}

void handleApiLightMode() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing light mode value"));
    return;
  }

  int value = webServer.arg("value").toInt();
  if (value < 0 || value > LIGHT_GATE_ALWAYS_ON) {
    sendBadRequest(F("Invalid light mode value"));
    return;
  }

  setLightGateMode((LightGateMode)value);
  sendStatusJson();
}

void handleApiDistanceBrightness() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing distance brightness value"));
    return;
  }

  int value = webServer.arg("value").toInt();
  if (value != 0 && value != 1) {
    sendBadRequest(F("Invalid distance brightness value"));
    return;
  }

  setDistanceBrightnessEnabled(value == 1);
  sendStatusJson();
}

void handleApiFixedBrightness() {
  if (!webServer.hasArg("value")) {
    sendBadRequest(F("Missing fixed brightness value"));
    return;
  }

  int value = webServer.arg("value").toInt();
  if (value < CUSTOM_MIN_BRIGHTNESS || value > CUSTOM_MAX_BRIGHTNESS) {
    sendBadRequest(F("Invalid fixed brightness value"));
    return;
  }

  setFixedCustomBrightness((uint8_t)value);
  sendStatusJson();
}

void handleApiApSettingsSave() {
  if (!webServer.hasArg("ssid") || !webServer.hasArg("password")) {
    sendBadRequest(F("Thiếu SSID hoặc mật khẩu AP"));
    return;
  }

  String ssid = webServer.arg("ssid");
  String password = webServer.arg("password");
  ssid.trim();
  password.trim();

  if (ssid.length() == 0) {
    sendBadRequest(F("SSID AP không được để trống"));
    return;
  }

  if (ssid.length() > WIFI_STA_SSID_MAX_LEN) {
    sendBadRequest(F("SSID AP quá dài"));
    return;
  }

  if (password.length() < 8 || password.length() > WIFI_STA_PASSWORD_MAX_LEN) {
    sendBadRequest(F("Mật khẩu AP phải có từ 8 đến 64 ký tự"));
    return;
  }

  copySettingText(wifiSettings.apSsid, sizeof(wifiSettings.apSsid), ssid);
  copySettingText(wifiSettings.apPassword, sizeof(wifiSettings.apPassword), password);

  if (!saveStoredWiFiSettings()) {
    webServer.send(500, "text/plain; charset=utf-8", "Không thể lưu cấu hình WiFi AP");
    return;
  }

  scheduleAccessPointRestart(400);
  webServer.send(200, "text/plain; charset=utf-8", "Đã lưu WiFi AP. AP sẽ khởi động lại với cấu hình mới.");
}

void handleApiWiFiSettingsSave() {
  if (!webServer.hasArg("ssid")) {
    sendBadRequest(F("Thiếu SSID WiFi"));
    return;
  }

  String ssid = webServer.arg("ssid");
  String password = webServer.hasArg("password") ? webServer.arg("password") : String("");
  ssid.trim();
  password.trim();

  if (ssid.length() == 0) {
    sendBadRequest(F("SSID WiFi không được để trống"));
    return;
  }

  if (ssid.length() > WIFI_STA_SSID_MAX_LEN) {
    sendBadRequest(F("SSID WiFi quá dài"));
    return;
  }

  if (password.length() > WIFI_STA_PASSWORD_MAX_LEN) {
    sendBadRequest(F("Mật khẩu WiFi quá dài"));
    return;
  }

  bool keepExistingPassword =
    password.length() == 0 &&
    strcmp(wifiSettings.staSsid, ssid.c_str()) == 0 &&
    wifiSettings.staPassword[0] != '\0';

  if (!keepExistingPassword && password.length() > 0 && password.length() < 8) {
    sendBadRequest(F("Mật khẩu WiFi phải để trống hoặc có ít nhất 8 ký tự"));
    return;
  }

  copySettingText(wifiSettings.staSsid, sizeof(wifiSettings.staSsid), ssid);
  if (!keepExistingPassword) {
    copySettingText(wifiSettings.staPassword, sizeof(wifiSettings.staPassword), password);
  }

  if (!saveStoredWiFiSettings()) {
    webServer.send(500, "text/plain; charset=utf-8", "Không thể lưu cấu hình WiFi");
    return;
  }

  reconnectStationWiFi(false);
  webServer.send(200, "text/plain; charset=utf-8", "Đã lưu WiFi. Board đang thử kết nối lại.");
}

void handleApiWiFiSettingsClear() {
  wifiSettings.staSsid[0] = '\0';
  wifiSettings.staPassword[0] = '\0';

  if (!saveStoredWiFiSettings()) {
    webServer.send(500, "text/plain; charset=utf-8", "Không thể xóa cấu hình WiFi");
    return;
  }

  WiFi.disconnect();
  webServer.send(200, "text/plain; charset=utf-8", "Đã xóa WiFi nhà đã lưu.");
}

void handleNotFound() {
  webServer.send(404, "text/plain; charset=utf-8", "Không tìm thấy");
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/settings", HTTP_GET, handleSettingsPage);
  webServer.on("/api/status", HTTP_GET, sendStatusJson);
  webServer.on("/api/ap-settings/save", HTTP_GET, handleApiApSettingsSave);
  webServer.on("/api/color", HTTP_GET, handleApiColor);
  webServer.on("/api/custom-color", HTTP_GET, handleApiCustomColor);
  webServer.on("/api/light-mode", HTTP_GET, handleApiLightMode);
  webServer.on("/api/distance-brightness", HTTP_GET, handleApiDistanceBrightness);
  webServer.on("/api/fixed-brightness", HTTP_GET, handleApiFixedBrightness);
  webServer.on("/api/wifi-settings/save", HTTP_GET, handleApiWiFiSettingsSave);
  webServer.on("/api/wifi-settings/clear", HTTP_GET, handleApiWiFiSettingsClear);
  webServer.on("/favicon.ico", HTTP_GET, []() {
    webServer.send(204, "text/plain", "");
  });
  webServer.onNotFound(handleNotFound);
  webServer.begin();
}

// ====== Setup ======
// Initialization restores saved config first, then brings up hardware and the UI.
void setupButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  bool initialState = readButtonRaw();
  buttonState.rawPressed = initialState;
  buttonState.stablePressed = initialState;
  buttonState.longHoldHandled = false;
  buttonState.lastRawChangeMs = millis();
  buttonState.pressStartMs = millis();
}

void setupUltrasonic() {
  pinMode(US_TRIG_PIN, OUTPUT);
  pinMode(US_ECHO_PIN, INPUT);
  digitalWrite(US_TRIG_PIN, LOW);
}

void setupAmbientLightSensor() {
  pinMode(AMBIENT_LIGHT_PIN, INPUT);
  ambientLightState.isBright = readAmbientLightBright();
  ambientLightState.outputAllowed = evaluateLightOutputAllowed(ambientLightState.isBright);
  ambientLightState.outputSuppressed = false;
}

void setupStrip() {
  strip.updateType(ledColorOrderPixelType(ledColorOrder));
  strip.begin();
  strip.setBrightness(CUSTOM_MAX_BRIGHTNESS);
  strip.show();
}

void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  restartAccessPoint();

  if (!hasStationCredentials()) {
    return;
  }

  reconnectStationWiFi(true);
}

void setup() {
  if (isDebugSerialEnabled()) {
    Serial.begin(DEBUG_SERIAL_BAUD);
    delay(50);
  }

  loadStoredWiFiSettings();
  setupStrip();
  setupButton();
  setupUltrasonic();
  setupAmbientLightSensor();
  setupWiFi();
  setupWebServer();
  randomSeed(ESP.getCycleCount());
  debugLogBoot();
  debugLogModeChange(currentMode);
  debugLogColorChange(customColorIndex);
}

// ====== Main loop ======
void loop() {
  handleBackgroundTasks();
  runCustomMode();
}
