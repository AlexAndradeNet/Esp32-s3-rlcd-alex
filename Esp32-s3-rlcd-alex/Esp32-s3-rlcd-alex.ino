#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "display_bsp.h"
#include "esp_sleep.h"
#include "font.h"
#include "secfont.h"
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
bool timeSyncedToday = false;
static volatile bool ntpPacketReceived = false;

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

// ---------- AUDIO RECORDING STATE & SD WATCHDOG ----------
uint8_t frame_buffer[AUDIO_FRAME_SIZE];
bool isRecording = false;
bool sdCardError = false; // Flagged if 0x107 or write failure occurs
FILE *currentMeetingFile = NULL;
char currentMeetingFileName[64] = "";
uint32_t totalRecordedBytes = 0;
unsigned long recordingStartTime = 0;
unsigned long lastTimerScreenUpdate = 0;

// ---------- CALENDAR DATA ----------
const int daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
bool isLeapYear(int yr) {
  return ((yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0));
}

// Forward Declarations
void safeStrCopy(char *dest, const char *src, size_t destSize);
void saveSDClockConfig();
void saveWiFiTxtConfig();
void saveAllConfigs();
bool readWiFiDatabase(DynamicJsonDocument &doc);
bool saveWiFiDatabase(DynamicJsonDocument &doc);
bool isInWiFiDatabase(const char* ssid);
void appendWiFiDatabase(const char* essid, const char* pass, const char* city, float lat, float lon, int gmt);
void applyLocalizationStrategy();
void updateOpenMeteo();
bool syncNTP();
bool ensureWiFiConnected(int maxWaitSec = 6);
void disableWiFi();
void transitionToScreen(AppScreen next);
void drawClockCalendarScreen();
void drawMeetingAssistantUI(const char* extraStatus = NULL);
void startWiFiConfiguratorAP();

// ================= STRING & POWER UTILITIES =================
void safeStrCopy(char *dest, const char *src, size_t destSize) {
  if (!dest || destSize == 0) {
    return;
  }
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

void setLowPowerClock(bool lowPower) {
  setCpuFrequencyMhz(lowPower ? 80 : 160);
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
  int raw = analogRead(BAT_ADC_PIN);
  float v_adc = (raw / 4095.0f) * 3.3f;
  v_bat = v_adc * 3.0f * 1.079f;
}

int getBatteryPercentage(float vbat) {
  if (vbat >= 4.15f) {
    return 100;
  }
  if (vbat <= 3.40f) {
    return 0;
  }
  int pct = (int)roundf(((vbat - 3.40f) / (4.15f - 3.40f)) * 100.0f);
  return constrain(pct, 0, 100);
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

bool shtc3_read_i2cbus(float &tempC, float &rh) {
  if (shtc3_dev_handle == NULL) {
    i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = SHTC3_ADDR,
      .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(I2cbus.user_i2c_handle, &dev_cfg, &shtc3_dev_handle) != ESP_OK) {
      return false;
    }
  }

  uint8_t cmd_wake[2] = {0x35, 0x17};
  if (i2c_master_transmit(shtc3_dev_handle, cmd_wake, 2, 50) != ESP_OK) {
    return false;
  }
  delay(1);

  uint8_t cmd_meas[2] = {0x78, 0x66};
  if (i2c_master_transmit(shtc3_dev_handle, cmd_meas, 2, 50) != ESP_OK) {
    return false;
  }
  delay(20);

  uint8_t d[6] = {0};
  if (i2c_master_receive(shtc3_dev_handle, d, 6, 50) != ESP_OK) {
    return false;
  }

  if (crc8(&d[0], 2) != d[2] || crc8(&d[3], 2) != d[5]) {
    return false;
  }

  uint16_t tRaw  = (uint16_t)d[0] << 8 | d[1];
  uint16_t rhRaw = (uint16_t)d[3] << 8 | d[4];

  tempC = -45.0f + 175.0f * (float)tRaw / 65535.0f;
  rh    = 100.0f * (float)rhRaw / 65535.0f;

  uint8_t cmd_sleep[2] = {0xB0, 0x98};
  i2c_master_transmit(shtc3_dev_handle, cmd_sleep, 2, 50);
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
      if (invert) {
        v ^= 0xFF;
      }
      int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        int x = x0 + bit;
        if (x >= W) {
          break;
        }
        if ((v & (0x80 >> bit)) != 0) {
          RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)y, ColorBlack);
        }
      }
    }
  }
  RlcdPort.RLCD_Display();
}

