#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "display_bsp.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "font.h"
#include "qrcode.h"

// Expose internal handle for SHTC3 ESP-IDF calls
#include "driver/i2c_master.h"
#define private public
#define protected public
#include "i2c_bsp.h"
#include "PCF85063A.h"
#include "codec_bsp.h"
#undef private
#undef protected

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_sntp.h"
#include "mbedtls/md.h"
#include <sys/time.h>
#include <vector>

#include "sdcard_bsp.h"

#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

// ---------- HARDWARE PINOUT ----------
#define I2C_SDA            13
#define I2C_SCL            14
#define PA_CTRL            46
#define BUTTON_PIN         18  // Screen cycle button (Left)
#define RECORD_BUTTON_PIN  0   // Record toggle button (Right / BOOT)
#define BAT_ADC_PIN        4

#define SAMPLE_RATE        16000
#define CHANNELS           1
#define BITS_PER_SAMPLE    16
#define AUDIO_FRAME_SIZE   2048

static const uint8_t SHTC3_ADDR = 0x70;
static const int W = 400;
static const int H = 300;

// ---------- GLOBAL HARDWARE OBJECTS ----------
DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
GFXcanvas1 canvas(W, H);
U8G2_FOR_ADAFRUIT_GFX u8g2;

static I2cMasterBus I2cbus(I2C_SCL, I2C_SDA, 0);
CodecPort *codecport = NULL;
bool codecActive = false; // True while ES8311/ES7210 and I2S are open (Screen 2 only)
PCF85063A rtc;

CustomSDPort *sdcardPort = NULL;
WebServer server(80);
DNSServer dnsServer;

// ---------- STATE MACHINE & TIMING ----------
enum AppScreen {
  SCREEN_CLOCK = 0,
  SCREEN_MEETING,
  SCREEN_CONFIG
};

AppScreen currentScreen = SCREEN_CLOCK;
AppScreen previousScreen = SCREEN_CLOCK;

unsigned long lastButtonPress = 0;
const unsigned long debounceTime = 250;
bool lastButtonState = HIGH;

unsigned long lastRecButtonPress = 0;
bool lastRecButtonState = HIGH;

int lastDrawnMinute = -1;
unsigned long lastWeatherSync = 0;
const unsigned long WEATHER_SYNC_INTERVAL = 900000UL; // 15 mins

// ---------- BUFFERS & CONFIGS ----------
char config_buffer[2048] = {0};
char wifi_ssid[64] = "";
char wifi_pass[64] = "";
char mac_server[128] = "http://192.168.1.100:5000";
char city_name[32] = "Bogota";
char latitude[16] = "4.7110";
char longitude[16] = "-74.0721";
int gmt_offset = -5;

// Network & Localization State Management
char location_override[16] = "false";
char current_network[64] = "";
char last_synced_city[32] = "";
int lastWeatherSyncMinute = -1;
bool timeSyncedToday = false;
static volatile bool ntpPacketReceived = false;
bool clockSyncedSinceBoot = false; // Set only by a confirmed NTP sync; esp_sntp_get_sync_status() resets after each read
int rtc_gmt_offset = 0;            // UTC offset the RTC's local time was last written with (and the active TZ)

// Network State Cache (Displayed while Wi-Fi radio is powered off)
String lastKnownIP = "No Wi-Fi";
bool lastNetworkAvailable = false;
bool lastWeatherSuccess = false;

// ---------- SENSOR & BATTERY VALUES ----------
float temp_shtc3 = 0.0, hum_shtc3 = 0.0;
float v_bat = 0.0;

// ---------- WEATHER METRICS ----------
int cur_temp = 0, cur_humidity = 0, feels_like = 0, weather_code = 0;
int max_temp = 0, min_temp = 0;
int rain_prob_max = 0;
float precip_sum = 0.0;
int rain_start_hour = -1, rain_stop_hour = -1;
bool will_rain = false;
char weather_label[24] = "Fair";

// ---------- DEVICE IDENTITY & AUTH ----------
char device_id[64] = "";
char key_id[64] = "";
char hmac_secret[128] = "";

// ---------- UPLOAD OUTCOMES ----------
enum UploadResult {
  UPLOAD_OK = 0,        // 200/202, or 409 duplicate: the server has the file
  UPLOAD_RETRY_LATER,   // Network error, timeout, 408, 429, 5xx, 507, hash mismatch
  UPLOAD_CLOCK_SKEW,    // 401 clock_skew: resync NTP, then retry
  UPLOAD_REJECTED       // Other 4xx or recording_conflict: keep the file, never retry
};

// ---------- AUDIO RECORDING STATE & SD WATCHDOG ----------
uint8_t frame_buffer[AUDIO_FRAME_SIZE];
bool isRecording = false;
bool sdCardError = false; // Flagged if 0x107 or write failure occurs
FILE *currentMeetingFile = NULL;
char currentMeetingFileName[128] = ""; // ~85 chars: /sdcard/meetings/ + timestamp + UUID + clock flag + status
uint32_t totalRecordedBytes = 0;
unsigned long recordingStartTime = 0;
unsigned long lastTimerScreenUpdate = 0;

// ---------- CALENDAR DATA ----------
const int daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
bool isLeapYear(int yr) {
  return ((yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0));
}

// Forward Declarations
void loadSDClockConfig();
void saveSDClockConfig();
void saveWiFiTxtConfig();
bool readWiFiDatabase(JsonDocument &doc);
bool saveWiFiDatabase(JsonDocument &doc);
bool isInWiFiDatabase(const char* ssid);
void appendWiFiDatabase(const char* essid, const char* pass, const char* city, float lat, float lon, int gmt);
void applyLocalizationStrategy();
bool queryWPSCoordinates(float &lat, float &lon);
bool queryIPCoordinates(float &lat, float &lon);
bool obtainCoordinates(float &lat, float &lon);
bool queryBigDataCloudCity();
bool syncNTP();
void updateOpenMeteo();
void processFailedUploads();
void resyncClockIfOffsetChanged();
void writeWavHeader(uint8_t* buffer, uint32_t dataSize);

// ================= CRYPTOGRAPHY & ID GENERATION =================
String generateUUIDv4() {
  uint8_t uuid[16];
  esp_fill_random(uuid, 16);
  uuid[6] = (uuid[6] & 0x0F) | 0x40; // Version 4
  uuid[8] = (uuid[8] & 0x3F) | 0x80; // Variant 1
  char buf[37];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
           uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
  return String(buf);
}

String computeFileSHA256(const char* filepath) {
  FILE *f = fopen(filepath, "rb");
  if (!f) return "";
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  
  // Heap allocation prevents Stack Overflow on large hashing
  uint8_t *buf = (uint8_t*)malloc(AUDIO_FRAME_SIZE);
  if (buf) {
    size_t bytesRead;
    while ((bytesRead = fread(buf, 1, AUDIO_FRAME_SIZE, f)) > 0) {
      mbedtls_md_update(&ctx, buf, bytesRead);
      delay(2); // Keep watchdog alive during big file hashes
    }
    free(buf);
  }
  
  fclose(f);
  uint8_t digest[32];
  mbedtls_md_finish(&ctx, digest);
  mbedtls_md_free(&ctx);
  char hexBuf[65];
  for (int i = 0; i < 32; i++) sprintf(&hexBuf[i * 2], "%02x", digest[i]);
  return String(hexBuf);
}

String computeHMACSHA256(const String& payload, const char* secret) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret, strlen(secret));
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)payload.c_str(), payload.length());
  uint8_t hmac[32];
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  char hexBuf[65];
  for (int i = 0; i < 32; i++) sprintf(&hexBuf[i * 2], "%02x", hmac[i]);
  return String(hexBuf);
}

// ================= DYNAMIC CPU POWER SCALING =================
void setLowPowerClock(bool lowPower) {
  if (lowPower) {
    // 80 MHz preserves bus stability while cutting base CPU current by ~60%
    setCpuFrequencyMhz(80);
  } else {
    // Boost clock for Wi-Fi, DNS/Web server, and audio capture
    setCpuFrequencyMhz(160);
  }
}

// ================= WI-FI POSITIONING SYSTEM (WPS) & GEOLOCATION =================
bool queryWPSCoordinates(float &lat, float &lon) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int n = WiFi.scanNetworks();
  if (n <= 0) return false;

  // Query Wi-Fi Positioning System using detected BSSID
  for (int i = 0; i < n && i < 3; i++) {
    String bssidStr = WiFi.BSSIDstr(i);
    HTTPClient http;
    String url = "https://api.mylnikov.org/geolocation/wifi?v=1.1&data=open&bssid=" + bssidStr;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
        if (doc["result"] == 200 && !doc["data"]["lat"].isNull() && !doc["data"]["lon"].isNull()) {
          lat = doc["data"]["lat"].as<float>();
          lon = doc["data"]["lon"].as<float>();
          http.end();
          return true;
        }
      }
    }
    http.end();
  }
  return false;
}

bool queryIPCoordinates(float &lat, float &lon) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin("http://ip-api.com/json/");
  int httpCode = http.GET();
  if (httpCode == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      if (!doc["lat"].isNull() && !doc["lon"].isNull()) {
        lat = doc["lat"].as<float>();
        lon = doc["lon"].as<float>();
        http.end();
        return true;
      }
    }
  }
  http.end();
  return false;
}

bool obtainCoordinates(float &lat, float &lon) {
  // 1. Attempt location via WPS (Wi-Fi Positioning System) first
  if (queryWPSCoordinates(lat, lon)) {
    return true;
  }
  // 2. If WPS fails, query ip-api.com to fetch network location coordinates
  if (queryIPCoordinates(lat, lon)) {
    return true;
  }
  return false;
}

bool queryBigDataCloudCity() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String("https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=") + latitude +
               "&longitude=" + longitude + "&localityLanguage=en";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      const char* city = doc["city"];
      const char* locality = doc["locality"];
      if (city && strlen(city) > 0) {
        strncpy(city_name, city, sizeof(city_name) - 1);
        city_name[sizeof(city_name) - 1] = '\0';
      } else if (locality && strlen(locality) > 0) {
        strncpy(city_name, locality, sizeof(city_name) - 1);
        city_name[sizeof(city_name) - 1] = '\0';
      }
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

