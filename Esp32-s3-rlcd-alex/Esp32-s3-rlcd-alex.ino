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
unsigned long lastWeatherSync = 0;
const unsigned long WEATHER_SYNC_INTERVAL = 900000UL; // 15 mins

// ---------- BUFFERS & CONFIGS ----------
char config_buffer[2048] = {0};
char wifi_ssid[64] = "";
char wifi_pass[64] = "";
char mac_server[128] = "http://192.168.1.100:5000";
char city_name[32] = "Medellin";
char latitude[16] = "6.2442";
char longitude[16] = "-75.5812";
int gmt_offset = -5;

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

// ================= BATTERY LOGIC =================
void readBattery() {
  int raw = analogRead(BAT_ADC_PIN);
  float v_adc = (raw / 4095.0f) * 3.3f;
  v_bat = v_adc * 3.0f * 1.079f;
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
  if (i2c_master_transmit(shtc3_dev_handle, cmd_wake, 2, 50) != ESP_OK) return false;
  delay(1);

  uint8_t cmd_meas[2] = {0x78, 0x66};
  if (i2c_master_transmit(shtc3_dev_handle, cmd_meas, 2, 50) != ESP_OK) return false;
  delay(20);

  uint8_t d[6] = {0};
  if (i2c_master_receive(shtc3_dev_handle, d, 6, 50) != ESP_OK) return false;

  if (crc8(&d[0], 2) != d[2]) return false;
  if (crc8(&d[3], 2) != d[5]) return false;

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
        if (strcmp(key, "wifi_ssid") == 0) strncpy(wifi_ssid, val, sizeof(wifi_ssid) - 1);
        else if (strcmp(key, "wifi_pass") == 0) strncpy(wifi_pass, val, sizeof(wifi_pass) - 1);
        else if (strcmp(key, "mac_server") == 0) strncpy(mac_server, val, sizeof(mac_server) - 1);
      }
      line = strtok(NULL, "\r\n");
    }
  } else {
    sdCardError = true;
  }

  memset(config_buffer, 0, sizeof(config_buffer));
  if (sdcardPort->SDPort_ReadFile("/sdcard/config_clock.txt", (uint8_t *)config_buffer, NULL) == 0) {
    char *line = strtok(config_buffer, "\r\n");
    while (line != NULL) {
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "city") == 0) strncpy(city_name, val, sizeof(city_name) - 1);
        else if (strcmp(key, "latitude") == 0) strncpy(latitude, val, sizeof(latitude) - 1);
        else if (strcmp(key, "longitude") == 0) strncpy(longitude, val, sizeof(longitude) - 1);
        else if (strcmp(key, "gmt") == 0) gmt_offset = atoi(val);
      }
      line = strtok(NULL, "\r\n");
    }
  }
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
      struct tm ftime;
      memset(&ftime, 0, sizeof(ftime));
      int yr, mon, dy, hr, mn, sc;
      if (sscanf(fname.c_str(), "%4d%2d%2d_%2d%2d%2d.wav", &yr, &mon, &dy, &hr, &mn, &sc) == 6) {
        ftime.tm_year = yr - 1900;
        ftime.tm_mon  = mon - 1;
        ftime.tm_mday = dy;
        ftime.tm_hour = hr;
        ftime.tm_min  = mn;
        ftime.tm_sec  = sc;
        time_t f_epoch = mktime(&ftime);
        if (difftime(now, f_epoch) > 172800) { 
          String fullPath = String("/sdcard/meetings/") + fname;
          unlink(fullPath.c_str());
        }
      }
    }
  }
  closedir(dir);
}