// ================= GEOLOCATION UTILITIES =================
bool obtainCoordinates(float &lat, float &lon) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // 1. Wi-Fi Positioning System (WPS) via detected router BSSIDs
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 3; i++) {
    HTTPClient http;
    http.begin("https://api.mylnikov.org/geolocation/wifi?v=1.1&data=open&bssid=" + WiFi.BSSIDstr(i));
    if (http.GET() == 200) {
      DynamicJsonDocument doc(1024);
      if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
        if (doc["result"] == 200 && doc["data"].containsKey("lat")) {
          lat = doc["data"]["lat"].as<float>();
          lon = doc["data"]["lon"].as<float>();
          http.end();
          return true;
        }
      }
    }
    http.end();
  }

  // 2. IP-API Fallback
  HTTPClient http;
  http.begin("http://ip-api.com/json/");
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      if (doc.containsKey("lat")) {
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

void queryBigDataCloudCity() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  HTTPClient http;
  http.begin("https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=" + String(latitude) +
             "&longitude=" + String(longitude) + "&localityLanguage=en");
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      const char* city = doc["city"];
      const char* locality = doc["locality"];
      if (city && city[0]) {
        safeStrCopy(city_name, city, sizeof(city_name));
      } else if (locality && locality[0]) {
        safeStrCopy(city_name, locality, sizeof(city_name));
      }
    }
  }
  http.end();
}

// ================= SD CARD & DATABASE PERSISTENCE =================
void saveWiFiTxtConfig() {
  FILE *f = fopen("/sdcard/config_wifi.txt", "w");
  if (f) {
    fprintf(f, "wifi_ssid=%s\nwifi_pass=%s\nmac_server=%s\n", wifi_ssid, wifi_pass, mac_server);
    fclose(f);
  } else {
    sdCardError = true;
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

void saveAllConfigs() {
  saveWiFiTxtConfig();
  saveSDClockConfig();
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
          safeStrCopy(city_name, val, sizeof(city_name));
        } else if (strcmp(key, "latitude") == 0) {
          safeStrCopy(latitude, val, sizeof(latitude));
        } else if (strcmp(key, "longitude") == 0) {
          safeStrCopy(longitude, val, sizeof(longitude));
        } else if (strcmp(key, "gmt") == 0 && val[0]) {
          gmt_offset = atoi(val);
        } else if (strcmp(key, "override") == 0) {
          safeStrCopy(location_override, val, sizeof(location_override));
        }
      }
      line = strtok(NULL, "\r\n");
    }
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
          safeStrCopy(wifi_ssid, val, sizeof(wifi_ssid));
        } else if (strcmp(key, "wifi_pass") == 0) {
          safeStrCopy(wifi_pass, val, sizeof(wifi_pass));
        } else if (strcmp(key, "mac_server") == 0) {
          safeStrCopy(mac_server, val, sizeof(mac_server));
        }
      }
      line = strtok(NULL, "\r\n");
    }
  } else {
    sdCardError = true;
  }
  loadSDClockConfig();
}