// ================= DATABASE PERSISTENCE =================
bool readWiFiDatabase(JsonDocument &doc) {
  FILE *f = fopen("/sdcard/wifi_database.json", "r");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return false;
  }
  char *buf = (char *)malloc(size + 1);
  if (!buf) {
    fclose(f);
    return false;
  }
  fread(buf, 1, size, f);
  buf[size] = '\0';
  fclose(f);
  DeserializationError err = deserializeJson(doc, buf);
  free(buf);
  return (err == DeserializationError::Ok);
}

bool saveWiFiDatabase(JsonDocument &doc) {
  FILE *f = fopen("/sdcard/wifi_database.json", "w");
  if (!f) {
    return false;
  }
  
  String jsonStr;
  serializeJsonPretty(doc, jsonStr);
  
  size_t written = fwrite(jsonStr.c_str(), 1, jsonStr.length(), f);
  fclose(f);
  
  return (written == jsonStr.length());
}

bool isInWiFiDatabase(const char* ssid) {
  if (!ssid || strlen(ssid) == 0) return false;
  JsonDocument doc;
  if (!readWiFiDatabase(doc) || !doc.is<JsonArray>()) return false;
  for (JsonObject entry : doc.as<JsonArray>()) {
    if (entry["essid"] == ssid) {
      return true;
    }
  }
  return false;
}

void appendWiFiDatabase(const char* essid, const char* pass, const char* city, float lat, float lon, int gmt) {
  if (!essid || strlen(essid) == 0) return;
  JsonDocument doc;
  bool loaded = readWiFiDatabase(doc);
  JsonArray arr;
  if (loaded && doc.is<JsonArray>()) {
    arr = doc.as<JsonArray>();
  } else {
    doc.clear();
    arr = doc.to<JsonArray>();
  }

  bool exists = false;
  for (JsonObject obj : arr) {
    if (obj["essid"] == essid) {
      obj["password"] = pass ? pass : "";
      obj["city"] = city ? city : "";
      obj["latitude"] = lat;
      obj["longitude"] = lon;
      obj["gmt"] = gmt;
      exists = true;
      break;
    }
  }
  if (!exists) {
    JsonObject entry = arr.add<JsonObject>();
    entry["essid"] = essid;
    entry["password"] = pass ? pass : "";
    entry["city"] = city ? city : "";
    entry["latitude"] = lat;
    entry["longitude"] = lon;
    entry["gmt"] = gmt;
  }
  saveWiFiDatabase(doc);
}

void saveWiFiTxtConfig() {
  FILE *f = fopen("/sdcard/config_wifi.txt", "w");
  if (f) {
    fprintf(f, "wifi_ssid=%s\nwifi_pass=%s\n", wifi_ssid, wifi_pass);
    fclose(f);
  } else {
    sdCardError = true;
  }
}

void handleFirstTimeConnection(const char* ssid, const char* pass) {
  // 1. Get coordinates via WPS (Wi-Fi Positioning System) first; fallback to IP-API if WPS fails
  float lat = (float)atof(latitude);
  float lon = (float)atof(longitude);
  if (obtainCoordinates(lat, lon)) {
    dtostrf(lat, 6, 4, latitude);
    dtostrf(lon, 6, 4, longitude);
  }

  // 2. Use coordinates to query the weather and calculate GMT offset from timezone_abbreviation
  updateOpenMeteo();

  // 3. Sync the time with NTP using the resolved timezone
  syncNTP();

  // 4. Use BigDataCloud Reverse Geocoding to City API to get the city name
  queryBigDataCloudCity();
  strncpy(last_synced_city, city_name, sizeof(last_synced_city) - 1);

  // 5. Save coordinates, city name, GMT offset, essid and password in the .txt files and json database
  strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
  wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
  strncpy(wifi_pass, pass, sizeof(wifi_pass) - 1);
  wifi_pass[sizeof(wifi_pass) - 1] = '\0';
  saveWiFiTxtConfig();
  saveSDClockConfig();

  appendWiFiDatabase(ssid, pass, city_name, (float)atof(latitude), (float)atof(longitude), gmt_offset);
}

// ================= POWER-SAVING WI-FI MANAGERS =================
bool ensureWiFiConnected(int maxWaitSec = 6) {
  // The radio stays off for the whole recording; uploads start only after isRecording is cleared
  if (isRecording) return false;

  setLowPowerClock(false);

  if (WiFi.status() == WL_CONNECTED) {
    lastKnownIP = WiFi.localIP().toString();
    lastNetworkAvailable = true;
    return true;
  }

  WiFi.mode(WIFI_STA);

  // 1. Primary Attempt: Attempt to connect directly to the default network specified in .txt
  bool connected = false;
  if (strlen(wifi_ssid) > 0) {
    WiFi.begin(wifi_ssid, wifi_pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < (maxWaitSec * 2)) {
      delay(500);
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    }
  }

  // 2. JSON Database Fallback: If the primary connection fails
  if (!connected) {
    WiFi.disconnect(true);
    delay(200);
    int n = WiFi.scanNetworks();
    if (n > 0) {
      JsonDocument dbDoc;
      if (readWiFiDatabase(dbDoc) && dbDoc.is<JsonArray>()) {
        JsonArray arr = dbDoc.as<JsonArray>();
        for (int i = 0; i < n && !connected; i++) {
          String scannedSSID = WiFi.SSID(i);
          for (JsonObject entry : arr) {
            const char* db_essid = entry["essid"];
            const char* db_pass = entry["password"];
            if (db_essid && strlen(db_essid) > 0 && scannedSSID == db_essid) {
              WiFi.begin(db_essid, db_pass ? db_pass : "");
              int waitCount = 0;
              while (WiFi.status() != WL_CONNECTED && waitCount < (maxWaitSec * 2)) {
                delay(500);
                waitCount++;
              }
              if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                // Match found: connect and update .txt file to set SSID as new default
                strncpy(wifi_ssid, db_essid, sizeof(wifi_ssid) - 1);
                wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
                strncpy(wifi_pass, db_pass ? db_pass : "", sizeof(wifi_pass) - 1);
                wifi_pass[sizeof(wifi_pass) - 1] = '\0';
                saveWiFiTxtConfig();
                break;
              }
            }
          }
        }
      }
    }
  }

  if (connected) {
    lastKnownIP = WiFi.localIP().toString();
    lastNetworkAvailable = true;

    // First-Time Connection Setup
    String activeSSID = WiFi.SSID();
    if (!isInWiFiDatabase(activeSSID.c_str()) && currentScreen != SCREEN_MEETING) {
      handleFirstTimeConnection(activeSSID.c_str(), wifi_pass);
    }

    // Screen 2 Behavior: Suppress all API calls for time, weather, and localization in Screen 2
    if (currentScreen != SCREEN_MEETING) {
      bool netChanged = false;
      if (activeSSID.length() > 0 && strcmp(current_network, activeSSID.c_str()) != 0) {
        netChanged = true;
        strncpy(current_network, activeSSID.c_str(), sizeof(current_network) - 1);
      }

      applyLocalizationStrategy();

      // City Change Handling: track the city; the time resync below is driven by the UTC offset
      if (strlen(last_synced_city) > 0 && strcmp(last_synced_city, city_name) != 0) {
        strncpy(last_synced_city, city_name, sizeof(last_synced_city) - 1);
      }

      // Weather Data trigger: Whenever the active network changes
      if (netChanged) {
        updateOpenMeteo();
      }

      // A new city or an Open-Meteo correction may have changed the offset: sync time immediately
      resyncClockIfOffsetChanged();
    } else {
      if (activeSSID.length() > 0) {
        strncpy(current_network, activeSSID.c_str(), sizeof(current_network) - 1);
      }
    }

    // Every time a connection is established, config_wifi.txt and config_clock.txt are created if they don't exist and overwritten
    saveWiFiTxtConfig();
    saveSDClockConfig();

    return true;
  } else {
    lastKnownIP = "No Wi-Fi";
    lastNetworkAvailable = false;
    return false;
  }
}

