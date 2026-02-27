/*
 * ESP8266 Environmental Monitoring System
 * Production Firmware v1.2
 * 
 * Hardware:
 * - ESP8266 (Dasduino Connect)
 * - BMP180 (I2C: 0x77) - Temperature, Pressure, Altitude
 * - DHT11 (GPIO14/D5) - Humidity
 * - OLED SSD1306 128x64 (I2C: 0x3C)
 * - LiPo Battery (voltage on A0)
 * 
 * I2C Bus: SDA=GPIO4(D2), SCL=GPIO5(D1)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

// WiFi Credentials - CHANGE THESE
const char* WIFI_SSID = "DNA-WIFI-2C28";
const char* WIFI_PASSWORD = "18554222";

// Hardware Pin Definitions
#define I2C_SDA         4     // GPIO4 (D2)
#define I2C_SCL         5     // GPIO5 (D1)
#define DHT_PIN         14    // GPIO14 (D5)
#define BATTERY_PIN     A0    // ADC pin for battery voltage

// DHT Sensor Type
#define DHT_TYPE        DHT11

// OLED Display
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1    // No reset pin
#define OLED_ADDRESS    0x3C

// Timing Intervals (milliseconds)
#define SENSOR_READ_INTERVAL    2000    // Read sensors every 2 seconds
#define OLED_UPDATE_INTERVAL    1000    // Update OLED every 1 second
#define BATTERY_READ_INTERVAL   5000    // Read battery every 5 seconds
#define SYSTEM_STATS_INTERVAL   1000    // Update system stats every 1 second

// Battery voltage calculation
#define BATTERY_VOLTAGE_MULTIPLIER  4.2

// ALTITUDE CALIBRATION
// Option 1: Set your local sea level pressure (get from weather station)
// Option 2: Set your known altitude and the system will calculate reference pressure
#define USE_KNOWN_ALTITUDE      true    // Set to true to use known altitude
#define KNOWN_ALTITUDE_METERS   20.0    // My actual altitude (6th floor ~20m)

// If USE_KNOWN_ALTITUDE is false, set sea level pressure manually:
#define SEA_LEVEL_PRESSURE_HPA  1013.25

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================

Adafruit_BMP085 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);

// ============================================================================
// SENSOR DATA STRUCTURE
// ============================================================================

struct SensorData {
    float temperature;       // Temperature from BMP180 (°C)
    float humidity;          // Humidity from DHT11 (%)
    float pressure;          // Pressure from BMP180 (hPa)
    float altitude;          // Altitude from BMP180 (m)
    float battery_voltage;   // Battery voltage (V)
    unsigned long uptime;    // System uptime (seconds)
    bool dht_valid;          // DHT reading valid
    bool bmp_valid;          // BMP reading valid
    
    // System stats
    uint32_t free_heap;      // Free heap memory (bytes)
    uint8_t heap_percent;    // Heap usage percentage
    float cpu_load;          // Simulated CPU load (%)
    int8_t wifi_rssi;        // WiFi signal strength (dBm)
    uint8_t wifi_quality;    // WiFi quality (%)
} sensorData;

// ============================================================================
// TIMING VARIABLES
// ============================================================================

unsigned long lastSensorRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastBatteryRead = 0;
unsigned long lastSystemStats = 0;
unsigned long systemStartTime = 0;

// Variables for CPU load calculation
unsigned long lastLoopTime = 0;
unsigned long loopCounter = 0;
unsigned long lastLoopCounter = 0;

// Reference pressure for altitude (calculated or fixed)
float referencePressure = SEA_LEVEL_PRESSURE_HPA;

// ============================================================================
// INITIALIZATION
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n========================================");
    Serial.println("ESP8266 Environmental Monitor v1.2");
    Serial.println("========================================");
    Serial.println("Sensor Config:");
    Serial.println("- Temperature: BMP180");
    Serial.println("- Humidity: DHT11");
    Serial.println("- Pressure: BMP180");
    Serial.println("- Altitude: BMP180");
    Serial.println("========================================\n");
    
    // Initialize I2C bus
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000); // 100kHz
    Serial.println("[I2C] Bus initialized (SDA=GPIO4, SCL=GPIO5)");
    delay(100);
    
    // Initialize BMP180
    Serial.print("[BMP180] Initializing... ");
    if (bmp.begin(BMP085_STANDARD)) {
        Serial.println("SUCCESS (0x77)");
        sensorData.bmp_valid = true;
        
        // Calibrate altitude if using known altitude
        if (USE_KNOWN_ALTITUDE) {
            delay(100);
            int32_t pressurePa = bmp.readPressure();
            float pressureHpa = pressurePa / 100.0;
            // Calculate reference pressure from known altitude
            // P0 = P / (1 - (0.0065 * h) / (T + 0.0065 * h + 273.15))^5.257
            float tempK = bmp.readTemperature() + 273.15;
            referencePressure = pressureHpa / pow(1.0 - (KNOWN_ALTITUDE_METERS * 0.0065) / tempK, 5.257);
            
            Serial.print("[BMP180] Altitude calibrated to ");
            Serial.print(KNOWN_ALTITUDE_METERS);
            Serial.println("m");
            Serial.print("[BMP180] Calculated reference pressure: ");
            Serial.print(referencePressure);
            Serial.println(" hPa");
        }
        
        // Test read
        delay(100);
        float testTemp = bmp.readTemperature();
        int32_t testPressure = bmp.readPressure();
        Serial.print("[BMP180] Test - Temp: ");
        Serial.print(testTemp);
        Serial.print("°C, Pressure: ");
        Serial.print(testPressure / 100.0);
        Serial.println(" hPa");
        
    } else {
        Serial.println("FAILED!");
        sensorData.bmp_valid = false;
    }
    
    // Initialize OLED Display
    Serial.print("[OLED] Initializing... ");
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println("SUCCESS (0x3C)");
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("== ENV MONITOR ==");
        display.println("");
        display.println("  Initializing...");
        display.println("");
        if (sensorData.bmp_valid) {
            display.println("  BMP180: OK");
        } else {
            display.println("  BMP180: FAIL!");
        }
        display.display();
    } else {
        Serial.println("FAILED!");
    }
    
    // Initialize DHT11
    Serial.print("[DHT11] Initializing... ");
    dht.begin();
    delay(2000);
    float testHumidity = dht.readHumidity();
    if (!isnan(testHumidity)) {
        Serial.print("SUCCESS - Humidity: ");
        Serial.print(testHumidity);
        Serial.println("%");
        sensorData.dht_valid = true;
    } else {
        Serial.println("WARNING: Initial read failed");
        sensorData.dht_valid = false;
    }
    
    // Initialize sensor data
    sensorData.temperature = 0.0;
    sensorData.humidity = 0.0;
    sensorData.pressure = 0.0;
    sensorData.altitude = 0.0;
    sensorData.battery_voltage = 0.0;
    sensorData.free_heap = ESP.getFreeHeap();
    sensorData.cpu_load = 0.0;
    
    // Connect to WiFi
    connectWiFi();
    
    // Initialize web server
    setupWebServer();
    
    // Record system start time
    systemStartTime = millis();
    lastLoopTime = millis();
    
    // Initial sensor read
    readBMP();
    readDHT();
    readBattery();
    updateSystemStats();
    
    Serial.println("\n[SYSTEM] Initialization complete!");
    Serial.println("========================================\n");
    
    delay(2000);
}

// ============================================================================
// WiFi CONNECTION
// ============================================================================

void connectWiFi() {
    Serial.print("[WiFi] Connecting to ");
    Serial.print(WIFI_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connected!");
        Serial.print("[WiFi] IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WiFi] Signal Strength: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
    } else {
        Serial.println("\n[WiFi] Connection failed - operating in offline mode");
    }
}

// ============================================================================
// SYSTEM STATISTICS
// ============================================================================

void updateSystemStats() {
    // Memory usage
    sensorData.free_heap = ESP.getFreeHeap();
    uint32_t total_heap = 81920; // ESP8266 typical heap size
    sensorData.heap_percent = 100 - ((sensorData.free_heap * 100) / total_heap);
    
    // CPU load estimation (based on loop iterations per second)
    unsigned long currentTime = millis();
    unsigned long deltaTime = currentTime - lastLoopTime;
    
    if (deltaTime >= 1000) {
        unsigned long loopsPerSecond = loopCounter - lastLoopCounter;
        // Typical ESP8266 can do ~50000-100000 loops/sec when idle
        // Lower loop count = higher load
        float maxLoops = 80000.0;
        float loadFactor = 1.0 - (min(loopsPerSecond, (unsigned long)maxLoops) / maxLoops);
        sensorData.cpu_load = loadFactor * 100.0;
        
        lastLoopCounter = loopCounter;
        lastLoopTime = currentTime;
    }
    
    // WiFi signal quality
    if (WiFi.status() == WL_CONNECTED) {
        sensorData.wifi_rssi = WiFi.RSSI();
        // Convert RSSI to quality percentage
        // RSSI typically ranges from -30 (excellent) to -90 (poor)
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
    }
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.begin();
    Serial.println("[WebServer] Started on port 80");
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WebServer] Dashboard: http://");
        Serial.println(WiFi.localIP());
    }
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================

void handleRoot() {
    String html = F("<!DOCTYPE html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>Environmental Monitor</title>"
    "<style>"
    "* { margin: 0; padding: 0; box-sizing: border-box; }"
    "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; color: #fff; }"
    ".container { max-width: 1400px; margin: 0 auto; }"
    ".header { text-align: center; margin-bottom: 30px; }"
    ".header h1 { font-size: 2.5em; margin-bottom: 10px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }"
    ".header p { font-size: 1.1em; opacity: 0.9; }"
    ".status { background: rgba(255,255,255,0.15); backdrop-filter: blur(10px); border-radius: 15px; padding: 15px; margin-bottom: 20px; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; }"
    ".status-item { display: flex; align-items: center; gap: 10px; font-size: 0.95em; }"
    ".status-indicator { width: 12px; height: 12px; border-radius: 50%; background: #4ade80; box-shadow: 0 0 10px #4ade80; animation: pulse 2s infinite; }"
    ".status-indicator.error { background: #ef4444; box-shadow: 0 0 10px #ef4444; }"
    "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }"
    ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 20px; }"
    ".card { background: rgba(255,255,255,0.15); backdrop-filter: blur(10px); border-radius: 15px; padding: 25px; box-shadow: 0 8px 32px rgba(0,0,0,0.1); border: 1px solid rgba(255,255,255,0.18); transition: transform 0.3s ease; }"
    ".card:hover { transform: translateY(-5px); }"
    ".card-header { font-size: 0.85em; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 12px; opacity: 0.8; }"
    ".card-value { font-size: 2.3em; font-weight: bold; margin-bottom: 5px; line-height: 1; }"
    ".card-unit { font-size: 1.1em; opacity: 0.8; }"
    ".card-icon { font-size: 1.8em; margin-bottom: 8px; }"
    ".card-source { font-size: 0.7em; opacity: 0.6; margin-top: 8px; }"
    ".footer { text-align: center; margin-top: 30px; opacity: 0.8; font-size: 0.9em; }"
    "@media (max-width: 768px) { .header h1 { font-size: 1.8em; } .card-value { font-size: 2em; } .status { flex-direction: column; align-items: flex-start; } }"
    "</style>"
    "</head>"
    "<body>"
    "<div class='container'>"
    "<div class='header'>"
    "<h1>🛰️ Environmental Monitor</h1>"
    "<p>ESP8266 Ground Station</p>"
    "</div>"
    "<div class='status'>"
    "<div class='status-item'><div class='status-indicator' id='systemStatus'></div><span>System Online</span></div>"
    "<div class='status-item'><span>Uptime: <span id='uptime'>--</span></span></div>"
    "<div class='status-item'><span>BMP180: <span id='bmpStatus'>--</span></span></div>"
    "<div class='status-item'><span>DHT11: <span id='dhtStatus'>--</span></span></div>"
    "<div class='status-item'><span>IP: ");
    
    html += WiFi.localIP().toString();
    
    html += F("</span></div>"
    "</div>"
    "<div class='grid'>"
    "<div class='card'>"
    "<div class='card-icon'>🌡️</div>"
    "<div class='card-header'>Temperature</div>"
    "<div class='card-value' id='temp'>--</div>"
    "<div class='card-unit'>°C</div>"
    "<div class='card-source'>from BMP180</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>💧</div>"
    "<div class='card-header'>Humidity</div>"
    "<div class='card-value' id='humidity'>--</div>"
    "<div class='card-unit'>%</div>"
    "<div class='card-source'>from DHT11</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>🌍</div>"
    "<div class='card-header'>Pressure</div>"
    "<div class='card-value' id='pressure'>--</div>"
    "<div class='card-unit'>hPa</div>"
    "<div class='card-source'>from BMP180</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>⛰️</div>"
    "<div class='card-header'>Altitude</div>"
    "<div class='card-value' id='altitude'>--</div>"
    "<div class='card-unit'>m</div>"
    "<div class='card-source'>from BMP180</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>🔋</div>"
    "<div class='card-header'>Battery</div>"
    "<div class='card-value' id='battery'>--</div>"
    "<div class='card-unit'>V</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>💾</div>"
    "<div class='card-header'>Memory</div>"
    "<div class='card-value' id='memory'>--</div>"
    "<div class='card-unit'>KB free</div>"
    "<div class='card-source'>Heap: <span id='heapPercent'>--</span>% used</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>⚡</div>"
    "<div class='card-header'>CPU Load</div>"
    "<div class='card-value' id='cpuLoad'>--</div>"
    "<div class='card-unit'>%</div>"
    "</div>"
    "<div class='card'>"
    "<div class='card-icon'>📡</div>"
    "<div class='card-header'>WiFi Signal</div>"
    "<div class='card-value' id='wifiQuality'>--</div>"
    "<div class='card-unit'>%</div>"
    "<div class='card-source'>RSSI: <span id='wifiRssi'>--</span> dBm</div>"
    "</div>"
    "</div>"
    "<div class='footer'>"
    "<p>Last updated: <span id='lastUpdate'>--</span></p>"
    "</div>"
    "</div>"
    "<script>"
    "function updateData() {"
    "  fetch('/data')"
    "    .then(response => response.json())"
    "    .then(data => {"
    "      document.getElementById('temp').textContent = data.temperature.toFixed(1);"
    "      document.getElementById('humidity').textContent = data.humidity.toFixed(0);"
    "      document.getElementById('pressure').textContent = data.pressure.toFixed(1);"
    "      document.getElementById('altitude').textContent = data.altitude.toFixed(0);"
    "      document.getElementById('battery').textContent = data.battery.toFixed(2);"
    "      document.getElementById('memory').textContent = (data.free_heap / 1024).toFixed(1);"
    "      document.getElementById('heapPercent').textContent = data.heap_percent;"
    "      document.getElementById('cpuLoad').textContent = data.cpu_load.toFixed(0);"
    "      document.getElementById('wifiQuality').textContent = data.wifi_quality;"
    "      document.getElementById('wifiRssi').textContent = data.wifi_rssi;"
    "      document.getElementById('bmpStatus').textContent = data.bmp_valid ? 'OK' : 'ERROR';"
    "      document.getElementById('dhtStatus').textContent = data.dht_valid ? 'OK' : 'ERROR';"
    "      const statusIndicator = document.getElementById('systemStatus');"
    "      if (data.bmp_valid && data.dht_valid) {"
    "        statusIndicator.className = 'status-indicator';"
    "      } else {"
    "        statusIndicator.className = 'status-indicator error';"
    "      }"
    "      const uptimeMin = Math.floor(data.uptime / 60);"
    "      const uptimeSec = data.uptime % 60;"
    "      document.getElementById('uptime').textContent = uptimeMin + 'm ' + uptimeSec + 's';"
    "      const now = new Date();"
    "      document.getElementById('lastUpdate').textContent = now.toLocaleTimeString();"
    "    })"
    "    .catch(error => {"
    "      console.error('Error:', error);"
    "      document.getElementById('systemStatus').className = 'status-indicator error';"
    "    });"
    "}"
    "updateData();"
    "setInterval(updateData, 2000);"
    "</script>"
    "</body>"
    "</html>");
    
    server.send(200, "text/html", html);
}

void handleData() {
    sensorData.uptime = (millis() - systemStartTime) / 1000;
    
    String json = "{";
    json += "\"temperature\":" + String(sensorData.temperature, 1) + ",";
    json += "\"humidity\":" + String(sensorData.humidity, 0) + ",";
    json += "\"pressure\":" + String(sensorData.pressure, 1) + ",";
    json += "\"altitude\":" + String(sensorData.altitude, 0) + ",";
    json += "\"battery\":" + String(sensorData.battery_voltage, 2) + ",";
    json += "\"free_heap\":" + String(sensorData.free_heap) + ",";
    json += "\"heap_percent\":" + String(sensorData.heap_percent) + ",";
    json += "\"cpu_load\":" + String(sensorData.cpu_load, 1) + ",";
    json += "\"wifi_rssi\":" + String(sensorData.wifi_rssi) + ",";
    json += "\"wifi_quality\":" + String(sensorData.wifi_quality) + ",";
    json += "\"uptime\":" + String(sensorData.uptime) + ",";
    json += "\"dht_valid\":" + String(sensorData.dht_valid ? "true" : "false") + ",";
    json += "\"bmp_valid\":" + String(sensorData.bmp_valid ? "true" : "false");
    json += "}";
    
    server.send(200, "application/json", json);
}

// ============================================================================
// SENSOR READING FUNCTIONS
// ============================================================================

void readDHT() {
    float h = dht.readHumidity();
    
    if (!isnan(h) && h >= 0 && h <= 100) {
        sensorData.humidity = h;
        sensorData.dht_valid = true;
    } else {
        sensorData.dht_valid = false;
    }
}

void readBMP() {
    if (!sensorData.bmp_valid) return;
    
    float temp = bmp.readTemperature();
    int32_t pressurePa = bmp.readPressure();
    float pressureHpa = pressurePa / 100.0;
    
    // Calculate altitude using reference pressure
    float altitude = bmp.readAltitude(referencePressure * 100); // needs Pa
    
    if (!isnan(temp) && temp > -40 && temp < 85) {
        sensorData.temperature = temp;
    }
    
    if (pressurePa > 0 && pressureHpa > 300 && pressureHpa < 1100) {
        sensorData.pressure = pressureHpa;
    }
    
    if (!isnan(altitude) && altitude > -500 && altitude < 9000) {
        sensorData.altitude = altitude;
    }
}

void readBattery() {
    int adcValue = analogRead(BATTERY_PIN);
    sensorData.battery_voltage = (adcValue / 1023.0) * BATTERY_VOLTAGE_MULTIPLIER;
}

// ============================================================================
// OLED DISPLAY UPDATE
// ============================================================================

void updateOLED() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    // Header
    display.setCursor(0, 0);
    display.println("== ENV MONITOR ==");
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
    
    // Sensor readings - Line 1
    display.setCursor(0, 14);
    display.print("T:");
    display.print(sensorData.temperature, 1);
    display.print("C ");
    display.print("H:");
    display.print(sensorData.humidity, 0);
    display.println("%");
    
    // Line 2
    display.print("P:");
    display.print(sensorData.pressure, 0);
    display.print(" ");
    display.print("Alt:");
    display.print(sensorData.altitude, 0);
    display.println("m");
    
    // Line 3 - System stats
    display.drawLine(0, 32, 127, 32, SSD1306_WHITE);
    display.setCursor(0, 35);
    display.print("Mem:");
    display.print(sensorData.free_heap / 1024);
    display.print("KB ");
    display.print(sensorData.heap_percent);
    display.println("%");
    
    // Line 4
    display.print("CPU:");
    display.print(sensorData.cpu_load, 0);
    display.print("% ");
    display.print("WiFi:");
    display.print(sensorData.wifi_quality);
    display.println("%");
    
    // Bottom status line
    display.drawLine(0, 54, 127, 54, SSD1306_WHITE);
    display.setCursor(0, 56);
    display.print("Bat:");
    display.print(sensorData.battery_voltage, 2);
    display.print("V");
    
    if (WiFi.status() == WL_CONNECTED) {
        display.setCursor(85, 56);
        display.print("Link");
    }
    
    display.display();
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
    unsigned long currentMillis = millis();
    loopCounter++; // Count loops for CPU load calculation
    
    // Read sensors
    if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
        readBMP();
        readDHT();
        lastSensorRead = currentMillis;
    }
    
    // Read battery
    if (currentMillis - lastBatteryRead >= BATTERY_READ_INTERVAL) {
        readBattery();
        lastBatteryRead = currentMillis;
    }
    
    // Update system statistics
    if (currentMillis - lastSystemStats >= SYSTEM_STATS_INTERVAL) {
        updateSystemStats();
        lastSystemStats = currentMillis;
    }
    
    // Update OLED
    if (currentMillis - lastOLEDUpdate >= OLED_UPDATE_INTERVAL) {
        updateOLED();
        lastOLEDUpdate = currentMillis;
    }
    
    // Handle web server
    server.handleClient();
    
    yield();
}