// ================= NETWORK & NTP =================
bool syncNTP() {
  configTime(gmt_offset * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm ti;
  int retry = 0;
  while (!getLocalTime(&ti) && retry < 20) {
    delay(500);
    retry++;
  }
  if (retry >= 20) return false;
  rtc.setDate(ti.tm_wday, ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
  rtc.setTime(ti.tm_hour, ti.tm_min, ti.tm_sec);
  return true;
}

void updateOpenMeteo() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + latitude +
               "&longitude=" + longitude +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code" +
               "&hourly=precipitation_probability,precipitation" +
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max" +
               "&timezone=auto";
  http.begin(url);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(12288);
    deserializeJson(doc, http.getString());
    
    cur_temp = round(doc["current"]["temperature_2m"].as<float>());
    cur_humidity = round(doc["current"]["relative_humidity_2m"].as<float>());
    feels_like = round(doc["current"]["apparent_temperature"].as<float>());
    weather_code = doc["current"]["weather_code"].as<int>();

    max_temp = round(doc["daily"]["temperature_2m_max"][0].as<float>());
    min_temp = round(doc["daily"]["temperature_2m_min"][0].as<float>());
    precip_sum = doc["daily"]["precipitation_sum"][0].as<float>();
    rain_prob_max = round(doc["daily"]["precipitation_probability_max"][0].as<float>());

    will_rain = (rain_prob_max > 30 || precip_sum > 0.5f);

    if (weather_code == 0) strcpy(weather_label, "Clear");
    else if (weather_code <= 3) strcpy(weather_label, "Cloudy");
    else if (weather_code <= 48) strcpy(weather_label, "Foggy");
    else if (weather_code <= 67) strcpy(weather_label, "Rainy");
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

  readBattery();
  int bX = 345, bY = 12;
  canvas.fillRect(bX, bY, 44, 22, 1);
  canvas.fillRect(bX + 3, bY + 3, 38, 16, 0);
  canvas.fillRect(bX + 44, bY + 6, 4, 10, 1);
  for (int i = 0; i < getBatteryBars(v_bat); i++) {
    canvas.fillRect(bX + 5 + (i * 7), bY + 5, 5, 12, 1);
  }

  canvas.fillRect(10, 95, 380, 2, 1);

  canvas.setFont(&FreeSansBold9pt7b);
  char dateHeader[32];
  sprintf(dateHeader, "%02d/%02d/%04d", cur_day, cur_mon, cur_year);
  canvas.setCursor(10, 114);
  canvas.print(dateHeader);

  const char* wLabels[] = {"S", "M", "T", "W", "T", "F", "S"};
  int calX = 10, calY = 130, colW = 24, rowH = 18;
  for (int c = 0; c < 7; c++) {
    if (c == cur_wday) {
      canvas.fillRect(calX + (c * colW) - 2, calY - 12, colW, 15, 1);
      canvas.setTextColor(0);
    } else {
      canvas.setTextColor(1);
    }
    canvas.setCursor(calX + (c * colW) + 4, calY);
    canvas.print(wLabels[c]);
  }
  canvas.setTextColor(1);

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
      int posY = calY + 16 + (r * rowH);

      if (dCount == cur_day) {
        canvas.fillRect(posX - 2, posY - 12, colW, 14, 1);
        canvas.setTextColor(0);
      } else {
        canvas.setTextColor(1);
      }
      canvas.setCursor(posX, posY);
      canvas.printf("%2d", dCount);
      dCount++;
    }
  }

  canvas.fillRect(182, 102, 2, 180, 1);

  canvas.setTextColor(1);
  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setCursor(195, 130);
  canvas.printf("%dC", cur_temp);

  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(265, 130);
  canvas.printf("%s", weather_label);

  canvas.setCursor(195, 150);
  canvas.printf("Min/Max: %d / %dC", min_temp, max_temp);

  canvas.setCursor(195, 168);
  canvas.printf("Feels: %dC | Hum: %d%%", feels_like, cur_humidity);

  canvas.setCursor(195, 186);
  canvas.printf("Rain?: %s (%d%%)", will_rain ? "Yes" : "No", rain_prob_max);

  if (rain_start_hour != -1) {
    canvas.setCursor(195, 204);
    canvas.printf("Rain: %02d:00-%02d:00", rain_start_hour, rain_stop_hour);
  } else {
    canvas.setCursor(195, 204);
    canvas.printf("Rain: None expected");
  }

  canvas.setCursor(195, 222);
  canvas.printf("Precip Sum: %.1f mm", precip_sum);

  shtc3_read_i2cbus(temp_shtc3, hum_shtc3);
  canvas.setCursor(195, 245);
  canvas.printf("Local: %dC | %d%% RH", (int)round(temp_shtc3), (int)round(hum_shtc3));

  canvas.setCursor(195, 285);
  if (WiFi.status() == WL_CONNECTED) {
    canvas.print(WiFi.localIP().toString());
  } else {
    canvas.print("No Wi-Fi");
  }

  pushCanvasToRLCD(false);
}