void disableWiFi() {
  // Never turn off Wi-Fi if the user is in Screen 3 (AP configuration mode)
  if (currentScreen != SCREEN_CONFIG) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

// ================= BATTERY LOGIC =================
void readBattery() {
  if (isRecording) return; // Keep the last value; no ADC sampling while recording
  int raw = analogRead(BAT_ADC_PIN);
  float v_adc = (raw / 4095.0f) * 3.3f;
  v_bat = v_adc * 3.0f * 1.079f;
}

int getBatteryPercentage(float vbat) {
  if (vbat >= 4.15f) return 100;
  if (vbat <= 3.40f) return 0;
  int pct = (int)roundf(((vbat - 3.40f) / (4.15f - 3.40f)) * 100.0f);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

int getBatteryBars(float vbat) {
  if (vbat >= 4.00) return 5;
  if (vbat >= 3.90) return 4;
  if (vbat >= 3.80) return 3;
  if (vbat >= 3.65) return 2;
  if (vbat >= 3.50) return 1;
  return 0;
}

// ================= SHTC3 DRIVER VIA I2CBUS =================
static i2c_master_dev_handle_t shtc3_dev_handle = NULL;

uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

bool shtc3_attach() {
  if (shtc3_dev_handle != NULL) return true;
  i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = SHTC3_ADDR,
    .scl_speed_hz = 100000,
  };
  return i2c_master_bus_add_device(I2cbus.user_i2c_handle, &dev_cfg, &shtc3_dev_handle) == ESP_OK;
}

void shtc3_sleep() {
  if (!shtc3_attach()) return;
  uint8_t cmd_sleep[2] = {0xB0, 0x98};
  i2c_master_transmit(shtc3_dev_handle, cmd_sleep, 2, 50);
}

bool shtc3_read_i2cbus(float &tempC, float &rh) {
  if (isRecording) return false; // Sensors stay idle while recording
  if (!shtc3_attach()) return false;

  uint8_t cmd_wake[2] = {0x35, 0x17};
  if (i2c_master_transmit(shtc3_dev_handle, cmd_wake, 2, 50) != ESP_OK) return false;
  delay(1);

  // From here on the sensor is awake: every exit path sends it back to sleep
  bool ok = false;
  uint8_t cmd_meas[2] = {0x78, 0x66};
  uint8_t d[6] = {0};
  if (i2c_master_transmit(shtc3_dev_handle, cmd_meas, 2, 50) == ESP_OK) {
    delay(20);
    ok = (i2c_master_receive(shtc3_dev_handle, d, 6, 50) == ESP_OK) &&
         crc8(&d[0], 2) == d[2] && crc8(&d[3], 2) == d[5];
  }
  shtc3_sleep();
  if (!ok) return false;

  uint16_t tRaw  = (uint16_t)d[0] << 8 | d[1];
  uint16_t rhRaw = (uint16_t)d[3] << 8 | d[4];

  tempC = -45.0f + 175.0f * (float)tRaw / 65535.0f;
  rh    = 100.0f * (float)rhRaw / 65535.0f;

  return true;
}

// ================= DISPLAY PUSH =================
void pushCanvasToRLCD(bool invert = false) {
  uint8_t *buf = canvas.getBuffer();
  const int bytesPerRow = (W + 7) / 8;
  RlcdPort.RLCD_ColorClear(ColorWhite);

  for (int y = 0; y < H; y++) {
    uint8_t *row = buf + y * bytesPerRow;
    for (int bx = 0; bx < bytesPerRow; bx++) {
      uint8_t v = row[bx];
      if (invert) v ^= 0xFF;
      int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        int x = x0 + bit;
        if (x >= W) break;
        if ((v & (0x80 >> bit)) != 0) {
          RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)y, ColorBlack);
        }
      }
    }
  }
  RlcdPort.RLCD_Display();
}

// ================= SD CARD CONFIGS & RETENTION =================
void loadSDClockConfig() {
  memset(config_buffer, 0, sizeof(config_buffer));
  if (sdcardPort && sdcardPort->SDPort_ReadFile("/sdcard/config_clock.txt", (uint8_t *)config_buffer, NULL) == 0) {
    char *line = strtok(config_buffer, "\r\n");
    while (line != NULL) {
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "city") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(city_name, val, sizeof(city_name) - 1);
            city_name[sizeof(city_name) - 1] = '\0';
          }
        } else if (strcmp(key, "latitude") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(latitude, val, sizeof(latitude) - 1);
            latitude[sizeof(latitude) - 1] = '\0';
          }
        } else if (strcmp(key, "longitude") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(longitude, val, sizeof(longitude) - 1);
            longitude[sizeof(longitude) - 1] = '\0';
          }
        } else if (strcmp(key, "gmt") == 0) {
          if (val && strlen(val) > 0) {
            gmt_offset = atoi(val);
          }
        } else if (strcmp(key, "override") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(location_override, val, sizeof(location_override) - 1);
            location_override[sizeof(location_override) - 1] = '\0';
          }
        }
      }
      line = strtok(NULL, "\r\n");
    }
  }
}

void saveSDClockConfig() {
  FILE *f = fopen("/sdcard/config_clock.txt", "w");
  if (f) {
    fprintf(f, "city=%s\nlatitude=%s\nlongitude=%s\ngmt=%d\noverride=%s\n", city_name, latitude, longitude, gmt_offset, location_override);
    fclose(f);
  } else {
    sdCardError = true;
  }
}

void loadSDConfigs() {
  sdcardPort = new CustomSDPort("/sdcard");
  
  memset(config_buffer, 0, sizeof(config_buffer));
  if (sdcardPort->SDPort_ReadFile("/sdcard/config_wifi.txt", (uint8_t *)config_buffer, NULL) == 0) {
    char *line = strtok(config_buffer, "\r\n");
    while (line != NULL) {
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "wifi_ssid") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(wifi_ssid, val, sizeof(wifi_ssid) - 1);
            wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
          }
        } else if (strcmp(key, "wifi_pass") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(wifi_pass, val, sizeof(wifi_pass) - 1);
            wifi_pass[sizeof(wifi_pass) - 1] = '\0';
          }
        }
      }
      line = strtok(NULL, "\r\n");
    }
  }

  memset(config_buffer, 0, sizeof(config_buffer));
  if (sdcardPort->SDPort_ReadFile("/sdcard/config_server.txt", (uint8_t *)config_buffer, NULL) == 0) {
    char *line = strtok(config_buffer, "\r\n");
    while (line != NULL) {
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        // Fix typo handler from client SD
        if (strcmp(key, "mac_server") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(mac_server, val, sizeof(mac_server) - 1);
            mac_server[sizeof(mac_server) - 1] = '\0';
          }
        } else if (strcmp(key, "device_id") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(device_id, val, sizeof(device_id) - 1);
            device_id[sizeof(device_id) - 1] = '\0';
          }
        } else if (strcmp(key, "key_id") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(key_id, val, sizeof(key_id) - 1);
            key_id[sizeof(key_id) - 1] = '\0';
          }
        } else if (strcmp(key, "hmac_secret") == 0) {
          if (val && strlen(val) > 0) {
            strncpy(hmac_secret, val, sizeof(hmac_secret) - 1);
            hmac_secret[sizeof(hmac_secret) - 1] = '\0';
          }
        }
      }
      line = strtok(NULL, "\r\n");
    }
  }

  loadSDClockConfig();
}

// ================= RECORDING FILES: FILESYSTEM AS STATE MACHINE =================
// Recordings are /sdcard/meetings/YYYYMMDD_HHMMSS_<recording_id>_c<0|1>_<status>.wav
// with status pending -> OK | failed | rejected. The filename is the whole state: the identity and clock
// validity are fixed when fopen() creates the file, and the status only changes through rename(), which
// is atomic on FAT. There is no RAM tracking and no manifest that a brownout could leave half-written.
// Legacy names (YYYYMMDD_HHMMSS_<status>.wav) carry no recording_id.
String withRecordingStatus(const String &wavPath, const char *status) {
  int us = wavPath.lastIndexOf('_');
  if (us == -1) return wavPath;
  return wavPath.substring(0, us) + "_" + status + ".wav";
}

// Reads recording_id and clock validity back from the filename; false for legacy names.
bool parseRecordingIdentity(const String &wavPath, String &recordingId, int &clockValid) {
  int slash = wavPath.lastIndexOf('/');
  String name = wavPath.substring(slash + 1);
  // YYYYMMDD_HHMMSS_ = 16 chars, UUID = 36 chars, then "_c?_" and the status
  if (name.length() < 57 || name.charAt(15) != '_' || name.charAt(52) != '_' ||
      name.charAt(53) != 'c' || name.charAt(55) != '_') {
    return false;
  }
  recordingId = name.substring(16, 52);
  clockValid = (name.charAt(54) == '1') ? 1 : 0;
  return true;
}

// Only uploaded (_OK) recordings expire. Failed, pending and rejected files are unsent data and stay until handled.
void cleanOldRecordings() {
  DIR *dir = opendir("/sdcard/meetings");
  if (!dir) {
    if (mkdir("/sdcard/meetings", 0777) != 0) {
      sdCardError = true;
    }
    return;
  }

  time_t now;
  time(&now);

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    String fname = String(entry->d_name);
    if (fname.endsWith("_OK.wav") && fname.length() >= 19) {
      struct tm ftime;
      memset(&ftime, 0, sizeof(ftime));
      int yr, mon, dy, hr, mn, sc;
      if (sscanf(fname.c_str(), "%4d%2d%2d_%2d%2d%2d", &yr, &mon, &dy, &hr, &mn, &sc) == 6) {
        ftime.tm_year = yr - 1900;
        ftime.tm_mon  = mon - 1;
        ftime.tm_mday = dy;
        ftime.tm_hour = hr;
        ftime.tm_min  = mn;
        ftime.tm_sec  = sc;
        time_t f_epoch = mktime(&ftime);
        if (difftime(now, f_epoch) > 172800) {
          unlink((String("/sdcard/meetings/") + fname).c_str());
        }
      }
    }
  }
  closedir(dir);
}

// A crash or power loss mid-recording leaves a _pending file whose header is still 44 zero bytes.
// Rewrite the header from the file size and queue it as _failed so it gets uploaded.
void recoverPendingRecordings() {
  DIR *dir = opendir("/sdcard/meetings");
  if (!dir) return;

  std::vector<String> pending;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    String fname = String(entry->d_name);
    if (fname.endsWith("_pending.wav")) pending.push_back(String("/sdcard/meetings/") + fname);
  }
  closedir(dir);

  for (const String &path : pending) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (st.st_size <= 44) {
      unlink(path.c_str()); // Nothing was recorded
      continue;
    }
    FILE *f = fopen(path.c_str(), "r+b");
    if (!f) {
      sdCardError = true;
      continue;
    }
    uint8_t header[44];
    writeWavHeader(header, (uint32_t)(st.st_size - 44));
    bool ok = (fwrite(header, 1, 44, f) == 44);
    fclose(f);
    if (!ok) {
      sdCardError = true;
      continue;
    }
    rename(path.c_str(), withRecordingStatus(path, "failed").c_str());
  }
}