bool readWiFiDatabase(DynamicJsonDocument &doc) {
  FILE *f = fopen("/sdcard/wifi_database.json", "r");
  if (!f) {
    return false;
  }
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

bool saveWiFiDatabase(DynamicJsonDocument &doc) {
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
  if (!ssid || !ssid[0]) {
    return false;
  }
  DynamicJsonDocument doc(8192);
  if (!readWiFiDatabase(doc) || !doc.is<JsonArray>()) {
    return false;
  }
  for (JsonObject entry : doc.as<JsonArray>()) {
    if (entry["essid"] && strcmp(entry["essid"], ssid) == 0) {
      return true;
    }
  }
  return false;
}

void appendWiFiDatabase(const char* essid, const char* pass, const char* city, float lat, float lon, int gmt) {
  if (!essid || !essid[0]) {
    return;
  }
  DynamicJsonDocument doc(8192);
  bool loaded = readWiFiDatabase(doc);
  JsonArray arr = (loaded && doc.is<JsonArray>()) ? doc.as<JsonArray>() : doc.to<JsonArray>();

  for (JsonObject obj : arr) {
    if (obj["essid"] && strcmp(obj["essid"], essid) == 0) {
      obj["password"] = pass ? pass : "";
      obj["city"] = city ? city : "";
      obj["latitude"] = lat;
      obj["longitude"] = lon;
      obj["gmt"] = gmt;
      saveWiFiDatabase(doc);
      return;
    }
  }

  JsonObject entry = arr.createNestedObject();
  entry["essid"] = essid;
  entry["password"] = pass ? pass : "";
  entry["city"] = city ? city : "";
  entry["latitude"] = lat;
  entry["longitude"] = lon;
  entry["gmt"] = gmt;
  saveWiFiDatabase(doc);
}

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
    if (fname.endsWith(".wav") && fname.length() >= 19) {
      struct tm ftime = {0};
      int yr, mon, dy, hr, mn, sc;
      if (sscanf(fname.c_str(), "%4d%2d%2d_%2d%2d%2d.wav", &yr, &mon, &dy, &hr, &mn, &sc) == 6) {
        ftime.tm_year = yr - 1900;
        ftime.tm_mon  = mon - 1;
        ftime.tm_mday = dy;
        ftime.tm_hour = hr;
        ftime.tm_min  = mn;
        ftime.tm_sec  = sc;
        if (difftime(now, mktime(&ftime)) > 172800) {
          unlink(("/sdcard/meetings/" + fname).c_str());
        }
      }
    }
  }
  closedir(dir);
}

// ================= LOCALIZATION STRATEGY =================
void handleFirstTimeConnection(const char* ssid, const char* pass) {
  float lat = (float)atof(latitude);
  float lon = (float)atof(longitude);
  if (obtainCoordinates(lat, lon)) {
    dtostrf(lat, 6, 4, latitude);
    dtostrf(lon, 6, 4, longitude);
  }

  updateOpenMeteo();
  queryBigDataCloudCity();
  safeStrCopy(last_synced_city, city_name, sizeof(last_synced_city));

  safeStrCopy(wifi_ssid, ssid, sizeof(wifi_ssid));
  safeStrCopy(wifi_pass, pass, sizeof(wifi_pass));
  saveAllConfigs();
  appendWiFiDatabase(ssid, pass, city_name, (float)atof(latitude), (float)atof(longitude), gmt_offset);
}

