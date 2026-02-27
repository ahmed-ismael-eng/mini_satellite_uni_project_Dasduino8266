

#include "web_interface.h"
#include <Adafruit_BMP085.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoOTA.h>
#include <DHT.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <FS.h>
#include <Wire.h>

// ===================WiFi
// ============================================================================
const char *WIFI_SSID = "WIFI NAME";
const char *WIFI_PASSWORD = "WIFI PASSWORD";

// ============================================================================
//  
// ============================================================================
#define I2C_SDA 4
#define I2C_SCL 5
#define DHT_PIN 14
#define BATTERY_PIN A0
#define DHT_TYPE DHT11

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

// ============================================================================
//  
// ============================================================================
#define BATTERY_VOLTAGE_MULTIPLIER 4.2
#define BATTERY_MIN_VOLTAGE 3.0
#define BATTERY_MAX_VOLTAGE 4.2
#define BATTERY_CRITICAL_VOLTAGE 3.2
#define BATTERY_LOW_VOLTAGE 3.4

// ============================================================================
//  
// ============================================================================
#define HISTORY_SIZE 60
#define LOG_FILE "/flight_log.csv"
#define BLACKBOX_FILE "/blackbox.csv"
#define MAX_LOG_SIZE 50000 // 50KB max

// ============================================================================
//  
// ============================================================================
#define KNOWN_ALTITUDE_METERS 20.0

// ============================================================================
//  
// ============================================================================
Adafruit_BMP085 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);

// ============================================================================
// 
// ============================================================================

struct SensorData {
  // 
  float temperature;
  float temperature_bmp;
  float temperature_dht;
  float humidity;
  float pressure;
  float pressure_sea_level;
  float altitude;
  float altitude_raw;
  float battery_voltage;
  uint8_t battery_percent;

  // الوقت
  unsigned long uptime;
  unsigned long timestamp;

  // حالة الحساسات
  bool dht_valid;
  bool bmp_valid;
  bool battery_valid;

  // إحصائيات النظام
  uint32_t free_heap;
  uint8_t heap_percent;
  float cpu_load;
  int8_t wifi_rssi;
  uint8_t wifi_quality;
  String wifi_ssid;
  String ip_address;

  //  
  String health_status;
  String weather_trend;
  float pressure_rate;
  float temp_rate;
  int estimated_runtime_min;

  //  
  float dew_point;
  float heat_index;
  float absolute_humidity;
  float vapor_pressure;

} sensorData;

struct HistoryBuffer {
  float temperature[HISTORY_SIZE];
  float humidity[HISTORY_SIZE];
  float pressure[HISTORY_SIZE];
  float altitude[HISTORY_SIZE];
  float battery[HISTORY_SIZE];
  unsigned long timestamps[HISTORY_SIZE];
  uint8_t index;
  bool full;
} history;

struct SystemState {
  float referencePressure;
  unsigned long lastLogSave;
  unsigned long lastBlackboxSave;
  bool oledEnabled;
  String currentProfile;
  unsigned long totalReadings;
  unsigned long errorCount;
  unsigned long refreshRate;
} systemState;

struct Statistics {
  float temp_min;
  float temp_max;
  float temp_avg;
  float humidity_min;
  float humidity_max;
  float humidity_avg;
  float pressure_min;
  float pressure_max;
  float pressure_avg;
  unsigned long session_start;
} stats;

// ============================================================================
//  
// ============================================================================
unsigned long lastSensorRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastStatsUpdate = 0;
unsigned long lastHistoryUpdate = 0;
unsigned long systemStartTime = 0;
unsigned long loopStartTime = 0;