// ================= LOCALIZATION STRATEGY =================
void applyLocalizationStrategy() {
  // Offline Mode (No Internet): Mandatory read from local file data, ignoring override flag entirely
  if (WiFi.status() != WL_CONNECTED) {
    loadSDClockConfig();
    return;
  }

  // Online Mode:
  // If override=true in the .txt file: Read location details directly from the .txt file
  if (strcasecmp(location_override, "true") == 0) {
    loadSDClockConfig();
  } else {
    // If override=false or empty: Read location details from wifi_database.json associated with active network
    String currentSSID = WiFi.SSID();
    JsonDocument doc;
    bool foundInDb = false;

    if (readWiFiDatabase(doc) && doc.is<JsonArray>()) {
      for (JsonObject entry : doc.as<JsonArray>()) {
        if (entry["essid"] == currentSSID.c_str()) {
          const char* db_city = entry["city"].as<const char*>(); // nullptr unless the value is a string
          if (db_city && strlen(db_city) > 0) {
            strncpy(city_name, db_city, sizeof(city_name) - 1);
            city_name[sizeof(city_name) - 1] = '\0';
          }
          if (!entry["latitude"].isNull()) {
            dtostrf(entry["latitude"].as<float>(), 6, 4, latitude);
          }
          if (!entry["longitude"].isNull()) {
            dtostrf(entry["longitude"].as<float>(), 6, 4, longitude);
          }
          if (!entry["gmt"].isNull()) {
            gmt_offset = entry["gmt"].as<int>();
          }
          foundInDb = true;
          break;
        }
      }
    }

    if (!foundInDb) {
      handleFirstTimeConnection(currentSSID.c_str(), wifi_pass);
    }
  }
}

// ================= NTP & WEATHER =================
void timeSyncNotificationCallback(struct timeval *tv) {
  ntpPacketReceived = true;
}

bool syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return false;

  ntpPacketReceived = false;
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  esp_sntp_set_time_sync_notification_cb(timeSyncNotificationCallback);
  configTime(gmt_offset * 3600, 0, "pool.ntp.org", "time.nist.gov");

  int retry = 0;
  while (!ntpPacketReceived && retry < 25) {
    delay(400);
    retry++;
  }

  // If no NTP packet arrived over internet, exit without modifying RTC
  if (!ntpPacketReceived) {
    return false;
  }

  struct tm ti;
  if (!getLocalTime(&ti, 2000)) return false;

  rtc.setDate(ti.tm_wday, ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
  rtc.setTime(ti.tm_hour, ti.tm_min, ti.tm_sec);
  clockSyncedSinceBoot = true;
  // TZ must change together with the RTC: filename timestamps are RTC local time
  rtc_gmt_offset = gmt_offset;
  applyTimezone();
  return true;
}

// The RTC holds local time, so a new UTC offset (another city's network, a DST change, or an
// Open-Meteo correction) leaves the displayed clock wrong until the 00:00 sync. Rewrite it now.
// A failed sync leaves rtc_gmt_offset unchanged, so the next sync attempt retries.
void resyncClockIfOffsetChanged() {
  if (gmt_offset != rtc_gmt_offset) {
    syncNTP();
  }
}

// The RTC holds local time; mktime() needs TZ to convert it (and WAV filenames) to UTC epochs.
// POSIX TZ signs are inverted: GMT-5 is "UTC+5".
void applyTimezone() {
  char tz[16];
  snprintf(tz, sizeof(tz), "UTC%+d", -gmt_offset);
  setenv("TZ", tz, 1);
  tzset();
}

// SNTP is the only thing that sets the system clock. Without this, an offline boot leaves
// time(NULL) near 1970 and every signed upload fails with 401 clock_skew.
void seedSystemClockFromRTC() {
  rtc_gmt_offset = gmt_offset; // Best available assumption: the RTC was last set with the saved offset
  applyTimezone();
  int yr = rtc.getYear();
  if (yr < 2024) return; // RTC never set; leave the system clock alone
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = yr - 1900;
  t.tm_mon  = rtc.getMonth() - 1;
  t.tm_mday = rtc.getDay();
  t.tm_hour = rtc.getHour();
  t.tm_min  = rtc.getMinute();
  t.tm_sec  = rtc.getSecond();
  t.tm_isdst = 0;
  struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
  settimeofday(&tv, NULL);
}

void updateOpenMeteo() {
  if (WiFi.status() != WL_CONNECTED) {
    lastWeatherSuccess = false;
    return;
  }
  HTTPClient http;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + latitude +
               "&longitude=" + longitude +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code" +
               "&hourly=precipitation_probability,precipitation" +
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max" +
               "&timezone=auto";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    JsonDocument doc;
    deserializeJson(doc, http.getString());
    
    // Reliable fallback: Open-Meteo always provides exact offset in seconds
    if (!doc["utc_offset_seconds"].isNull()) {
      gmt_offset = doc["utc_offset_seconds"].as<int>() / 3600;
    } else if (!doc["timezone_abbreviation"].isNull()) {
      const char* tz_abbr = doc["timezone_abbreviation"].as<const char*>();
      const char* p = strstr(tz_abbr, "GMT");
      if (p) {
        gmt_offset = atoi(p + 3);
      } else {
        int parsed = atoi(tz_abbr);
        if (parsed != 0) {
          gmt_offset = parsed;
        }
      }
    }

    cur_temp = round(doc["current"]["temperature_2m"].as<float>());
    cur_humidity = round(doc["current"]["relative_humidity_2m"].as<float>());
    feels_like = round(doc["current"]["apparent_temperature"].as<float>());
    weather_code = doc["current"]["weather_code"].as<int>();

    max_temp = round(doc["daily"]["temperature_2m_max"][0].as<float>());
    min_temp = round(doc["daily"]["temperature_2m_min"][0].as<float>());
    precip_sum = doc["daily"]["precipitation_sum"][0].as<float>();
    rain_prob_max = round(doc["daily"]["precipitation_probability_max"][0].as<float>());

    will_rain = (rain_prob_max > 30 || precip_sum > 0.5f);

    if (weather_code <= 1) strcpy(weather_label, "Clear");
    else if (weather_code <= 3) strcpy(weather_label, "Cloudy");
    else if (weather_code <= 48) strcpy(weather_label, "Foggy");
    else if (weather_code <= 67 || (weather_code >= 80 && weather_code <= 82)) strcpy(weather_label, "Rainy");
    else if ((weather_code >= 71 && weather_code <= 77) || weather_code == 85 || weather_code == 86) strcpy(weather_label, "Snow");
    else if (weather_code >= 95) strcpy(weather_label, "Storm");
    else strcpy(weather_label, "Fair");

    rain_start_hour = -1;
    rain_stop_hour = -1;
    JsonArray hourly_precip = doc["hourly"]["precipitation"];
    for (int h_idx = 0; h_idx < min(24, (int)hourly_precip.size()); h_idx++) {
      if (hourly_precip[h_idx].as<float>() > 0.1f) {
        if (rain_start_hour == -1) rain_start_hour = h_idx;
        rain_stop_hour = h_idx + 1;
      }
    }
    lastWeatherSuccess = true;
  } else {
    lastWeatherSuccess = false;
  }
  http.end();
}

// ================= SCREEN 1: CLOCK & CALENDAR =================
void drawClockCalendarScreen() {
  canvas.fillScreen(0);

  int cur_hour = rtc.getHour();
  int cur_min  = rtc.getMinute();
  int cur_wday = rtc.getWeekday();
  int cur_day  = rtc.getDay();
  int cur_mon  = rtc.getMonth();
  int cur_year = rtc.getYear();

  if (cur_mon < 1 || cur_mon > 12) cur_mon = 1;
  if (cur_year < 2000) cur_year = 2026;
  if (cur_day < 1 || cur_day > 31) cur_day = 1;

  char timeBuf[8];
  sprintf(timeBuf, "%02d:%02d", cur_hour, cur_min);
  canvas.setFont(&DSEG7_Classic_Bold_84);
  canvas.setTextColor(1);
  canvas.setCursor(10, 85);
  canvas.print(timeBuf);

  // Battery Indicator (Centered percentage number inside icon, no '%' symbol)
  readBattery();
  int bX = 343, bY = 12;
  canvas.fillRect(bX, bY, 46, 22, 1);
  canvas.fillRect(bX + 3, bY + 3, 40, 16, 0);
  canvas.fillRect(bX + 46, bY + 6, 4, 10, 1);

  char batStr[8];
  sprintf(batStr, "%d", getBatteryPercentage(v_bat));
  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setTextColor(1);
  int16_t bx1, by1;
  uint16_t bw, bh;
  canvas.getTextBounds(batStr, 0, 0, &bx1, &by1, &bw, &bh);
  int textX = (bX + 3) + (40 - (int)bw) / 2 - bx1;
  int textY = (bY + 3) + (16 - (int)bh) / 2 - by1;
  canvas.setCursor(textX, textY);
  canvas.print(batStr);

  canvas.fillRect(10, 95, 380, 2, 1);

  // 1. Calendar Date Header with extra breathing room
  canvas.setFont(&FreeSansBold9pt7b);
  char dateHeader[32];
  sprintf(dateHeader, "%02d/%02d/%04d", cur_day, cur_mon, cur_year);
  canvas.setCursor(10, 114);
  canvas.print(dateHeader);

  // 2. Weekday Row with taller, balanced highlight boxes
  const char *wLabels[] = {"S", "M", "T", "W", "T", "F", "S"};
  int calX = 10;
  int calY = 136; // Lowered baseline gives more separation below the date
  int colW = 24;
  int rowH = 20;  // Expanded row pitch for taller highlights

  for (int c = 0; c < 7; c++) {
    if (c == cur_wday) {
      // 18px tall rounded box with 3px top and 4px bottom padding
      canvas.fillRoundRect(calX + (c * colW) - 1, calY - 13, 21, 18, 3, 1);
      canvas.setTextColor(0);
    } else {
      canvas.setTextColor(1);
    }
    canvas.setCursor(calX + (c * colW) + 4, calY);
    canvas.print(wLabels[c]);
  }
  canvas.setTextColor(1);

  // 3. Calendar Grid Generation
  struct tm firstDayTm = {0};
  firstDayTm.tm_year = cur_year - 1900;
  firstDayTm.tm_mon  = cur_mon - 1;
  firstDayTm.tm_mday = 1;
  mktime(&firstDayTm);
  int startCol = firstDayTm.tm_wday;

  int totalDays = daysPerMonth[cur_mon - 1];
  if (cur_mon == 2 && isLeapYear(cur_year)) totalDays = 29;

  int dCount = 1;
  for (int r = 0; r < 6; r++) {
    for (int c = 0; c < 7; c++) {
      if ((r == 0 && c < startCol) || dCount > totalDays) continue;
      int posX = calX + (c * colW);
      int posY = calY + 20 + (r * rowH);

      if (dCount == cur_day) {
        // 18px tall rounded box with generous top/bottom padding
        canvas.fillRoundRect(posX - 1, posY - 13, 21, 18, 3, 1);
        canvas.setTextColor(0);
      } else {
        canvas.setTextColor(1);
      }
      canvas.setCursor(posX, posY);
      canvas.printf("%2d", dCount);
      dCount++;
    }
  }

  // Vertical Divider
  canvas.fillRect(182, 102, 2, 180, 1);

  // Weather Unit
  canvas.setTextColor(1);
  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(195, 114);
  canvas.print(city_name);

  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setCursor(195, 150);
  canvas.printf("%dC", cur_temp);

  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(265, 150);
  canvas.printf("%s", weather_label);

  canvas.setCursor(195, 168);
  canvas.printf("Min/Max: %d / %dC", min_temp, max_temp);

  canvas.setCursor(195, 186);
  canvas.printf("Feels: %dC | Hum: %d%%", feels_like, cur_humidity);

  canvas.setCursor(195, 204);
  canvas.printf("Rain?: %s (%d%%)", will_rain ? "Yes" : "No", rain_prob_max);

  if (rain_start_hour != -1) {
    canvas.setCursor(195, 222);
    canvas.printf("Rain: %02d:00-%02d:00", rain_start_hour, rain_stop_hour);
  } else {
    canvas.setCursor(195, 222);
    canvas.printf("Rain: None expected");
  }

  canvas.setCursor(195, 240);
  canvas.printf("Precip Sum: %.1f mm", precip_sum);

  shtc3_read_i2cbus(temp_shtc3, hum_shtc3);
  canvas.setCursor(195, 262);
  canvas.printf("Local: %dC | %d%% RH", (int)round(temp_shtc3), (int)round(hum_shtc3));

  // Footer: Cached IP address displayed in bottom-left corner
  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(10, 285);
  canvas.print(lastKnownIP);

  pushCanvasToRLCD(false);
}