// ================= SCREEN 2: MEETING ASSISTANT =================
void writeWavHeader(uint8_t* buffer, uint32_t dataSize) {
  memcpy(buffer, "RIFF", 4);
  uint32_t chunkSize = dataSize + 36;
  memcpy(buffer + 4, &chunkSize, 4);
  memcpy(buffer + 8, "WAVEfmt ", 8);

  uint32_t subChunk1Size = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = CHANNELS;
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = sampleRate * CHANNELS * 2;
  uint16_t blockAlign = CHANNELS * 2;
  uint16_t bitsPerSample = 16;

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
  if (WiFi.status() != WL_CONNECTED) return;
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
  const char* justName = strrchr(filepath, '/');
  justName = justName ? justName + 1 : filepath;

  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + String(justName) + "\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  long totalLen = head.length() + fileSize + tail.length();

  client.print("POST /api/transcribe HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(totalLen) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(head);

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
    if (client.available()) break;
    delay(10);
  }
  client.stop();
}

void drawMeetingAssistantUI(const char* extraStatus = NULL) {
  canvas.fillScreen(0);
  canvas.setTextColor(1);
  
  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setCursor(20, 45);
  canvas.print("Meeting Assistant");

  bool isOnline = (WiFi.status() == WL_CONNECTED);
  canvas.setFont(&FreeSansBold9pt7b);
  canvas.setCursor(20, 75);
  if (isOnline) {
    canvas.print("Network: Online (Cloud Sync)");
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
      canvas.print("Last: ");
      const char* shortName = strrchr(currentMeetingFileName, '/');
      canvas.print(shortName ? shortName + 1 : currentMeetingFileName);
    }
  }

  pushCanvasToRLCD(false);
}

void startMeetingRecording() {
  if (sdCardError) return;

  sprintf(currentMeetingFileName, "/sdcard/meetings/%04d%02d%02d_%02d%02d%02d.wav",
          rtc.getYear(), rtc.getMonth(), rtc.getDay(),
          rtc.getHour(), rtc.getMinute(), rtc.getSecond());

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

  bool isOnline = (WiFi.status() == WL_CONNECTED);
  if (isOnline) {
    drawMeetingAssistantUI("Syncing to Mac...");
    streamFileToBackend(currentMeetingFileName);
    drawMeetingAssistantUI("Saved & Transmitted!");
  } else {
    drawMeetingAssistantUI("Saved to SD (Offline)");
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

void queryIPGeolocation() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://ip-api.com/json/");
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());
    strncpy(city_name, doc["city"] | "Medellin", sizeof(city_name) - 1);
    dtostrf(doc["lat"] | 6.2442, 6, 4, latitude);
    dtostrf(doc["lon"] | -75.5812, 6, 4, longitude);
    gmt_offset = (doc["offset"] | -18000) / 3600;

    FILE *f = fopen("/sdcard/config_clock.txt", "w");
    if (f) {
      fprintf(f, "city=%s\nlatitude=%s\nlongitude=%s\ngmt=%d\n", city_name, latitude, longitude, gmt_offset);
      fclose(f);
    } else {
      sdCardError = true;
    }
  }
  http.end();
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

    FILE *f = fopen("/sdcard/config_wifi.txt", "w");
    if (f) {
      fprintf(f, "wifi_ssid=%s\nwifi_pass=%s\nmac_server=%s\n", req_ssid.c_str(), req_pass.c_str(), mac_server);
      fclose(f);
    } else {
      sdCardError = true;
    }

    server.send(200, "text/html", "<html><body><h3>Credentials Saved! Connecting...</h3></body></html>");
    delay(1000);

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(req_ssid.c_str(), req_pass.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      queryIPGeolocation();
      syncNTP();
      updateOpenMeteo();
    }

    currentScreen = SCREEN_CLOCK;
    drawClockCalendarScreen();
  });
}