// ============================================================================
//  
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println(F("\n\n╔═══════════════════════════════════════════════╗"));
  Serial.println(F("║   ESP8266 MISSION CONTROL SYSTEM v3.0        ║"));
  Serial.println(F("║                                              ║"));
  Serial.println(F("╚═══════════════════════════════════════════════╝\n"));

  //  SPIFFS
  Serial.print(F("[SPIFFS] Initializing... "));
  if (SPIFFS.begin()) {
    Serial.println(F("SUCCESS"));
    FSInfo fs_info;
    SPIFFS.info(fs_info);
    Serial.printf("[SPIFFS] Total: %d bytes, Used: %d bytes\n",
                  fs_info.totalBytes, fs_info.usedBytes);
  } else {
    Serial.println(F("FAILED - Formatting..."));
    SPIFFS.format();
    SPIFFS.begin();
  }

  //   
  systemState.referencePressure = 1013.25;
  systemState.lastLogSave = 0;
  systemState.lastBlackboxSave = 0;
  systemState.oledEnabled = true;
  systemState.currentProfile = "Balanced";
  systemState.totalReadings = 0;
  systemState.errorCount = 0;
  systemState.refreshRate = 2000;

  //  
  stats.temp_min = 999;
  stats.temp_max = -999;
  stats.temp_avg = 0;
  stats.humidity_min = 999;
  stats.humidity_max = -999;
  stats.humidity_avg = 0;
  stats.pressure_min = 9999;
  stats.pressure_max = -9999;
  stats.pressure_avg = 0;
  stats.session_start = millis();

  //  
  history.index = 0;
  history.full = false;

  // 
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Serial.println(F("[I2C] Initialized"));

  // BMP180
  Serial.print(F("[BMP180] Initializing... "));
  if (bmp.begin(BMP085_ULTRAHIGHRES)) {
    Serial.println(F("✓ SUCCESS"));
    sensorData.bmp_valid = true;

    // 
    delay(100);
    float tempK = bmp.readTemperature() + 273.15;
    float pressureHpa = bmp.readPressure() / 100.0;
    systemState.referencePressure =
        pressureHpa /
        pow(1.0 - (KNOWN_ALTITUDE_METERS * 0.0065) / tempK, 5.257);
    Serial.printf("[BMP180] Auto-calibrated | Ref: %.2f hPa\n",
                  systemState.referencePressure);
  } else {
    Serial.println(F("✗ FAILED"));
    sensorData.bmp_valid = false;
  }

  //  OLED
  Serial.print(F("[OLED] Initializing... "));
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("✓ SUCCESS"));
    displayBootScreen();
  } else {
    Serial.println(F("✗ FAILED"));
  }

  //  DHT11
  Serial.print(F("[DHT11] Initializing... "));
  dht.begin();
  delay(2000);
  float testHumidity = dht.readHumidity();
  if (!isnan(testHumidity)) {
    Serial.println(F("✓ SUCCESS"));
    sensorData.dht_valid = true;
  } else {
    Serial.println(F("⚠ WARNING - Sensor may need warm-up"));
    sensorData.dht_valid = false;
  }

  //   WiFi
  connectWiFi();

  //  OTA
  setupOTA();

  //   
  setupWebServer();

  //  
  readAllSensors();
  updateStatistics();
  updateHistory();

  //
  if (!SPIFFS.exists(LOG_FILE)) {
    File f = SPIFFS.open(LOG_FILE, "w");
    if (f) {
      f.println("Timestamp,Uptime,Temp_BMP,Temp_DHT,Humidity,Pressure,Altitude,"
                "Battery_V,Battery_%,Heap,CPU,WiFi_RSSI,Health,Trend");
      f.close();
      Serial.println(F("[LOG] Flight log created"));
    }
  }

  systemStartTime = millis();

  Serial.println(F("\n✓ SYSTEM READY - Mission Control Active"));
  Serial.println(F("═══════════════════════════════════════════════\n"));
}

// ============================================================================
//  WiFi
// ============================================================================

void connectWiFi() {
  Serial.print(F("[WiFi] Connecting to: "));
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(F("."));
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WiFi] ✓ CONNECTED"));
    Serial.print(F("[WiFi] SSID: "));
    Serial.println(WiFi.SSID());
    Serial.print(F("[WiFi] IP: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("[WiFi] Signal: "));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));

    sensorData.wifi_ssid = WiFi.SSID();
  } else {
    Serial.println(F("\n[WiFi] ✗ OFFLINE MODE"));
    sensorData.wifi_ssid = "Disconnected";
  }
}