// ================= SCREEN 2: MEETING ASSISTANT (16-BIT PCM WAV) =================

// Fast 16-bit PCM to 8-bit G.711 Mu-law compressor
inline uint8_t pcm16_to_ulaw(int16_t pcm_val) {
  const int16_t BIAS = 0x84;
  const int16_t CLIP = 32635;
  int16_t sign = (pcm_val >> 8) & 0x80;
  if (sign != 0) pcm_val = -pcm_val;
  if (pcm_val > CLIP) pcm_val = CLIP;
  pcm_val = (pcm_val + BIAS) >> 2;

  int16_t exponent = 7;
  for (int16_t expMask = 0x4000; (pcm_val & expMask) == 0 && exponent > 0; expMask >>= 1) {
    exponent--;
  }
  int16_t mantissa = (pcm_val >> (exponent + 3)) & 0x0F;
  return ~(uint8_t)(sign | (exponent << 4) | mantissa);
}

void writeWavHeader(uint8_t* buffer, uint32_t dataSize) {
  memcpy(buffer, "RIFF", 4);
  uint32_t chunkSize = dataSize + 36;
  memcpy(buffer + 4, &chunkSize, 4);
  memcpy(buffer + 8, "WAVEfmt ", 8);

  uint32_t subChunk1Size = 16;
  uint16_t audioFormat = 7;                                      // 7 = WAVE_FORMAT_MULAW (G.711 compression)
  uint16_t numChannels = CHANNELS;                               // Uses global
  uint32_t sampleRate = SAMPLE_RATE;                             // Uses global
  uint16_t blockAlign = CHANNELS * 1;                            // 1 byte per sample (8-bit compressed)
  uint32_t byteRate = sampleRate * blockAlign;                   // 16000 bytes/sec
  uint16_t bitsPerSample = 8;                                    // Compressed 8-bit

  memcpy(buffer + 16, &subChunk1Size, 4);
  memcpy(buffer + 20, &audioFormat, 2);
  memcpy(buffer + 22, &numChannels, 2);
  memcpy(buffer + 24, &sampleRate, 4);
  memcpy(buffer + 28, &byteRate, 4);
  memcpy(buffer + 32, &blockAlign, 2);
  memcpy(buffer + 34, &bitsPerSample, 2);

  memcpy(buffer + 36, "data", 4);
  memcpy(buffer + 40, &dataSize, 4);
}

// "HTTP/1.1 202 ACCEPTED" -> 202; anything else -> -1
int parseHttpStatus(const String &line) {
  if (!line.startsWith("HTTP/")) return -1;
  int sp = line.indexOf(' ');
  if (sp == -1) return -1;
  return line.substring(sp + 1, sp + 4).toInt();
}

// Waits for the next status line, or returns "" on timeout or a closed connection.
String readStatusLine(WiFiClient &client, unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) return line;
    } else if (!client.connected()) {
      break;
    }
    delay(10);
  }
  return "";
}

// Consumes the headers and a small JSON body after a status line, and extracts error_code.
void readResponseRest(WiFiClient &client, unsigned long timeoutMs, String &errorCode) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs && (client.connected() || client.available())) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
    } else {
      delay(10);
    }
  }
  char body[512] = {0};
  size_t len = 0;
  start = millis();
  while (millis() - start < timeoutMs && len < sizeof(body) - 1 && (client.connected() || client.available())) {
    if (client.available()) {
      body[len++] = (char)client.read();
    } else {
      delay(10);
    }
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    errorCode = doc["error_code"] | "";
  }
}

UploadResult classifyUploadResponse(int status, const String &errorCode) {
  if (status == 200 || status == 202) return UPLOAD_OK;
  if (status == 409) {
    if (errorCode == "duplicate") return UPLOAD_OK;               // Server already has this recording
    if (errorCode == "recording_conflict") return UPLOAD_REJECTED;
    return UPLOAD_RETRY_LATER;                                    // e.g. upload_in_progress
  }
  if (status == 401 && errorCode == "clock_skew") return UPLOAD_CLOCK_SKEW;
  if (status == 400 && errorCode == "hash_mismatch") return UPLOAD_RETRY_LATER; // SD read glitch: re-hash next attempt
  if (status == 408 || status == 429 || status == 507 || status >= 500 || status < 0) return UPLOAD_RETRY_LATER;
  return UPLOAD_REJECTED;
}

UploadResult streamFileToBackend(const char* filepath) {
  if (WiFi.status() != WL_CONNECTED) return UPLOAD_RETRY_LATER;

  // Disable power save to prevent dropped packets, but lower TX power
  // to 8.5dBm to prevent 3.3V rail brownouts during SD card reads.
  // Applied before the SHA-256 pass: hashing is a full SD read of the recording too.
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  FILE *f = fopen(filepath, "rb");
  if (!f) {
    sdCardError = true;
    WiFi.setSleep(true);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    return UPLOAD_RETRY_LATER;
  }

  fseek(f, 0, SEEK_END);
  long fileSize = ftell(f);
  fseek(f, 0, SEEK_SET);

  String serverStr = String(mac_server);
  serverStr.replace("http://", "");
  serverStr.replace("https://", "");
  int colonIdx = serverStr.indexOf(':');
  String host = (colonIdx != -1) ? serverStr.substring(0, colonIdx) : serverStr;
  int port = (colonIdx != -1) ? serverStr.substring(colonIdx + 1).toInt() : 8443;

  String content_sha256 = computeFileSHA256(filepath);
  String request_id = generateUUIDv4();
  unsigned long request_timestamp = time(NULL);

  // Identity comes from the filename fixed at recording start, so every retry sends the same values.
  // Legacy names have no ID: the header is omitted and the server derives it from device_id + start time.
  String recording_id = "";
  int file_clock_valid = 0;
  parseRecordingIdentity(String(filepath), recording_id, file_clock_valid);

  unsigned long file_recording_started_at = 0;
  const char* fname = strrchr(filepath, '/');
  fname = fname ? fname + 1 : filepath;
  int yr, mon, dy, hr, mn, sc;
  if (sscanf(fname, "%4d%2d%2d_%2d%2d%2d", &yr, &mon, &dy, &hr, &mn, &sc) == 6) {
    struct tm ftime;
    memset(&ftime, 0, sizeof(ftime));
    ftime.tm_year = yr - 1900;
    ftime.tm_mon  = mon - 1;
    ftime.tm_mday = dy;
    ftime.tm_hour = hr;
    ftime.tm_min  = mn;
    ftime.tm_sec  = sc;
    file_recording_started_at = mktime(&ftime);
  }

  // 5-field HMAC Signature matching the backend requirement
  String canonical = String(device_id) + "\n" +
                     String(key_id) + "\n" +
                     String(request_timestamp) + "\n" +
                     content_sha256 + "\n" +
                     String(fileSize);

  String signature = computeHMACSHA256(canonical, hmac_secret);

  int status = -1;
  String errorCode = "";
  WiFiClient client;
  client.setTimeout(30000); 

  if (client.connect(host.c_str(), port)) {
    client.print("POST /api/upload HTTP/1.1\r\n");
    client.print("Host: " + host + "\r\n");
    client.print("Content-Type: audio/wav\r\n");
    client.print("Content-Length: " + String(fileSize) + "\r\n");
    client.print("Expect: 100-continue\r\n");
    client.print("X-Device-Id: " + String(device_id) + "\r\n");
    client.print("X-Key-Id: " + String(key_id) + "\r\n");
    client.print("X-Request-Id: " + request_id + "\r\n");
    if (recording_id.length() > 0) {
      client.print("X-Recording-Id: " + recording_id + "\r\n");
    }
    client.print("X-Recording-Started-At: " + String(file_recording_started_at) + "\r\n");
    client.print("X-Clock-Valid: " + String(file_clock_valid) + "\r\n");
    client.print("X-Request-Timestamp: " + String(request_timestamp) + "\r\n");
    client.print("X-Content-SHA256: " + content_sha256 + "\r\n");
    client.print("X-Signature: " + signature + "\r\n");
    client.print("Connection: close\r\n\r\n");

    // The server answers 100 Continue once the headers pass, or a final status (401, 409, duplicate 202...)
    // without reading the body. No answer within 5 s means it ignores Expect, so send the body anyway.
    String firstLine = readStatusLine(client, 5000);
    int firstStatus = parseHttpStatus(firstLine);
    bool sendBody = (firstLine.length() == 0 || firstStatus == 100);

    if (firstStatus == 100) {
      while (client.available() && client.readStringUntil('\n') != "\r");
    } else if (firstStatus > 0) {
      status = firstStatus;
      readResponseRest(client, 5000, errorCode);
    }

    if (sendBody) {
      bool bodyComplete = false;
      uint8_t *sendBuf = (uint8_t*)malloc(AUDIO_FRAME_SIZE); // Uses global AUDIO_FRAME_SIZE
      if (sendBuf) {
        size_t remaining = fileSize;
        while (remaining > 0) {
          size_t toRead = (remaining > AUDIO_FRAME_SIZE) ? AUDIO_FRAME_SIZE : remaining;
          size_t bytesRead = fread(sendBuf, 1, toRead, f);
          if (bytesRead == 0) break; // File read error

          // Robust retry loop for Wi-Fi buffer congestion
          size_t written = 0;
          int retries = 0;
          while (written < bytesRead && retries < 20) {
            size_t res = client.write(sendBuf + written, bytesRead - written);
            if (res > 0) {
              written += res;
              retries = 0; // Reset retries on successful write
            } else {
              retries++;
              delay(50); // Yield to LwIP network stack to free TX buffers
            }
          }
          if (written < bytesRead) break; // Hard disconnect after 1 full second of failure

          remaining -= bytesRead;
          delay(2); // Keep watchdog alive
        }
        bodyComplete = (remaining == 0);
        free(sendBuf);
      }

      if (bodyComplete) {
        // Skip a late interim 100 Continue if the body went out before it arrived
        String line = readStatusLine(client, 10000);
        if (parseHttpStatus(line) == 100) {
          while (client.available() && client.readStringUntil('\n') != "\r");
          line = readStatusLine(client, 10000);
        }
        status = parseHttpStatus(line);
        if (status > 0) readResponseRest(client, 5000, errorCode);
      }
    }
    client.stop();
  }
  fclose(f);
  
  // Re-enable power save mode and restore maximum TX power
  WiFi.setSleep(true); 
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  
  return classifyUploadResponse(status, errorCode);
}