void applyLocalizationStrategy() {
  if (WiFi.status() != WL_CONNECTED || strcasecmp(location_override, "true") == 0) {
    loadSDClockConfig();
    return;
  }

  String currentSSID = WiFi.SSID();
  DynamicJsonDocument doc(8192);
  bool foundInDb = false;

  if (readWiFiDatabase(doc) && doc.is<JsonArray>()) {
    for (JsonObject entry : doc.as<JsonArray>()) {
      if (entry["essid"] && currentSSID == entry["essid"].as<const char*>()) {
        if (entry["city"] && entry["city"].as<const char*>()[0]) {
          safeStrCopy(city_name, entry["city"], sizeof(city_name));
        }
        if (entry.containsKey("latitude") && !entry["latitude"].isNull()) {
          dtostrf(entry["latitude"].as<float>(), 6, 4, latitude);
        }
        if (entry.containsKey("longitude") && !entry["longitude"].isNull()) {
          dtostrf(entry["longitude"].as<float>(), 6, 4, longitude);
        }
        if (entry.containsKey("gmt") && !entry["gmt"].isNull()) {
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

// ================= NTP & WEATHER =================
void timeSyncNotificationCallback(struct timeval *tv) {
  ntpPacketReceived = true;
}

bool syncNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

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
  if (!ntpPacketReceived) {
    return false;
  }

  struct tm ti;
  if (!getLocalTime(&ti, 2000)) {
    return false;
  }

  rtc.setDate(ti.tm_wday, ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
  rtc.setTime(ti.tm_hour, ti.tm_min, ti.tm_sec);
  return true;
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
    DynamicJsonDocument doc(12288);
    deserializeJson(doc, http.getString());

    // Extract timezone abbreviation and calculate GMT offset
    if (doc.containsKey("timezone_abbreviation") && !doc["timezone_abbreviation"].isNull()) {
      const char* tz_abbr = doc["timezone_abbreviation"].as<const char*>();
      const char* p = strstr(tz_abbr, "GMT");
      gmt_offset = p ? atoi(p + 3) : atoi(tz_abbr);
    }
    if (doc.containsKey("utc_offset_seconds")) {
      gmt_offset = doc["utc_offset_seconds"].as<int>() / 3600;
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

    const char *label = "Fair";
    if (weather_code <= 1) {
      label = "Clear";
    } else if (weather_code <= 3) {
      label = "Cloudy";
    } else if (weather_code <= 48) {
      label = "Foggy";
    } else if (weather_code <= 67 || (weather_code >= 80 && weather_code <= 82)) {
      label = "Rainy";
    } else if ((weather_code >= 71 && weather_code <= 77) || weather_code == 85 || weather_code == 86) {
      label = "Snow";
    } else if (weather_code >= 95) {
      label = "Storm";
    }
    safeStrCopy(weather_label, label, sizeof(weather_label));

    rain_start_hour = -1;
    rain_stop_hour = -1;
    JsonArray hourly_precip = doc["hourly"]["precipitation"];
    for (int h_idx = 0; h_idx < min(24, (int)hourly_precip.size()); h_idx++) {
      if (hourly_precip[h_idx].as<float>() > 0.1f) {
        if (rain_start_hour == -1) {
          rain_start_hour = h_idx;
        }
        rain_stop_hour = h_idx + 1;
      }
    }
    lastWeatherSuccess = true;
  } else {
    lastWeatherSuccess = false;
  }
  http.end();
}

// ================= POWER-SAVING WI-FI MANAGERS =================
bool ensureWiFiConnected(int maxWaitSec) {
  setLowPowerClock(false);

  if (WiFi.status() == WL_CONNECTED) {
    lastKnownIP = WiFi.localIP().toString();
    lastNetworkAvailable = true;
    return true;
  }

  WiFi.mode(WIFI_STA);
  bool connected = false;

  // 1. Primary Attempt from .txt
  if (wifi_ssid[0]) {
    WiFi.begin(wifi_ssid, wifi_pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < (maxWaitSec * 2)) {
      delay(500);
      attempts++;
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  // 2. Database Fallback Scan
  if (!connected) {
    WiFi.disconnect(true);
    delay(200);
    int n = WiFi.scanNetworks();
    if (n > 0) {
      DynamicJsonDocument dbDoc(8192);
      if (readWiFiDatabase(dbDoc) && dbDoc.is<JsonArray>()) {
        for (int i = 0; i < n && !connected; i++) {
          String scannedSSID = WiFi.SSID(i);
          for (JsonObject entry : dbDoc.as<JsonArray>()) {
            const char* db_essid = entry["essid"];
            if (db_essid && db_essid[0] && scannedSSID == db_essid) {
              const char* db_pass = entry["password"];
              WiFi.begin(db_essid, db_pass ? db_pass : "");
              int waitCount = 0;
              while (WiFi.status() != WL_CONNECTED && waitCount < (maxWaitSec * 2)) {
                delay(500);
                waitCount++;
              }
              if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                safeStrCopy(wifi_ssid, db_essid, sizeof(wifi_ssid));
                safeStrCopy(wifi_pass, db_pass ? db_pass : "", sizeof(wifi_pass));
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
    String activeSSID = WiFi.SSID();

    if (!isInWiFiDatabase(activeSSID.c_str()) && currentScreen != SCREEN_MEETING) {
      handleFirstTimeConnection(activeSSID.c_str(), wifi_pass);
    }

    if (currentScreen != SCREEN_MEETING) {
      bool netChanged = (activeSSID.length() > 0 && strcmp(current_network, activeSSID.c_str()) != 0);
      if (netChanged) {
        safeStrCopy(current_network, activeSSID.c_str(), sizeof(current_network));
      }

      applyLocalizationStrategy();
      if (last_synced_city[0] && strcmp(last_synced_city, city_name) != 0) {
        safeStrCopy(last_synced_city, city_name, sizeof(last_synced_city));
      }
      if (netChanged) {
        updateOpenMeteo();
      }
    } else if (activeSSID.length() > 0) {
      safeStrCopy(current_network, activeSSID.c_str(), sizeof(current_network));
    }

    saveAllConfigs();
    return true;
  } else {
    lastKnownIP = "No Wi-Fi";
    lastNetworkAvailable = false;
    return false;
  }
}

bool syncWeather() {
  setLowPowerClock(false);
  bool success = false;
  if (ensureWiFiConnected(6)) {
    updateOpenMeteo();
    saveSDClockConfig();
    success = lastWeatherSuccess;
  }
  if (currentScreen == SCREEN_CLOCK) {
    disableWiFi();
  }
  return success;
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

  if (cur_mon < 1 || cur_mon > 12) {
    cur_mon = 1;
  }
  if (cur_year < 2000) {
    cur_year = 2026;
  }
  if (cur_day < 1 || cur_day > 31) {
    cur_day = 1;
  }

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
  canvas.setCursor((bX + 3) + (40 - (int)bw) / 2 - bx1, (bY + 3) + (16 - (int)bh) / 2 - by1);
  canvas.print(batStr);

  canvas.fillRect(10, 95, 380, 2, 1);

  // Date Header
  char dateHeader[32];
  sprintf(dateHeader, "%02d/%02d/%04d", cur_day, cur_mon, cur_year);
  canvas.setCursor(10, 114);
  canvas.print(dateHeader);

  // Weekdays
  const char *wLabels[] = {"S", "M", "T", "W", "T", "F", "S"};
  int calX = 10, calY = 136, colW = 24, rowH = 20;

  for (int c = 0; c < 7; c++) {
    if (c == cur_wday) {
      canvas.fillRoundRect(calX + (c * colW) - 1, calY - 13, 21, 18, 3, 1);
      canvas.setTextColor(0);
    } else {
      canvas.setTextColor(1);
    }
    canvas.setCursor(calX + (c * colW) + 4, calY);
    canvas.print(wLabels[c]);
  }
  canvas.setTextColor(1);

  // Calendar Days
  struct tm firstDayTm = {0};
  firstDayTm.tm_year = cur_year - 1900;
  firstDayTm.tm_mon  = cur_mon - 1;
  firstDayTm.tm_mday = 1;
  mktime(&firstDayTm);

  int totalDays = daysPerMonth[cur_mon - 1];
  if (cur_mon == 2 && isLeapYear(cur_year)) {
    totalDays = 29;
  }

  int dCount = 1;
  for (int r = 0; r < 6; r++) {
    for (int c = 0; c < 7; c++) {
      if ((r == 0 && c < firstDayTm.tm_wday) || dCount > totalDays) {
        continue;
      }
      int posX = calX + (c * colW);
      int posY = calY + 20 + (r * rowH);

      if (dCount == cur_day) {
        canvas.fillRoundRect(posX - 1, posY - 13, 21, 18, 3, 1);
        canvas.setTextColor(0);
      } else {
        canvas.setTextColor(1);
      }
      canvas.setCursor(posX, posY);
      canvas.printf("%2d", dCount++);
    }
  }

  // Divider & Weather Panel
  canvas.fillRect(182, 102, 2, 180, 1);
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

  canvas.setCursor(195, 222);
  if (rain_start_hour != -1) {
    canvas.printf("Rain: %02d:00-%02d:00", rain_start_hour, rain_stop_hour);
  } else {
    canvas.print("Rain: None expected");
  }

  canvas.setCursor(195, 240);
  canvas.printf("Precip Sum: %.1f mm", precip_sum);

  shtc3_read_i2cbus(temp_shtc3, hum_shtc3);
  canvas.setCursor(195, 262);
  canvas.printf("Local: %dC | %d%% RH", (int)round(temp_shtc3), (int)round(hum_shtc3));

  canvas.setCursor(10, 285);
  canvas.print(lastKnownIP);

  pushCanvasToRLCD(false);
}

// ================= SCREEN 2: MEETING ASSISTANT =================
void writeWavHeader(uint8_t* buffer, uint32_t dataSize) {
  memcpy(buffer, "RIFF", 4);
  uint32_t chunkSize = dataSize + 36;
  memcpy(buffer + 4, &chunkSize, 4);
  memcpy(buffer + 8, "WAVEfmt ", 8);

  uint32_t subChunk1Size = 16;
  uint16_t audioFormat = 1;                                      // 1 = WAVE_FORMAT_PCM (Uncompressed 16-bit linear)
  uint16_t numChannels = CHANNELS;                               // 1 (Mono)
  uint32_t sampleRate = SAMPLE_RATE;                             // 16000 Hz
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);        // 2 bytes per sample
  uint32_t byteRate = sampleRate * blockAlign;                   // 32000 bytes/sec
  uint16_t bitsPerSample = BITS_PER_SAMPLE;                      // 16-bit PCM

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

void streamFileToBackend(const char* filepath) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  FILE *f = fopen(filepath, "rb");
  if (!f) {
    sdCardError = true;
    return;
  }

  fseek(f, 0, SEEK_END);
  long fileSize = ftell(f);
  fseek(f, 0, SEEK_SET);

  String serverStr = String(mac_server);
  serverStr.replace("http://", "");
  int colonIdx = serverStr.indexOf(':');
  String host = (colonIdx != -1) ? serverStr.substring(0, colonIdx) : serverStr;
  int port = (colonIdx != -1) ? serverStr.substring(colonIdx + 1).toInt() : 5000;

  WiFiClient client;
  if (!client.connect(host.c_str(), port)) {
    fclose(f);
    return;
  }

  String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
  const char *justName = strrchr(filepath, '/');
  justName = justName ? justName + 1 : filepath;

  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + String(justName) + "\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  long totalLen = head.length() + fileSize + tail.length();

  client.print("POST /api/transcribe HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: multipart/form-data; boundary=" + boundary +
               "\r\nContent-Length: " + String(totalLen) + "\r\nConnection: close\r\n\r\n" + head);

  uint8_t sendBuf[2048];
  while (fileSize > 0) {
    size_t toRead = (fileSize > sizeof(sendBuf)) ? sizeof(sendBuf) : fileSize;
    size_t bytesRead = fread(sendBuf, 1, toRead, f);
    if (bytesRead > 0) {
      client.write(sendBuf, bytesRead);
      fileSize -= bytesRead;
    } else {
      break;
    }
  }
  client.print(tail);
  fclose(f);

  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 5000) {
    if (client.available()) {
      break;
    }
    delay(10);
  }
  client.stop();
}

void drawMeetingAssistantUI(const char* extraStatus) {
  canvas.fillScreen(0);
  canvas.setTextColor(1);
  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setCursor(20, 45);
  canvas.print("Meeting Assistant");

  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(20, 75);
  canvas.print(lastNetworkAvailable ? "Network: Ready (Cloud Sync)" : "Network: Offline (SD Card Only)");
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
    canvas.print("The SD card was not read or\ntimed out during operation.\nPlease restart the device.");
    canvas.setTextColor(1);
    pushCanvasToRLCD(false);
    return;
  }

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

    canvas.setFont(&FreeSansBold9pt7b);
    canvas.setCursor(20, 235);
    if (extraStatus) {
      canvas.print(extraStatus);
    } else if (currentMeetingFileName[0]) {
      canvas.print("Last: ");
      const char *shortName = strrchr(currentMeetingFileName, '/');
      canvas.print(shortName ? shortName + 1 : currentMeetingFileName);
    }
  }
  pushCanvasToRLCD(false);
}

void startMeetingRecording() {
  if (sdCardError) {
    return;
  }

  sprintf(currentMeetingFileName, "/sdcard/meetings/%04d%02d%02d_%02d%02d%02d.wav",
          rtc.getYear(), rtc.getMonth(), rtc.getDay(), rtc.getHour(), rtc.getMinute(), rtc.getSecond());

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

  totalRecordedBytes = 0;
  recordingStartTime = millis();
  isRecording = true;
  drawMeetingAssistantUI();
}

void stopMeetingRecording() {
  isRecording = false;

  if (currentMeetingFile) {
    if (fseek(currentMeetingFile, 0, SEEK_SET) == 0) {
      uint8_t header[44];
      writeWavHeader(header, totalRecordedBytes);
      if (fwrite(header, 1, 44, currentMeetingFile) < 44) {
        sdCardError = true;
      }
    } else {
      sdCardError = true;
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
  setLowPowerClock(false);

  if (ensureWiFiConnected(6)) {
    drawMeetingAssistantUI("Syncing to Mac...");
    streamFileToBackend(currentMeetingFileName);
    drawMeetingAssistantUI("Saved & Transmitted!");
  } else {
    drawMeetingAssistantUI("Saved to SD (Offline)");
  }
  disableWiFi();
}

// ================= SCREEN 3: QR CONFIG =================
static void qrcode_canvas_draw(esp_qrcode_handle_t qrcode) {
  int size = esp_qrcode_get_size(qrcode);
  int scale = max(1, 155 / size);
  int startX = (190 - (size * scale)) / 2;

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qrcode, x, y)) {
        canvas.fillRect(startX + (x * scale), 60 + (y * scale), scale, scale, 1);
      }
    }
  }
}

void startWiFiConfiguratorAP() {
  setLowPowerClock(false);

  IPAddress apIP(192, 168, 5, 1);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  dnsServer.start(53, "*", apIP);

  server.on("/", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'/></head><body>"
                  "<h2>WiFi Wizard</h2><form method='POST' action='/save'>"
                  "<label>SSID:</label><br><select name='ssid'>";
    for (int i = 0; i < n; i++) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
    }
    html += "</select><br><br><label>Password:</label><br><input type='password' name='pass'/><br><br>"
            "<input type='submit' value='Connect & Save'/></form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    safeStrCopy(wifi_ssid, server.arg("ssid").c_str(), sizeof(wifi_ssid));
    safeStrCopy(wifi_pass, server.arg("pass").c_str(), sizeof(wifi_pass));
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
      handleFirstTimeConnection(wifi_ssid, wifi_pass);
      lastKnownIP = WiFi.localIP().toString();
      lastNetworkAvailable = true;
      saveAllConfigs();
      disableWiFi();
      transitionToScreen(SCREEN_CLOCK);
    } else {
      lastKnownIP = "No Wi-Fi";
      lastNetworkAvailable = false;
      startWiFiConfiguratorAP();
    }
  });

  server.begin();

  String defaultSSID = WiFi.softAPSSID();
  if (defaultSSID.length() == 0) {
    defaultSSID = "Intelligent-Clock";
  }

  canvas.fillScreen(0);
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrcode_canvas_draw;
  cfg.max_qrcode_version = 5;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, ("WIFI:S:" + defaultSSID + ";T:nopass;;").c_str());

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

// ================= UNIFIED SCREEN STATE MANAGER =================
void transitionToScreen(AppScreen newScreen) {
  previousScreen = currentScreen;
  currentScreen = newScreen;

  // Cleanup outgoing screen
  if (previousScreen == SCREEN_CONFIG) {
    server.close();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    disableWiFi();
  } else if (previousScreen == SCREEN_MEETING) {
    if (isRecording) {
      stopMeetingRecording();
    }
    digitalWrite(PA_CTRL, LOW);
  }

  // Setup incoming screen
  if (currentScreen == SCREEN_CLOCK) {
    setLowPowerClock(true);
    drawClockCalendarScreen();
    disableWiFi();
  } else if (currentScreen == SCREEN_MEETING) {
    setLowPowerClock(false);
    digitalWrite(PA_CTRL, HIGH);
    if (!codecport) {
      codecport = new CodecPort(I2cbus, "S3_RLCD_4_2");
      codecport->CodecPort_SetInfo("es8311 & es7210", 1, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);
      codecport->CodecPort_SetMicGain(35);
    }
    ensureWiFiConnected(6);
    disableWiFi();
    drawMeetingAssistantUI();
  } else if (currentScreen == SCREEN_CONFIG) {
    startWiFiConfiguratorAP();
  }
}

// ================= ARDUINO LIFECYCLE =================
void setup() {
  Serial.begin(115200);
  delay(200);

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
  cleanOldRecordings();

  // Boot Network Handshake
  if (ensureWiFiConnected(8)) {
    safeStrCopy(current_network, WiFi.SSID().c_str(), sizeof(current_network));
    applyLocalizationStrategy();
    updateOpenMeteo();
    syncNTP();
    safeStrCopy(last_synced_city, city_name, sizeof(last_synced_city));
    saveAllConfigs();
    disableWiFi();
  } else {
    lastWeatherSuccess = false;
    applyLocalizationStrategy();
    disableWiFi();
  }

  transitionToScreen(SCREEN_CLOCK);
  lastDrawnMinute = rtc.getMinute();
}

void loop() {
  // 1. Screen Switch Button (GPIO 18 - Left)
  bool reading = digitalRead(BUTTON_PIN);
  if (reading == LOW && lastButtonState == HIGH && (millis() - lastButtonPress > debounceTime)) {
    lastButtonPress = millis();

    AppScreen nextScreen = SCREEN_CLOCK;
    if (currentScreen == SCREEN_CLOCK) {
      nextScreen = SCREEN_MEETING;
    } else if (currentScreen == SCREEN_MEETING) {
      nextScreen = lastNetworkAvailable ? SCREEN_CLOCK : SCREEN_CONFIG;
    }
    transitionToScreen(nextScreen);
  }
  lastButtonState = reading;

  // 2. Meeting Recording Toggle (GPIO 0 - BOOT)
  if (currentScreen == SCREEN_MEETING) {
    bool recBtn = digitalRead(RECORD_BUTTON_PIN);
    if (recBtn == LOW && lastRecButtonState == HIGH && (millis() - lastRecButtonPress > 350)) {
      lastRecButtonPress = millis();
      if (!isRecording) {
        startMeetingRecording();
      } else {
        stopMeetingRecording();
      }
    }
    lastRecButtonState = recBtn;

    if (isRecording) {
      if (codecport) {
        codecport->CodecPort_EchoRead(frame_buffer, AUDIO_FRAME_SIZE);
        if (currentMeetingFile) {
          size_t written = fwrite(frame_buffer, 1, AUDIO_FRAME_SIZE, currentMeetingFile);
          if (written < AUDIO_FRAME_SIZE || ferror(currentMeetingFile)) {
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

  // 3. Screen 3 Listener
  if (currentScreen == SCREEN_CONFIG) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (WiFi.status() == WL_CONNECTED) {
      lastKnownIP = WiFi.localIP().toString();
      lastNetworkAvailable = true;
      saveAllConfigs();
      transitionToScreen(SCREEN_CLOCK);
    }
  } 
  // 4. Screen 1 Clock & Periodic Routines
  else {
    static unsigned long lastRtcCheck = 0;
    unsigned long now = millis();

    // Check RTC triggers once per second (1 Hz)
    if (now - lastRtcCheck >= 1000) {
      lastRtcCheck = now;
      int curMin = rtc.getMinute();
      int curHour = rtc.getHour();

      // Daily 00:00 Time Sync
      if (curHour == 0 && curMin == 0) {
        if (!timeSyncedToday) {
          timeSyncedToday = true;
          if (strcmp(last_synced_city, city_name) == 0 && !isRecording && currentScreen != SCREEN_CONFIG) {
            setLowPowerClock(false);
            if (ensureWiFiConnected(6)) {
              syncNTP();
              saveAllConfigs();
            }
            if (currentScreen == SCREEN_CLOCK) {
              disableWiFi();
              setLowPowerClock(true);
            }
          }
        }
      } else {
        timeSyncedToday = false;
      }

      // Minute Display Tick & Battery-Throttled Weather Sync
      if (currentScreen == SCREEN_CLOCK && curMin != lastDrawnMinute) {
        lastDrawnMinute = curMin;

        bool isWeatherMinute = (curMin % 15 == 2);
        static uint8_t offlineFailures = 0;
        bool shouldSync = isWeatherMinute || (!lastNetworkAvailable || !lastWeatherSuccess);

        // After 3 failed minutes, back off to scheduled weather minutes only
        if (shouldSync && !isWeatherMinute && offlineFailures >= 3) {
          shouldSync = false;
        }

        if (shouldSync && !isRecording) {
          if (syncWeather()) {
            offlineFailures = 0;
          } else if (offlineFailures < 255) {
            offlineFailures++;
          }
        }

        drawClockCalendarScreen();
        setLowPowerClock(true); // Guarantees 80 MHz sleep state
      }
    }
  }

  // Adaptive delay: 100ms idle delay on Clock Screen saves CPU cycles, 20ms elsewhere
  delay((currentScreen == SCREEN_CLOCK && !isRecording) ? 100 : 20);
}