// ============================================================================
//  OTA
// ============================================================================

void setupOTA() {
  ArduinoOTA.setHostname("mission-control");
  ArduinoOTA.onStart([]() {
    String type =
        (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.println("[OTA] Update start: " + type);
  });
  ArduinoOTA.onEnd([]() { Serial.println(F("\n[OTA] Update complete")); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR)
      Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR)
      Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR)
      Serial.println(F("End Failed"));
  });
  ArduinoOTA.begin();
  Serial.println(F("[OTA] Ready for updates"));
}

// ============================================================================
// خادم الويب
// ============================================================================

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/download", handleDownload);
  server.on("/clear_blackbox", handleClearBlackbox);
  server.on("/calibrate", handleCalibrate);
  server.on("/statistics", handleStatistics);
  server.on("/set_profile", handleSetProfile);
  server.on("/toggle_oled", handleToggleOLED);
  server.on("/restart", handleRestart);

  server.begin();
  Serial.println(F("[WebServer] Started on port 80"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WebServer] Access: http://"));
    Serial.println(WiFi.localIP());
  }
}

void handleRoot() { server.send_P(200, "text/html", MAIN_PAGE); }

void handleSetProfile() {
  if (server.hasArg("profile")) {
    systemState.currentProfile = server.arg("profile");

    // Apply profile settings
    if (systemState.currentProfile == "Performance") {
      systemState.refreshRate = 500;
    } else if (systemState.currentProfile == "Balanced") {
      systemState.refreshRate = 2000;
    } else if (systemState.currentProfile == "Ultra Low Power") {
      systemState.refreshRate = 10000;
      if (systemState.oledEnabled) {
        systemState.oledEnabled = false;
        display.clearDisplay();
        display.display();
      }
    }

    server.send(200, "text/plain",
                "Profile set to: " + systemState.currentProfile);
  } else {
    server.send(400, "text/plain", "Missing profile argument");
  }
}

void handleToggleOLED() {
  systemState.oledEnabled = !systemState.oledEnabled;

  if (!systemState.oledEnabled) {
    display.clearDisplay();
    display.display();
  }

  String status = systemState.oledEnabled ? "ON" : "OFF";
  server.send(200, "text/plain", "OLED display turned " + status);
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting system...");
  delay(1000);
  ESP.restart();
}