// Uploads one recording and renames it by outcome. A clock_skew rejection gets one NTP resync and retry.
UploadResult uploadRecordingFile(String &wavPath) {
  UploadResult result = streamFileToBackend(wavPath.c_str());
  if (result == UPLOAD_CLOCK_SKEW && syncNTP()) {
    result = streamFileToBackend(wavPath.c_str());
  }

  const char *status = (result == UPLOAD_OK) ? "OK" : (result == UPLOAD_REJECTED) ? "rejected" : "failed";
  String newPath = withRecordingStatus(wavPath, status);
  if (newPath != wavPath && rename(wavPath.c_str(), newPath.c_str()) == 0) {
    wavPath = newPath;
  }
  return result;
}

void drawMeetingAssistantUI(const char* extraStatus = NULL) {
  canvas.fillScreen(0);
  canvas.setTextColor(1);
  
  const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char timeHeader[32];
  sprintf(timeHeader, "%s %02d/%02d %02d:%02d", days[rtc.getWeekday()], rtc.getDay(), rtc.getMonth(), rtc.getHour(), rtc.getMinute());

  canvas.setFont(&FreeSansBold9pt7b);
  
  // Calculate text width to right-align
  int16_t tx1, ty1;
  uint16_t tw, th;
  canvas.getTextBounds(timeHeader, 0, 0, &tx1, &ty1, &tw, &th);
  int timeX = W - tw - 20; // W=400, leaves 20px padding on the right edge
  
  canvas.setCursor(timeX, 20);
  canvas.print(timeHeader);

  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setCursor(20, 45);
  canvas.print("Meeting Assistant");

  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(20, 75);
  if (lastNetworkAvailable) {
    canvas.print("Network: Ready (Cloud Sync)");
  } else {
    canvas.print("Network: Offline (SD Card Only)");
  }

  canvas.fillRect(20, 88, 360, 2, 1);

  // ---------- SD ERROR WARNING DIALOG ----------
  if (sdCardError) {
    canvas.fillRoundRect(20, 105, 360, 140, 6, 1);
    canvas.setTextColor(0);
    
    canvas.setFont(&FreeSansBold12pt7b);
    canvas.setCursor(35, 140);
    canvas.print("SD Card Error!");

    canvas.setFont(&FreeSansBold9pt7b);
    canvas.setCursor(35, 175);
    canvas.print("The SD card was not read or");
    canvas.setCursor(35, 198);
    canvas.print("timed out during operation.");
    
    canvas.setCursor(35, 230);
    canvas.print("Please restart the device.");
    
    canvas.setTextColor(1);
    pushCanvasToRLCD(false);
    return;
  }

  // Mandatory instructions
  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(20, 115);
  canvas.print("Press the right button");
  canvas.setCursor(20, 135);
  canvas.print("to start and stop recording.");

  if (isRecording) {
    canvas.drawCircle(40, 185, 14, 1);
    canvas.fillCircle(40, 185, 9, 1);

    canvas.setFont(&FreeSansBold12pt7b);
    canvas.setCursor(65, 192);
    canvas.print("RECORDING...");

    unsigned long elapsedSec = (millis() - recordingStartTime) / 1000;
    char timerBuf[16];
    sprintf(timerBuf, "%02lu:%02lu", elapsedSec / 60, elapsedSec % 60);

    canvas.setFont(&FreeSansBold18pt7b);
    canvas.setCursor(65, 235);
    canvas.print(timerBuf);
  } else {
    canvas.drawCircle(40, 185, 14, 1);

    canvas.setFont(&FreeSansBold12pt7b);
    canvas.setCursor(65, 192);
    canvas.print("Ready to Record");

    if (extraStatus != NULL) {
      canvas.setFont(&FreeSansBold9pt7b);
      canvas.setCursor(20, 235);
      canvas.print(extraStatus);
    } else if (strlen(currentMeetingFileName) > 0) {
      canvas.setFont(&FreeSansBold9pt7b);
      canvas.setCursor(20, 235);
      // The full name (with the recording_id) is too wide for the screen: show "YYYYMMDD_HHMMSS (status)"
      String lastName = String(currentMeetingFileName);
      lastName = lastName.substring(lastName.lastIndexOf('/') + 1);
      String lastStatus = lastName.substring(lastName.lastIndexOf('_') + 1);
      lastStatus.replace(".wav", "");
      canvas.print("Last: " + lastName.substring(0, 15) + " (" + lastStatus + ")");
    }
  }

  pushCanvasToRLCD(false);
}

const char* uploadResultMessage(UploadResult result) {
  switch (result) {
    case UPLOAD_OK:       return "Saved & Transmitted!";
    case UPLOAD_REJECTED: return "Rejected by server (kept)";
    default:              return "Upload Failed (will retry)";
  }
}

void processFailedUploads() {
  // Collect names first: renaming entries while readdir() walks the FAT directory can skip files
  DIR *dir = opendir("/sdcard/meetings");
  if (!dir) return;
  std::vector<String> failed;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    String fname = String(entry->d_name);
    if (fname.endsWith("_failed.wav")) failed.push_back(fname);
  }
  closedir(dir);

  for (const String &fname : failed) {
    String fullPath = String("/sdcard/meetings/") + fname;
    String shortName = fname.substring(9, 15);
    drawMeetingAssistantUI(("Syncing " + shortName + "...").c_str());
    UploadResult result = uploadRecordingFile(fullPath);
    drawMeetingAssistantUI(uploadResultMessage(result));
  }

  drawMeetingAssistantUI(failed.empty() ? "Saved & Transmitted!" : "All syncs complete!");
}

void startMeetingRecording() {
  if (sdCardError) return;

  // The identity is part of the name from the moment the file exists, so every retry
  // (even after a reboot) sends the same recording_id
  snprintf(currentMeetingFileName, sizeof(currentMeetingFileName),
           "/sdcard/meetings/%04d%02d%02d_%02d%02d%02d_%s_c%d_pending.wav",
           rtc.getYear(), rtc.getMonth(), rtc.getDay(),
           rtc.getHour(), rtc.getMinute(), rtc.getSecond(),
           generateUUIDv4().c_str(), clockSyncedSinceBoot ? 1 : 0);

  currentMeetingFile = fopen(currentMeetingFileName, "wb");
  if (!currentMeetingFile) {
    sdCardError = true;
    drawMeetingAssistantUI();
    return;
  }

  uint8_t dummyHeader[44] = {0};
  if (fwrite(dummyHeader, 1, 44, currentMeetingFile) < 44) {
    sdCardError = true;
    fclose(currentMeetingFile);
    currentMeetingFile = NULL;
    drawMeetingAssistantUI();
    return;
  }

  enterRecordingQuietMode(); // Radio, NTP, speaker and sensors off before the first audio frame

  totalRecordedBytes = 0;
  recordingStartTime = millis();
  isRecording = true;

  drawMeetingAssistantUI();
}