// ================= SCREEN 3: DYNAMIC PASSWORDLESS QR CONFIG =================
void startWiFiConfiguratorAP() {
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
void applyScreenPowerMode(AppScreen scr) {
  if (scr == SCREEN_MEETING) {
    digitalWrite(PA_CTRL, HIGH);
    if (!codecport) {
      codecport = new CodecPort(I2cbus, "S3_RLCD_4_2");
      codecport->CodecPort_SetInfo("es8311 & es7210", 1, SAMPLE_RATE, CHANNELS, 16);
      codecport->CodecPort_SetMicGain(35);
    }
  } else {
    if (isRecording) {
      stopMeetingRecording();
    }
    digitalWrite(PA_CTRL, LOW);
  }

  if (scr == SCREEN_CONFIG) {
    startWiFiConfiguratorAP();
  } else if (previousScreen == SCREEN_CONFIG) {
    server.close();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_pass);
  }
}

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

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  int wRetries = 0;
  while (WiFi.status() != WL_CONNECTED && wRetries < 15) {
    delay(400);
    wRetries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncNTP();
    updateOpenMeteo();
    lastWeatherSync = millis();
  }

  drawClockCalendarScreen();
  lastDrawnMinute = rtc.getMinute();
}

void loop() {
  // 1. Screen Switching Button (GPIO 18 - Left)
  bool reading = digitalRead(BUTTON_PIN);
  if (reading == LOW && lastButtonState == HIGH && (millis() - lastButtonPress > debounceTime)) {
    lastButtonPress = millis();
    previousScreen = currentScreen;

    // CONDITIONAL 3rd SCREEN:
    // Only cycle into SCREEN_CONFIG if WiFi is disconnected
    if (currentScreen == SCREEN_CLOCK) {
      currentScreen = SCREEN_MEETING;
    } else if (currentScreen == SCREEN_MEETING) {
      if (WiFi.status() == WL_CONNECTED) {
        currentScreen = SCREEN_CLOCK;
      } else {
        currentScreen = SCREEN_CONFIG;
      }
    } else if (currentScreen == SCREEN_CONFIG) {
      currentScreen = SCREEN_CLOCK;
    }

    applyScreenPowerMode(currentScreen);

    if (currentScreen == SCREEN_CLOCK) drawClockCalendarScreen();
    else if (currentScreen == SCREEN_MEETING) drawMeetingAssistantUI();
  }
  lastButtonState = reading;

  // 2. Right Button (GPIO 0 - BOOT) for Recording Toggle
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

    // Stream audio frames to SD card with write-timeout protection
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

  // 3. Screen-Specific Periodic Tasks
  if (currentScreen == SCREEN_CONFIG) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else if (currentScreen == SCREEN_CLOCK) {
    int curMin = rtc.getMinute();
    unsigned long now = millis();

    // 15-minute sync query
    if (now - lastWeatherSync >= WEATHER_SYNC_INTERVAL) {
      lastWeatherSync = now;
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(wifi_ssid, wifi_pass);
        int r = 0;
        while (WiFi.status() != WL_CONNECTED && r < 10) {
          delay(300);
          r++;
        }
      }
      if (WiFi.status() == WL_CONNECTED) {
        syncNTP();
        updateOpenMeteo();
      }
      drawClockCalendarScreen();
      lastDrawnMinute = curMin;
    } 
    // Minute tick update
    else if (curMin != lastDrawnMinute) {
      lastDrawnMinute = curMin;
      drawClockCalendarScreen();
    }
  }

  delay(20);
}