void handleData() {
  sensorData.uptime = (millis() - systemStartTime) / 1000;

  // 
  String health = "NORMAL";
  if (!sensorData.bmp_valid ||
      sensorData.battery_voltage < BATTERY_CRITICAL_VOLTAGE) {
    health = "CRITICAL";
  } else if (!sensorData.dht_valid ||
             sensorData.battery_voltage < BATTERY_LOW_VOLTAGE ||
             sensorData.wifi_quality < 30) {
    health = "WARNING";
  }

  String json = "{";
  json += "\"temperature\":" + String(sensorData.temperature, 2) + ",";
  json += "\"temperature_bmp\":" + String(sensorData.temperature_bmp, 2) + ",";
  json += "\"temperature_dht\":" + String(sensorData.temperature_dht, 2) + ",";
  json += "\"humidity\":" + String(sensorData.humidity, 1) + ",";
  json += "\"pressure\":" + String(sensorData.pressure, 2) + ",";
  json += "\"pressure_sea_level\":" + String(sensorData.pressure_sea_level, 2) +
          ",";
  json += "\"altitude\":" + String(sensorData.altitude, 1) + ",";
  json += "\"altitude_raw\":" + String(sensorData.altitude_raw, 1) + ",";
  json += "\"battery\":" + String(sensorData.battery_voltage, 3) + ",";
  json += "\"battery_percent\":" + String(sensorData.battery_percent) + ",";
  json += "\"free_heap\":" + String(sensorData.free_heap) + ",";
  json += "\"heap_percent\":" + String(sensorData.heap_percent) + ",";
  json += "\"cpu_load\":" + String(sensorData.cpu_load, 1) + ",";
  json += "\"wifi_rssi\":" + String(sensorData.wifi_rssi) + ",";
  json += "\"wifi_quality\":" + String(sensorData.wifi_quality) + ",";
  json += "\"wifi_ssid\":\"" + sensorData.wifi_ssid + "\",";
  json += "\"ip_address\":\"" + sensorData.ip_address + "\",";
  json += "\"uptime\":" + String(sensorData.uptime) + ",";
  json += "\"health\":\"" + health + "\",";
  json += "\"trend\":\"" + sensorData.weather_trend + "\",";
  json += "\"pressure_rate\":" + String(sensorData.pressure_rate, 2) + ",";
  json += "\"temp_rate\":" + String(sensorData.temp_rate, 2) + ",";
  json += "\"profile\":\"" + systemState.currentProfile + "\",";
  json += "\"total_readings\":" + String(systemState.totalReadings) + ",";
  json += "\"dew_point\":" + String(sensorData.dew_point, 1) + ",";
  json += "\"heat_index\":" + String(sensorData.heat_index, 1) + ",";
  json += "\"runtime_text\":\"";
  if (sensorData.estimated_runtime_min > 0) {
    json += String(sensorData.estimated_runtime_min) + " min";
  } else {
    json += "N/A";
  }
  json += "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleHistory() {
  String json = "{";

  int count = history.full ? HISTORY_SIZE : history.index;

  json += "\"temperature\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0)
      json += ",";
    json += String(history.temperature[i], 1);
  }
  json += "],\"humidity\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0)
      json += ",";
    json += String(history.humidity[i], 1);
  }
  json += "],\"pressure\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0)
      json += ",";
    json += String(history.pressure[i], 1);
  }
  json += "],\"altitude\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0)
      json += ",";
    json += String(history.altitude[i], 1);
  }
  json += "],\"battery\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0)
      json += ",";
    json += String(history.battery[i], 2);
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleDownload() {
  if (!SPIFFS.exists(LOG_FILE)) {
    server.send(404, "text/plain", "No log file found");
    return;
  }

  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) {
    server.send(500, "text/plain", "Failed to open log");
    return;
  }

  server.streamFile(f, "text/csv");
  f.close();
}

void handleClearBlackbox() {
  if (SPIFFS.exists(BLACKBOX_FILE)) {
    SPIFFS.remove(BLACKBOX_FILE);
  }
  if (SPIFFS.exists(LOG_FILE)) {
    SPIFFS.remove(LOG_FILE);
  }

  File f = SPIFFS.open(LOG_FILE, "w");
  if (f) {
    f.println("Timestamp,Uptime,Temp_BMP,Temp_DHT,Humidity,Pressure,Altitude,"
              "Battery_V,Battery_%,Heap,CPU,WiFi_RSSI,Health,Trend");
    f.close();
  }

  systemState.totalReadings = 0;
  server.send(200, "text/plain", "Blackbox cleared successfully");
}

void handleCalibrate() {
  if (!sensorData.bmp_valid) {
    server.send(500, "text/plain", "BMP180 not available");
    return;
  }

  float tempK = bmp.readTemperature() + 273.15;
  float pressureHpa = bmp.readPressure() / 100.0;
  systemState.referencePressure =
      pressureHpa / pow(1.0 - (KNOWN_ALTITUDE_METERS * 0.0065) / tempK, 5.257);

  String msg = "Altitude calibrated!\nReference pressure: " +
               String(systemState.referencePressure, 2) + " hPa\n";
  msg += "Altitude set to: " + String(KNOWN_ALTITUDE_METERS) + " meters";
  server.send(200, "text/plain", msg);
}

void handleStatistics() {
  String json = "{";
  json += "\"temp_min\":" + String(stats.temp_min, 2) + ",";
  json += "\"temp_max\":" + String(stats.temp_max, 2) + ",";
  json += "\"temp_avg\":" + String(stats.temp_avg, 2) + ",";
  json += "\"humidity_min\":" + String(stats.humidity_min, 1) + ",";
  json += "\"humidity_max\":" + String(stats.humidity_max, 1) + ",";
  json += "\"humidity_avg\":" + String(stats.humidity_avg, 1) + ",";
  json += "\"pressure_min\":" + String(stats.pressure_min, 2) + ",";
  json += "\"pressure_max\":" + String(stats.pressure_max, 2) + ",";
  json += "\"pressure_avg\":" + String(stats.pressure_avg, 2);
  json += "}";
  server.send(200, "application/json", json);
}