void stopMeetingRecording() {
  isRecording = false;

  if (currentMeetingFile) {
    if (fseek(currentMeetingFile, 0, SEEK_SET) != 0) {
      sdCardError = true;
    } else {
      uint8_t header[44];
      writeWavHeader(header, totalRecordedBytes);
      if (fwrite(header, 1, 44, currentMeetingFile) < 44) {
        sdCardError = true;
      }
    }
    fclose(currentMeetingFile);
    currentMeetingFile = NULL;
  }

  if (sdCardError) {
    drawMeetingAssistantUI();
    return;
  }

  // Re-establish connection during audio sync and disable after transmission concludes
  drawMeetingAssistantUI("Connecting Wi-Fi...");
  setLowPowerClock(false); // Boost CPU for high-speed transmission
  bool connected = ensureWiFiConnected(6);

  String meetingPath = String(currentMeetingFileName);
  if (connected) {
    drawMeetingAssistantUI("Syncing to Mac...");
    UploadResult result = uploadRecordingFile(meetingPath);
    strncpy(currentMeetingFileName, meetingPath.c_str(), sizeof(currentMeetingFileName) - 1);
    currentMeetingFileName[sizeof(currentMeetingFileName) - 1] = '\0';
    drawMeetingAssistantUI(uploadResultMessage(result));
    if (result == UPLOAD_OK) {
      processFailedUploads();
    }
    disableWiFi(); // Immediately shut down Wi-Fi after transmission concludes
  } else {
    drawMeetingAssistantUI("Saved to SD (Offline)");
    String failedName = withRecordingStatus(meetingPath, "failed");
    rename(meetingPath.c_str(), failedName.c_str());
    strncpy(currentMeetingFileName, failedName.c_str(), sizeof(currentMeetingFileName) - 1);
    currentMeetingFileName[sizeof(currentMeetingFileName) - 1] = '\0';
    disableWiFi(); // Power down Wi-Fi if connection attempt fails
  }
}

// ================= DYNAMIC QR CODE GENERATOR (NATIVE ESP-IDF) =================
static void qrcode_canvas_draw(esp_qrcode_handle_t qrcode) {
  int size = esp_qrcode_get_size(qrcode);
  int maxPixelSize = 155;
  int scale = maxPixelSize / size;
  if (scale < 1) scale = 1;
  int qrWidth = size * scale;
  int startX = (190 - qrWidth) / 2;
  int startY = 60;

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qrcode, x, y)) {
        canvas.fillRect(startX + (x * scale), startY + (y * scale), scale, scale, 1);
      }
    }
  }
}

void drawDynamicQRCode(const char* text) {
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrcode_canvas_draw;
  cfg.max_qrcode_version = 5;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, text);
}

void handleConfigServer() {
  server.on("/", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'/><style>body{font-family:sans-serif;padding:20px;}</style></head><body>";
    html += "<h2>Intelligent-Clock WiFi Wizard</h2><form method='POST' action='/save'>";
    html += "<label>Select Network:</label><br><select name='ssid'>";
    for (int i = 0; i < n; i++) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
    }
    html += "</select><br><br><label>Password:</label><br><input type='password' name='pass'/><br><br>";
    html += "<input type='submit' value='Connect & Save'/></form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    String req_ssid = server.arg("ssid");
    String req_pass = server.arg("pass");

    // Adding a network via Screen 3 overwrites the default network in the .txt file
    strncpy(wifi_ssid, req_ssid.c_str(), sizeof(wifi_ssid) - 1);
    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
    strncpy(wifi_pass, req_pass.c_str(), sizeof(wifi_pass) - 1);
    wifi_pass[sizeof(wifi_pass) - 1] = '\0';
    saveWiFiTxtConfig();

    server.send(200, "text/html", "<html><body><h3>Credentials Saved! Connecting...</h3></body></html>");
    delay(1000);

    WiFi.softAPdisconnect(true);
    dnsServer.stop();
    server.close();

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_pass);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      // First-Time Setup: WPS coordinates -> Weather (GMT) -> NTP -> BigDataCloud City -> Save
      handleFirstTimeConnection(wifi_ssid, wifi_pass);
      lastKnownIP = WiFi.localIP().toString();
      lastNetworkAvailable = true;
      saveWiFiTxtConfig();
      saveSDClockConfig();
      disableWiFi();

      // If a wifi connection is stablished when in the third screen, then go to first screen
      previousScreen = SCREEN_CONFIG;
      currentScreen = SCREEN_CLOCK;
      applyScreenPowerMode(currentScreen);
      drawClockCalendarScreen();
      setLowPowerClock(true);
    } else {
      lastKnownIP = "No Wi-Fi";
      lastNetworkAvailable = false;
      startWiFiConfiguratorAP();
    }
  });
}

// ================= SCREEN 3: DYNAMIC PASSWORDLESS QR CONFIG =================
void startWiFiConfiguratorAP() {
  setLowPowerClock(false); // Boost CPU for handling DNS and Web server requests

  IPAddress apIP(192, 168, 5, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);

  dnsServer.start(53, "*", apIP);
  handleConfigServer();
  server.begin();

  // Read the active SoftAP SSID
  String defaultSSID = WiFi.softAPSSID();
  if (defaultSSID.length() == 0) {
    defaultSSID = "Intelligent-Clock";
  }

  // Standard open/passwordless Wi-Fi QR format
  String qrPayload = "WIFI:S:" + defaultSSID + ";T:nopass;;";

  canvas.fillScreen(0);
  
  // Dynamically generated QR code
  drawDynamicQRCode(qrPayload.c_str());

  canvas.fillRect(190, 30, 2, 240, 1);

  canvas.setTextColor(1);
  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setCursor(205, 45);
  canvas.print("WiFi Setup");

  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(205, 75);  
  canvas.print("1. Scan QR Code");

  canvas.setCursor(205, 100); 
  canvas.print("   Network (SSID):");
  
  canvas.setCursor(205, 120); 
  canvas.print("   " + defaultSSID);

  canvas.setCursor(205, 142); 
  canvas.print("   (Passwordless)");

  canvas.setCursor(205, 172); 
  canvas.print("2. Open browser at:");

  canvas.setCursor(205, 192); 
  canvas.print("   http://192.168.5.1");

  canvas.setCursor(205, 225); 
  canvas.print("3. Follow Wizard");

  pushCanvasToRLCD(false);
}

// ================= POWER & DISPATCH =================
// Powers down the ES8311 DAC and its amplifier while keeping the ES7210 microphones running.
// Both codecs share one full-duplex I2S port: while the mic (RX) is open, esp_codec_dev only marks
// the TX disable as pending, so the mic clocks keep running; CodecPort_CloseMic() later stops both.
// The speaker is opened together with the mic on purpose: opening only the mic would leave the TX
// channel enabled from init_codec(), and the I2S clocks would never stop on the other screens.
// The ES8311 driver also owns the amplifier pin (pa: 46 in board_cfg.h) and lowers it on close.
void silenceSpeaker() {
  if (codecport && codecActive) {
    codecport->CodecPort_CloseSpeaker();
  }
  digitalWrite(PA_CTRL, LOW);
}

// Recording captures only the microphones: no RF bursts or network activity, no speaker hiss or
// feedback, no sensor I2C traffic. Called when a recording starts; every path below also refuses
// to wake these up while isRecording is set.
void enterRecordingQuietMode() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop(); // No periodic NTP retries in the background
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF); // Unconditional: disableWiFi() keeps the radio on for Screen 3
  btStop();            // No-op today (Bluetooth never runs); guarantees no BLE radio during recording
  silenceSpeaker();
  shtc3_sleep();
}

void applyScreenPowerMode(AppScreen scr) {
  if (scr == SCREEN_MEETING) {
    setLowPowerClock(false); // Boost CPU for 16 kHz I2S audio reading
    if (!codecport) {
      codecport = new CodecPort(I2cbus, "S3_RLCD_4_2");
      codecport->CodecPort_SetInfo("es8311 & es7210", 1, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE); // Uses global BITS_PER_SAMPLE
      codecport->CodecPort_SetMicGain(35);
      codecActive = true;
    } else if (!codecActive) {
      // Re-open the codec devices that were put in standby when leaving Screen 2
      codecport->CodecPort_SetInfo("es8311 & es7210", 1, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);
      codecport->CodecPort_SetMicGain(35);
      codecActive = true;
    }
    silenceSpeaker(); // Nothing is ever played: only the microphone path stays powered on Screen 2
  } else {
    if (isRecording) {
      stopMeetingRecording();
    }
    digitalWrite(PA_CTRL, LOW);
    if (codecport && codecActive) {
      // Put ES8311/ES7210 in standby and stop the I2S clocks while away from Screen 2.
      // Mic first: closing RX also applies the TX disable left pending by silenceSpeaker().
      codecport->CodecPort_CloseMic();
      codecport->CodecPort_CloseSpeaker();
      codecActive = false;
    }
  }

  if (scr == SCREEN_CONFIG) {
    startWiFiConfiguratorAP();
  } else if (previousScreen == SCREEN_CONFIG) {
    server.close();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    disableWiFi();
  }

  if (scr == SCREEN_CLOCK) {
    setLowPowerClock(true); // Return to 80 MHz low-power mode
  }
}