// ============================================================================
//  
// ============================================================================

void readAllSensors() {
  readBMP180();
  readDHT11();
  readBattery();
  readSystemStats();
  calculateAdvancedMetrics();
  systemState.totalReadings++;
}

void readBMP180() {
  if (!sensorData.bmp_valid)
    return;

  sensorData.temperature_bmp = bmp.readTemperature();
  int32_t pressurePa = bmp.readPressure();
  sensorData.pressure = pressurePa / 100.0;
  sensorData.altitude_raw = bmp.readAltitude();
  sensorData.altitude = bmp.readAltitude(systemState.referencePressure * 100);

  //   
  float tempK = sensorData.temperature_bmp + 273.15;
  sensorData.pressure_sea_level =
      sensorData.pressure /
      pow(1.0 - (sensorData.altitude * 0.0065) / tempK, 5.257);

  // 
  sensorData.temperature = sensorData.temperature_bmp;
}

void readDHT11() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && h >= 0 && h <= 100) {
    sensorData.humidity = h;
    sensorData.dht_valid = true;
  } else {
    sensorData.dht_valid = false;
  }

  if (!isnan(t) && t > -40 && t < 80) {
    sensorData.temperature_dht = t;
  }
}

void readBattery() {
  int adcValue = analogRead(BATTERY_PIN);
  sensorData.battery_voltage = (adcValue / 1023.0) * BATTERY_VOLTAGE_MULTIPLIER;

  if (sensorData.battery_voltage >= BATTERY_MAX_VOLTAGE) {
    sensorData.battery_percent = 100;
  } else if (sensorData.battery_voltage <= BATTERY_MIN_VOLTAGE) {
    sensorData.battery_percent = 0;
  } else {
    sensorData.battery_percent =
        ((sensorData.battery_voltage - BATTERY_MIN_VOLTAGE) /
         (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE)) *
        100.0;
  }

  sensorData.battery_valid = (sensorData.battery_voltage > 0);


  float batteryCapacity = sensorData.battery_percent;
  int currentConsumption = 100; // mA 
  float batteryMah = 500.0;
  float remainingMah = (batteryCapacity / 100.0) * batteryMah;
  if (currentConsumption > 0) {
    float hoursRemaining = remainingMah / (float)currentConsumption;
    sensorData.estimated_runtime_min = (int)(hoursRemaining * 60);
  }
}

void readSystemStats() {
  // 
  sensorData.free_heap = ESP.getFreeHeap();
  uint32_t total_heap = 81920;
  sensorData.heap_percent = 100 - ((sensorData.free_heap * 100) / total_heap);

  // WiFi
  if (WiFi.status() == WL_CONNECTED) {
    sensorData.wifi_rssi = WiFi.RSSI();
    sensorData.wifi_ssid = WiFi.SSID();
    sensorData.ip_address = WiFi.localIP().toString();

    if (sensorData.wifi_rssi >= -50) {
      sensorData.wifi_quality = 100;
    } else if (sensorData.wifi_rssi <= -100) {
      sensorData.wifi_quality = 0;
    } else {
      sensorData.wifi_quality = 2 * (sensorData.wifi_rssi + 100);
    }
  } else {
    sensorData.wifi_rssi = -100;
    sensorData.wifi_quality = 0;
    sensorData.wifi_ssid = "Disconnected";
    sensorData.ip_address = "0.0.0.0";
  }

  //   
  static unsigned long lastCPUCheck = 0;
  static unsigned long lastLoopTime = 0;
  unsigned long now = millis();
  if (now - lastCPUCheck >= 1000) {
    unsigned long loopTime = now - lastLoopTime;
    sensorData.cpu_load = constrain((loopTime / 10.0), 0, 100);
    lastCPUCheck = now;
  }
  lastLoopTime = now;
}

void calculateAdvancedMetrics() {
  //   (Dew Point)
  if (sensorData.dht_valid) {
    float a = 17.27;
    float b = 237.7;
    float alpha =
        ((a * sensorData.temperature) / (b + sensorData.temperature)) +
        log(sensorData.humidity / 100.0);
    sensorData.dew_point = (b * alpha) / (a - alpha);

    //  (Heat Index)
    float T = sensorData.temperature;
    float RH = sensorData.humidity;
    if (T >= 27) {
      sensorData.heat_index =
          -8.78469475556 + 1.61139411 * T + 2.33854883889 * RH -
          0.14611605 * T * RH - 0.012308094 * T * T -
          0.0164248277778 * RH * RH + 0.002211732 * T * T * RH +
          0.00072546 * T * RH * RH - 0.000003582 * T * T * RH * RH;
    } else {
      sensorData.heat_index = T;
    }
  }

  //   
  static float lastPressure = 0;
  static unsigned long lastTrendCheck = 0;
  unsigned long now = millis();

  if (lastPressure > 0 && (now - lastTrendCheck >= 3600000)) { // كل ساعة
    sensorData.pressure_rate = sensorData.pressure - lastPressure;

    if (sensorData.pressure_rate > 0.5) {
      sensorData.weather_trend = "RISING";
    } else if (sensorData.pressure_rate < -0.5) {
      sensorData.weather_trend = "FALLING";
    } else {
      sensorData.weather_trend = "STABLE";
    }

    lastTrendCheck = now;
  }

  if (lastPressure == 0) {
    lastPressure = sensorData.pressure;
    sensorData.weather_trend = "STABLE";
  }
}

// ============================================================================
// 
// ============================================================================

void updateStatistics() {
  if (sensorData.temperature < stats.temp_min)
    stats.temp_min = sensorData.temperature;
  if (sensorData.temperature > stats.temp_max)
    stats.temp_max = sensorData.temperature;

  if (sensorData.humidity < stats.humidity_min)
    stats.humidity_min = sensorData.humidity;
  if (sensorData.humidity > stats.humidity_max)
    stats.humidity_max = sensorData.humidity;

  if (sensorData.pressure < stats.pressure_min)
    stats.pressure_min = sensorData.pressure;
  if (sensorData.pressure > stats.pressure_max)
    stats.pressure_max = sensorData.pressure;



  static int readingCount = 0;
  readingCount++;
  stats.temp_avg =
      ((stats.temp_avg * (readingCount - 1)) + sensorData.temperature) /
      readingCount;
  stats.humidity_avg =
      ((stats.humidity_avg * (readingCount - 1)) + sensorData.humidity) /
      readingCount;
  stats.pressure_avg =
      ((stats.pressure_avg * (readingCount - 1)) + sensorData.pressure) /
      readingCount;
}

void updateHistory() {
  history.temperature[history.index] = sensorData.temperature;
  history.humidity[history.index] = sensorData.humidity;
  history.pressure[history.index] = sensorData.pressure;
  history.altitude[history.index] = sensorData.altitude;
  history.battery[history.index] = sensorData.battery_voltage;
  history.timestamps[history.index] = millis();

  history.index++;
  if (history.index >= HISTORY_SIZE) {
    history.index = 0;
    history.full = true;
  }
}