// Light sleep until the next minute rollover (RTC) or a press of the screen cycle button.
// RAM, peripherals and GPIO levels are retained, so the RLCD keeps its image and loop() resumes in place.
void enterClockLightSleep() {
  // Skip while the button is held, otherwise the low-level wakeup would fire immediately
  if (digitalRead(BUTTON_PIN) == LOW) return;

  int sec = rtc.getSecond();
  if (sec < 0 || sec > 59) sec = 0;
  uint64_t sleepUs = (uint64_t)(60 - sec) * 1000000ULL + 200000ULL; // Wake 200ms after the minute changes

  // Keep display, I2C, amplifier and button pins in their active configuration during sleep
  const gpio_num_t keepPins[] = {GPIO_NUM_12, GPIO_NUM_11, GPIO_NUM_5, GPIO_NUM_40, GPIO_NUM_41,
                                 (gpio_num_t)I2C_SDA, (gpio_num_t)I2C_SCL, (gpio_num_t)PA_CTRL,
                                 (gpio_num_t)BUTTON_PIN};
  for (gpio_num_t pin : keepPins) {
    gpio_sleep_sel_dis(pin);
  }

  esp_sleep_enable_timer_wakeup(sleepUs);
  gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  Serial.flush();
  esp_light_sleep_start();

  gpio_wakeup_disable((gpio_num_t)BUTTON_PIN);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Bluetooth is never used. The core already skips starting it and frees its memory at boot
  // (no BT library is linked); this keeps the controller off even if a future library starts it.
  btStop();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  RlcdPort.RLCD_Init();

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, LOW);

  rtc.setBus(I2cbus);
  rtc.begin();

  shtc3_read_i2cbus(temp_shtc3, hum_shtc3);

  loadSDConfigs();
  seedSystemClockFromRTC();   // Needs gmt_offset from config_clock.txt; before anything reads time(NULL)
  cleanOldRecordings();
  recoverPendingRecordings(); // Repair files cut off by a crash or power loss and queue them as _failed

  // Primary Attempt & Database Fallback during boot
  bool connected = ensureWiFiConnected(8);
  if (connected) {
    strncpy(current_network, WiFi.SSID().c_str(), sizeof(current_network) - 1);
    applyLocalizationStrategy();
    
    // System boot: Weather Data Trigger (fetches metrics & updates gmt_offset first)
    updateOpenMeteo();
    lastWeatherSync = millis();

    // System boot: Time Data Trigger (executes with verified gmt_offset)
    syncNTP();
    strncpy(last_synced_city, city_name, sizeof(last_synced_city) - 1);
    
    saveWiFiTxtConfig();
    saveSDClockConfig();
    disableWiFi(); // Power down Wi-Fi immediately after initial boot sync
  } else {
    lastWeatherSuccess = false;
    applyLocalizationStrategy(); // Mandatory offline file load
    disableWiFi();
  }

  drawClockCalendarScreen();
  lastDrawnMinute = rtc.getMinute();
  setLowPowerClock(true); // Drop clock speed to 80 MHz for power conservation
}

void loop() {
  // 1. Screen Switching Button (GPIO 18 - Left)
  bool reading = digitalRead(BUTTON_PIN);
  if (reading == LOW && lastButtonState == HIGH && (millis() - lastButtonPress > debounceTime)) {
    lastButtonPress = millis();
    previousScreen = currentScreen;

    // Cycle screens: Screen 3 only appears if Wi-Fi connection check on Screen 2 failed
    if (currentScreen == SCREEN_CLOCK) {
      currentScreen = SCREEN_MEETING;
    } else if (currentScreen == SCREEN_MEETING) {
      if (lastNetworkAvailable) {
        currentScreen = SCREEN_CLOCK;
      } else {
        currentScreen = SCREEN_CONFIG;
      }
    } else if (currentScreen == SCREEN_CONFIG) {
      currentScreen = SCREEN_CLOCK;
    }

    applyScreenPowerMode(currentScreen);

    if (currentScreen == SCREEN_CLOCK) {
      drawClockCalendarScreen();
      disableWiFi(); // Disable Wi-Fi after rendering Clock Screen
      setLowPowerClock(true);
    } else if (currentScreen == SCREEN_MEETING) {
      // Check whether a connection can be established and turn off Wi-Fi
      ensureWiFiConnected(6); // Tests connection, sets lastNetworkAvailable & lastKnownIP
      disableWiFi();          // Powers down Wi-Fi to keep audio pristine & save power
      drawMeetingAssistantUI();
    }
  }
  lastButtonState = reading;

  // 2. Right Button (GPIO 0 - BOOT) for Recording Toggle
  if (currentScreen == SCREEN_MEETING) {
    bool recBtn = digitalRead(RECORD_BUTTON_PIN);
    if (recBtn == LOW && lastRecButtonState == HIGH && (millis() - lastRecButtonPress > debounceTime)) { // Uses global debounceTime
      lastRecButtonPress = millis();
      if (!isRecording) {
        startMeetingRecording();
      } else {
        stopMeetingRecording();
      }
    }
    lastRecButtonState = recBtn;

    // Stream uncompressed 16-bit linear PCM frames to SD card
    if (isRecording) {
      if (codecport) {
        codecport->CodecPort_EchoRead(frame_buffer, AUDIO_FRAME_SIZE);

        if (currentMeetingFile) {
          // Convert 16-bit PCM to 8-bit ulaw in-place
          int num_samples = AUDIO_FRAME_SIZE / 2;
          int16_t* pcm_samples = (int16_t*)frame_buffer;
          for (int i = 0; i < num_samples; i++) {
            frame_buffer[i] = pcm16_to_ulaw(pcm_samples[i]);
          }

          size_t written = fwrite(frame_buffer, 1, num_samples, currentMeetingFile);
          if (written < num_samples || ferror(currentMeetingFile)) {
            sdCardError = true;
            isRecording = false;
            fclose(currentMeetingFile);
            currentMeetingFile = NULL;
            drawMeetingAssistantUI();
          } else {
            totalRecordedBytes += written;
          }
        }
      }

      // Update timer on screen once per second
      if (millis() - lastTimerScreenUpdate >= 1000) {
        lastTimerScreenUpdate = millis();
        drawMeetingAssistantUI();
      }
    }
  }

  // 3. Screen-Specific & Scheduled Synchronization Routines
  if (currentScreen == SCREEN_CONFIG) {
    dnsServer.processNextRequest();
    server.handleClient();

    // If a wifi connection is established when in the third screen, then go to first screen
    if (WiFi.status() == WL_CONNECTED) {
      lastKnownIP = WiFi.localIP().toString();
      lastNetworkAvailable = true;
      saveWiFiTxtConfig();
      saveSDClockConfig();
      previousScreen = SCREEN_CONFIG;
      currentScreen = SCREEN_CLOCK;
      applyScreenPowerMode(currentScreen);
      drawClockCalendarScreen();
      disableWiFi();
    }
  } else {
    static unsigned long lastRtcCheck = 0;
    unsigned long now = millis();

    // Check RTC triggers once per second (1 Hz)
    if (now - lastRtcCheck >= 1000) {
      lastRtcCheck = now;
      int curMin = rtc.getMinute();
      int curHour = rtc.getHour();

      // Weather Data Triggers: Minutes :02, :17, :32, and :47 of every hour
      bool isWeatherMinute = (curMin == 2 || curMin == 17 || curMin == 32 || curMin == 47);
      if (isWeatherMinute && curMin != lastWeatherSyncMinute) {
        lastWeatherSyncMinute = curMin;
        if (!isRecording && currentScreen != SCREEN_CONFIG) {
          setLowPowerClock(false);
          if (ensureWiFiConnected(6)) {
            applyLocalizationStrategy();
            if (strlen(last_synced_city) > 0 && strcmp(last_synced_city, city_name) != 0) {
              strncpy(last_synced_city, city_name, sizeof(last_synced_city) - 1);
            }
            updateOpenMeteo();
            resyncClockIfOffsetChanged();
            saveWiFiTxtConfig();
            saveSDClockConfig();
            if (currentScreen == SCREEN_CLOCK) {
              disableWiFi();
            }
          } else {
            if (currentScreen == SCREEN_CLOCK) {
              disableWiFi();
            }
          }
          if (currentScreen == SCREEN_CLOCK) {
            setLowPowerClock(true);
            drawClockCalendarScreen();
          }
        }
      }

      // Time Data Trigger: Daily at 00:00, provided the city parameter has not changed
      if (curHour == 0 && curMin == 0) {
        if (!timeSyncedToday) {
          timeSyncedToday = true;
          if (strcmp(last_synced_city, city_name) == 0 && !isRecording && currentScreen != SCREEN_CONFIG) {
            setLowPowerClock(false);
            if (ensureWiFiConnected(6)) {
              syncNTP();
              saveWiFiTxtConfig();
              saveSDClockConfig();
              if (currentScreen == SCREEN_CLOCK) {
                disableWiFi();
              }
            } else {
              if (currentScreen == SCREEN_CLOCK) {
                disableWiFi();
              }
            }
            if (currentScreen == SCREEN_CLOCK) {
              setLowPowerClock(true);
            }
          }
        }
      } else {
        timeSyncedToday = false;
      }

      // Minute Display Tick (Optimized for battery conservation)
      if (curMin != lastDrawnMinute) {
        lastDrawnMinute = curMin;

        if (currentScreen == SCREEN_CLOCK) {
          // Progressive offline back-off:
          // Try every minute for the first 3 minutes, then fall back to scheduled sync minutes (:02, :17, :32, :47)
          static uint8_t offlineFailures = 0;
          bool shouldRetry = (!lastNetworkAvailable || !lastWeatherSuccess);

          if (shouldRetry && offlineFailures >= 3) {
            bool isSyncMinute = (curMin == 2 || curMin == 17 || curMin == 32 || curMin == 47);
            if (!isSyncMinute) {
              shouldRetry = false; // Preserve battery power
            }
          }

          // The scheduled weather trigger already attempted Wi-Fi this minute; avoid a second connection attempt
          if (isWeatherMinute && lastWeatherSyncMinute == curMin) {
            shouldRetry = false;
          }

          if (shouldRetry && !isRecording) {
            setLowPowerClock(false);
            if (ensureWiFiConnected(6)) {
              applyLocalizationStrategy();
              if (strlen(last_synced_city) > 0 && strcmp(last_synced_city, city_name) != 0) {
                strncpy(last_synced_city, city_name, sizeof(last_synced_city) - 1);
              }
              updateOpenMeteo();
              resyncClockIfOffsetChanged();
              saveWiFiTxtConfig();
              saveSDClockConfig();
              disableWiFi();
              offlineFailures = 0; // Reset counter on success
            } else {
              disableWiFi();
              if (offlineFailures < 255) offlineFailures++;
            }
          }
          drawClockCalendarScreen();
          setLowPowerClock(true); // Guarantees 80 MHz sleep state
        } else if (currentScreen == SCREEN_MEETING && !isRecording) {
          drawMeetingAssistantUI(); // Updates the clock on the Meeting screen
        }
      }
    }
  }

  // Adaptive delay: 100ms idle delay on Clock Screen saves CPU cycles, 20ms elsewhere
  if (currentScreen == SCREEN_CLOCK && !isRecording) {
    delay(100);
    enterClockLightSleep(); // Screen 1: sleep between minute ticks instead of polling
  } else {
    delay(20);
  }
}