void saveToLog() {
  unsigned long now = millis();
  if (now - systemState.lastLogSave < 60000)
    return;   

  File f = SPIFFS.open(LOG_FILE, "a");
  if (!f)
    return;

  //    
  if (f.size() > MAX_LOG_SIZE) {
    f.close();
    SPIFFS.remove(LOG_FILE);
    f = SPIFFS.open(LOG_FILE, "w");
    f.println("Timestamp,Uptime,Temp_BMP,Temp_DHT,Humidity,Pressure,Altitude,"
              "Battery_V,Battery_%,Heap,CPU,WiFi_RSSI,Health,Trend");
  }

  String health = "NORMAL";
  if (!sensorData.bmp_valid ||
      sensorData.battery_voltage < BATTERY_CRITICAL_VOLTAGE) {
    health = "CRITICAL";
  } else if (!sensorData.dht_valid ||
             sensorData.battery_voltage < BATTERY_LOW_VOLTAGE) {
    health = "WARNING";
  }

  char line[256];
  sprintf(line, "%lu,%lu,%.2f,%.2f,%.1f,%.2f,%.1f,%.3f,%d,%lu,%.1f,%d,%s,%s",
          now, sensorData.uptime, sensorData.temperature_bmp,
          sensorData.temperature_dht, sensorData.humidity, sensorData.pressure,
          sensorData.altitude, sensorData.battery_voltage,
          sensorData.battery_percent, sensorData.free_heap, sensorData.cpu_load,
          sensorData.wifi_rssi, health.c_str(),
          sensorData.weather_trend.c_str());

  f.println(line);
  f.close();

  systemState.lastLogSave = now;

  Serial.println(F("[LOG] Data saved to flight log"));
}

// ============================================================================
//  OLED
// ============================================================================

void displayBootScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("MISSION CONTROL"));
  display.println(F("================"));
  display.println(F("ESP8266 v3.0"));
  display.println(F(""));
  display.println(F("Initializing..."));
  display.println(F(""));
  display.print(F("Network: "));
  display.println(WIFI_SSID);
  display.display();
}

void updateOLED() {
  if (!systemState.oledEnabled)
    return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // العنوان
  display.setCursor(0, 0);
  display.print(F("MISSION CTRL"));
  display.setCursor(90, 0);
  if (!sensorData.bmp_valid ||
      sensorData.battery_voltage < BATTERY_CRITICAL_VOLTAGE) {
    display.print(F("CRIT"));
  } else if (!sensorData.dht_valid ||
             sensorData.battery_voltage < BATTERY_LOW_VOLTAGE) {
    display.print(F("WARN"));
  } else {
    display.print(F("OK"));
  }

  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // البيانات الرئيسية
  display.setCursor(0, 12);
  display.print(F("T:"));
  display.print(sensorData.temperature, 1);
  display.print(F("C H:"));
  display.print(sensorData.humidity, 0);
  display.println(F("%"));

  display.print(F("P:"));
  display.print(sensorData.pressure, 0);
  display.print(F(" A:"));
  display.print(sensorData.altitude, 0);
  display.println(F("m"));

  display.drawLine(0, 30, 127, 30, SSD1306_WHITE);

  // 
  display.setCursor(0, 33);
  display.print(F("BAT:"));
  display.print(sensorData.battery_voltage, 2);
  display.print(F("V "));
  display.print(sensorData.battery_percent);
  display.println(F("%"));

  display.print(F("WiFi:"));
  display.print(sensorData.wifi_quality);
  display.print(F("% CPU:"));
  display.print(sensorData.cpu_load, 0);
  display.println(F("%"));

  display.drawLine(0, 51, 127, 51, SSD1306_WHITE);

  //   
  display.setCursor(0, 54);
  display.print(sensorData.wifi_ssid.substring(0, 10));
  display.setCursor(80, 54);
  int minutes = sensorData.uptime / 60;
  display.print(F("T+"));
  display.print(minutes);
  display.print(F("m"));

  display.display();
}

// ============================================================================
//  
// ============================================================================

void loop() {
  loopStartTime = millis();
  unsigned long currentMillis = millis();

  // 
  ArduinoOTA.handle();

  // 
  if (currentMillis - lastSensorRead >= systemState.refreshRate) {
    readAllSensors();
    lastSensorRead = currentMillis;
  }

  //  
  if (currentMillis - lastStatsUpdate >= 5000) {
    updateStatistics();
    lastStatsUpdate = currentMillis;
  }

  //
  if (currentMillis - lastHistoryUpdate >= 60000) {
    updateHistory();
    saveToLog();
    lastHistoryUpdate = currentMillis;
  }

  //
  if (currentMillis - lastOLEDUpdate >= 1000) {
    updateOLED();
    lastOLEDUpdate = currentMillis;
  }

  //
  server.handleClient();

  yield();
}

