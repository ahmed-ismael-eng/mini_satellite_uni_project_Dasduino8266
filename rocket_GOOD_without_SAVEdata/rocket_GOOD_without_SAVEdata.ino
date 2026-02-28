/*
 * ESP8266 Environmental Monitoring System
 * Professional Firmware v2.0 - Extended Edition
 * * Hardware:
 * - ESP8266 (Dasduino Connect)
 * - BMP180 (I2C: 0x77) - Temperature, Pressure, Altitude
 * - DHT11 (GPIO14/D5) - Humidity ONLY
 * - OLED SSD1306 128x64 (I2C: 0x3C)
 * - LiPo Battery (voltage on A0)
 * * Features:
 * - Real-time environmental monitoring
 * - System health monitoring
 * - Power management profiles
 * - Historical data & graphs
 * - Weather trend detection
 * - OTA firmware updates
 * - Web control panel
 * - Self-calibration
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <EEPROM.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

// WiFi Credentials
const char* WIFI_SSID = "DNA-WIFI-2C28";
const char* WIFI_PASSWORD = "18554222";

// Hardware Pin Definitions
#define I2C_SDA         4
#define I2C_SCL         5
#define DHT_PIN         14
#define BATTERY_PIN     A0

// DHT Sensor Type
#define DHT_TYPE        DHT11

// OLED Display
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C

// EEPROM Configuration
#define EEPROM_SIZE           512
#define EEPROM_MAGIC          0xAB12
#define EEPROM_ADDR_MAGIC     0
#define EEPROM_ADDR_REF_PRESS 4

// Battery Configuration
#define BATTERY_VOLTAGE_MULTIPLIER  4.2
#define BATTERY_MIN_VOLTAGE         3.0
#define BATTERY_MAX_VOLTAGE         4.2
#define BATTERY_CRITICAL_VOLTAGE    3.2
#define BATTERY_LOW_VOLTAGE         3.4

// History Buffer Size
#define HISTORY_SIZE    60

// Altitude Calibration
#define USE_KNOWN_ALTITUDE      true
#define KNOWN_ALTITUDE_METERS   20.0

// ============================================================================
// POWER PROFILES
// ============================================================================

enum PowerProfile {
    PROFILE_PERFORMANCE,
    PROFILE_BALANCED,
    PROFILE_ULTRA_LOW_POWER
};

struct ProfileConfig {
    unsigned long sensorInterval;
    unsigned long oledInterval;
    unsigned long batteryInterval;
    unsigned long statsInterval;
    bool wifiAlwaysOn;
    const char* name;
};

ProfileConfig profiles[] = {
    {1000, 500,  3000, 500,  true,  "Performance"},      // PERFORMANCE
    {2000, 1000, 5000, 1000, true,  "Balanced"},         // BALANCED
    {5000, 2000, 10000, 2000, false, "Ultra Low Power"}  // ULTRA_LOW_POWER
};

// ============================================================================
// SYSTEM HEALTH
// ============================================================================

enum HealthState {
    HEALTH_NORMAL,
    HEALTH_DEGRADED,
    HEALTH_CRITICAL
};

// ============================================================================
// WEATHER TRENDS
// ============================================================================

enum WeatherTrend {
    TREND_STABLE,
    TREND_RISING,
    TREND_FALLING
};

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================

Adafruit_BMP085 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct SensorData {
    float temperature;
    float humidity;
    float pressure;
    float altitude;
    float battery_voltage;
    uint8_t battery_percent;
    unsigned long uptime;
    bool dht_valid;
    bool bmp_valid;
    
    // System stats
    uint32_t free_heap;
    uint8_t heap_percent;
    float cpu_load;
    int8_t wifi_rssi;
    uint8_t wifi_quality;
    
    // Health & trends
    HealthState health;
    WeatherTrend trend;
    float pressure_rate;  // hPa per hour
    int estimated_runtime_min;
} sensorData;

// Circular buffers for history
struct HistoryBuffer {
    float temperature[HISTORY_SIZE];
    float pressure[HISTORY_SIZE];
    float battery[HISTORY_SIZE];
    uint8_t index;
    bool full;
} history;

// System state
struct SystemState {
    PowerProfile currentProfile;
    bool oledEnabled;
    bool otaInProgress;
    float referencePressure;
    unsigned long lastProfileChange;
} systemState;

// CPU Load Tracking
struct CPULoadTracker {
    unsigned long lastMeasureTime;
    unsigned long busyTime;
    unsigned long totalTime;
    float smoothedLoad;
} cpuTracker;

// ============================================================================
// TIMING VARIABLES
// ============================================================================

unsigned long lastSensorRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastBatteryRead = 0;
unsigned long lastSystemStats = 0;
unsigned long lastHistoryUpdate = 0;
unsigned long systemStartTime = 0;
unsigned long loopStartTime = 0;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void updateCPULoad();
void updateSystemHealth();
void updateWeatherTrend();
void updateHistory();
float calculateBatteryPercent(float voltage);
int estimateRuntimeMinutes();
void applyPowerProfile(PowerProfile profile);
void saveCalibrationToEEPROM();
void loadCalibrationFromEEPROM();
void connectWiFi();
void setupOTA();
void setupWebServer();
void handleRoot();
void handleData();
void handleHistory();
void handleControl();
void handleCalibrate();
void handleRestart();
void readDHT();
void readBMP();
void readBattery();
void updateSystemStats();
void updateOLED();

// ============================================================================
// INITIALIZATION
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(100);
    
    Serial.println("\n\n========================================");
    Serial.println("ESP8266 Environmental Monitor v2.0");
    Serial.println("Professional Edition");
    Serial.println("========================================\n");
    
    // Initialize EEPROM
    EEPROM.begin(EEPROM_SIZE);
    
    // Initialize system state
    systemState.currentProfile = PROFILE_BALANCED;
    systemState.oledEnabled = true;
    systemState.otaInProgress = false;
    systemState.lastProfileChange = 0;
    
    // Initialize CPU tracker
    cpuTracker.lastMeasureTime = 0;
    cpuTracker.busyTime = 0;
    cpuTracker.totalTime = 0;
    cpuTracker.smoothedLoad = 0.0;
    
    // Initialize history
    history.index = 0;
    history.full = false;
    
    // Initialize I2C
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    Serial.println("[I2C] Initialized");
    delay(100);
    
    // Initialize BMP180
    Serial.print("[BMP180] Initializing... ");
    if (bmp.begin(BMP085_STANDARD)) {
        Serial.println("SUCCESS");
        sensorData.bmp_valid = true;
        
        // Load or calculate calibration
        loadCalibrationFromEEPROM();
        
        if (USE_KNOWN_ALTITUDE && systemState.referencePressure == 0) {
            delay(100);
            int32_t pressurePa = bmp.readPressure();
            float pressureHpa = pressurePa / 100.0;
            float tempK = bmp.readTemperature() + 273.15;
            systemState.referencePressure = pressureHpa / pow(1.0 - (KNOWN_ALTITUDE_METERS * 0.0065) / tempK, 5.257);
            saveCalibrationToEEPROM();
            Serial.print("[BMP180] Auto-calibrated to ");
            Serial.print(KNOWN_ALTITUDE_METERS);
            Serial.println("m");
        }
    } else {
        Serial.println("FAILED");
        sensorData.bmp_valid = false;
    }
    
    // Initialize OLED
    Serial.print("[OLED] Initializing... ");
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println("SUCCESS");
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("ENV MONITOR v2.0");
        display.println("");
        display.println("Professional Ed.");
        display.println("");
        display.println("Initializing...");
        display.display();
    } else {
        Serial.println("FAILED");
    }
    
    // Initialize DHT11
    Serial.print("[DHT11] Initializing... ");
    dht.begin();
    delay(2000);
    float testHumidity = dht.readHumidity();
    if (!isnan(testHumidity)) {
        Serial.println("SUCCESS");
        sensorData.dht_valid = true;
    } else {
        Serial.println("WARNING");
        sensorData.dht_valid = false;
    }
    
    // Connect WiFi
    connectWiFi();
    
    // Setup OTA
    setupOTA();
    
    // Setup web server
    setupWebServer();
    
    // Initialize sensor data
    sensorData.health = HEALTH_NORMAL;
    sensorData.trend = TREND_STABLE;
    sensorData.pressure_rate = 0.0;
    
    systemStartTime = millis();
    
    // Initial reads
    readBMP();
    readDHT();
    readBattery();
    updateSystemStats();
    updateHistory();
    
    Serial.println("\n[SYSTEM] Initialization complete!");
    Serial.println("========================================\n");
    
    delay(2000);
}

// ============================================================================
// WIFI & OTA
// ============================================================================

void connectWiFi() {
    Serial.print("[WiFi] Connecting to ");
    Serial.println(WIFI_SSID);
    
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
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n[WiFi] Failed - offline mode");
    }
}

void setupOTA() {
    ArduinoOTA.setHostname("env-monitor");
    
    ArduinoOTA.onStart([]() {
        systemState.otaInProgress = true;
        Serial.println("[OTA] Update started");
        if (systemState.oledEnabled) {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("OTA UPDATE");
            display.println("In progress...");
            display.display();
        }
    });
    
    ArduinoOTA.onEnd([]() {
        systemState.otaInProgress = false;
        Serial.println("\n[OTA] Update complete");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        uint8_t percent = (progress / (total / 100));
        Serial.printf("[OTA] Progress: %u%%\r", percent);
        if (systemState.oledEnabled && percent % 10 == 0) {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("OTA UPDATE");
            display.print("Progress: ");
            display.print(percent);
            display.println("%");
            display.display();
        }
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        systemState.otaInProgress = false;
        Serial.printf("[OTA] Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    
    ArduinoOTA.begin();
    Serial.println("[OTA] Ready");
}

// ============================================================================
// WEB SERVER
// ============================================================================

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/history", handleHistory);
    server.on("/control", HTTP_POST, handleControl);
    server.on("/calibrate", HTTP_POST, handleCalibrate);
    server.on("/restart", HTTP_POST, handleRestart);
    
    server.begin();
    Serial.println("[WebServer] Started on port 80");
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WebServer] http://");
        Serial.println(WiFi.localIP());
    }
}

void handleRoot() {
    String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
    "<title>Environmental Monitor Pro</title><style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
    "min-height:100vh;padding:20px;color:#fff}"
    ".container{max-width:1600px;margin:0 auto}"
    ".header{text-align:center;margin-bottom:30px}"
    ".header h1{font-size:2.5em;margin-bottom:10px;text-shadow:2px 2px 4px rgba(0,0,0,0.3)}"
    ".status-bar{background:rgba(255,255,255,0.15);backdrop-filter:blur(10px);border-radius:15px;"
    "padding:15px;margin-bottom:20px;display:flex;justify-content:space-between;flex-wrap:wrap;gap:15px}"
    ".status-item{display:flex;align-items:center;gap:10px;font-size:0.9em}"
    ".indicator{width:12px;height:12px;border-radius:50%;animation:pulse 2s infinite}"
    ".indicator.normal{background:#4ade80;box-shadow:0 0 10px #4ade80}"
    ".indicator.degraded{background:#fbbf24;box-shadow:0 0 10px #fbbf24}"
    ".indicator.critical{background:#ef4444;box-shadow:0 0 10px #ef4444}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:20px;margin-bottom:20px}"
    ".card{background:rgba(255,255,255,0.15);backdrop-filter:blur(10px);border-radius:15px;"
    "padding:25px;box-shadow:0 8px 32px rgba(0,0,0,0.1);border:1px solid rgba(255,255,255,0.18);"
    "transition:transform 0.3s ease}"
    ".card:hover{transform:translateY(-5px)}"
    ".card-header{font-size:0.85em;text-transform:uppercase;letter-spacing:1px;margin-bottom:12px;opacity:0.8}"
    ".card-value{font-size:2.3em;font-weight:bold;margin-bottom:5px;line-height:1}"
    ".card-unit{font-size:1.1em;opacity:0.8}"
    ".card-icon{font-size:1.8em;margin-bottom:8px}"
    ".card-source{font-size:0.7em;opacity:0.6;margin-top:8px}"
    ".trend{font-size:2em;margin-left:10px}"
    ".chart-container{background:rgba(255,255,255,0.15);backdrop-filter:blur(10px);border-radius:15px;"
    "padding:25px;margin-bottom:20px}"
    "canvas{width:100%!important;height:200px!important}"
    ".controls{background:rgba(255,255,255,0.15);backdrop-filter:blur(10px);border-radius:15px;"
    "padding:25px;margin-bottom:20px}"
    ".control-group{margin-bottom:20px}"
    ".control-group h3{margin-bottom:15px;font-size:1.2em}"
    ".btn-group{display:flex;gap:10px;flex-wrap:wrap}"
    ".btn{padding:10px 20px;border:none;border-radius:8px;cursor:pointer;font-size:0.9em;"
    "font-weight:600;transition:all 0.3s;background:rgba(255,255,255,0.2);color:#fff}"
    ".btn:hover{background:rgba(255,255,255,0.3);transform:translateY(-2px)}"
    ".btn.active{background:rgba(255,255,255,0.4)}"
    ".footer{text-align:center;margin-top:30px;opacity:0.8;font-size:0.9em}"
    "@media(max-width:768px){.header h1{font-size:1.8em}.card-value{font-size:2em}}"
    "</style></head><body><div class='container'>"
    "<div class='header'><h1>🛰️ Environmental Monitor Pro</h1><p>ESP8266 Ground Station v2.0</p></div>"
    "<div class='status-bar'>"
    "<div class='status-item'><div class='indicator' id='healthInd'></div><span>Health: <span id='health'>--</span></span></div>"
    "<div class='status-item'><span>Profile: <span id='profile'>--</span></span></div>"
    "<div class='status-item'><span>Uptime: <span id='uptime'>--</span></span></div>"
    "<div class='status-item'><span>Runtime: <span id='runtime'>--</span></span></div>"
    "<div class='status-item'><span>IP: ");
    html += WiFi.localIP().toString();
    html += F("</span></div></div>"
    "<div class='grid'>"
    "<div class='card'><div class='card-icon'>🌡️</div><div class='card-header'>Temperature</div>"
    "<div class='card-value' id='temp'>--</div><div class='card-unit'>°C</div>"
    "<div class='card-source'>from BMP180</div></div>"
    "<div class='card'><div class='card-icon'>💧</div><div class='card-header'>Humidity</div>"
    "<div class='card-value' id='humidity'>--</div><div class='card-unit'>%</div>"
    "<div class='card-source'>from DHT11</div></div>"
    "<div class='card'><div class='card-icon'>🌍</div><div class='card-header'>Pressure</div>"
    "<div class='card-value'><span id='pressure'>--</span><span class='trend' id='trend'>→</span></div>"
    "<div class='card-unit'>hPa</div><div class='card-source'>from BMP180 | <span id='trendText'>Stable</span></div></div>"
    "<div class='card'><div class='card-icon'>⛰️</div><div class='card-header'>Altitude</div>"
    "<div class='card-value' id='altitude'>--</div><div class='card-unit'>m</div>"
    "<div class='card-source'>from BMP180</div></div>"
    "<div class='card'><div class='card-icon'>🔋</div><div class='card-header'>Battery</div>"
    "<div class='card-value' id='battery'>--</div><div class='card-unit'>V (<span id='batPercent'>--</span>%)</div></div>"
    "<div class='card'><div class='card-icon'>💾</div><div class='card-header'>Memory</div>"
    "<div class='card-value' id='memory'>--</div><div class='card-unit'>KB free</div>"
    "<div class='card-source'>Usage: <span id='heapPercent'>--</span>%</div></div>"
    "<div class='card'><div class='card-icon'>⚡</div><div class='card-header'>CPU Load</div>"
    "<div class='card-value' id='cpuLoad'>--</div><div class='card-unit'>%</div></div>"
    "<div class='card'><div class='card-icon'>📡</div><div class='card-header'>WiFi</div>"
    "<div class='card-value' id='wifiQuality'>--</div><div class='card-unit'>%</div>"
    "<div class='card-source'>RSSI: <span id='wifiRssi'>--</span> dBm</div></div>"
    "</div>"
    "<div class='chart-container'><h3 style='margin-bottom:15px'>Temperature History (60 samples)</h3>"
    "<canvas id='tempChart'></canvas></div>"
    "<div class='chart-container'><h3 style='margin-bottom:15px'>Pressure History (60 samples)</h3>"
    "<canvas id='pressChart'></canvas></div>"
    "<div class='controls'><h2 style='margin-bottom:20px'>Control Panel</h2>"
    "<div class='control-group'><h3>Power Profile</h3><div class='btn-group'>"
    "<button class='btn' onclick='setProfile(0)'>Performance</button>"
    "<button class='btn' onclick='setProfile(1)'>Balanced</button>"
    "<button class='btn' onclick='setProfile(2)'>Ultra Low Power</button></div></div>"
    "<div class='control-group'><h3>Display Control</h3><div class='btn-group'>"
    "<button class='btn' onclick='toggleOLED()'>Toggle OLED</button></div></div>"
    "<div class='control-group'><h3>Calibration & System</h3><div class='btn-group'>"
    "<button class='btn' onclick='calibrateAltitude()'>Calibrate Altitude</button>"
    "<button class='btn' onclick='restartESP()'>Restart ESP8266</button></div></div></div>"
    "<div class='footer'><p>Last updated: <span id='lastUpdate'>--</span></p></div></div>"
    "<script>"
    "let tempChart,pressChart;"
    "function drawChart(canvas,data,label,color){const ctx=canvas.getContext('2d');"
    "const w=canvas.width,h=canvas.height;ctx.clearRect(0,0,w,h);"
    "if(!data||data.length===0)return;"
    "const min=Math.min(...data),max=Math.max(...data),range=max-min||1;"
    "ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();"
    "data.forEach((v,i)=>{const x=i*(w/data.length),y=h-(((v-min)/range)*h*0.8+h*0.1);"
    "if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y)});ctx.stroke();"
    "ctx.fillStyle='rgba(255,255,255,0.7)';ctx.font='12px sans-serif';"
    "ctx.fillText(label+': '+min.toFixed(1)+' - '+max.toFixed(1),10,20)}"
    "function updateData(){"
    "fetch('/data').then(r=>r.json()).then(d=>{"
    "document.getElementById('temp').textContent=d.temperature.toFixed(1);"
    "document.getElementById('humidity').textContent=d.humidity.toFixed(0);"
    "document.getElementById('pressure').textContent=d.pressure.toFixed(1);"
    "document.getElementById('altitude').textContent=d.altitude.toFixed(0);"
    "document.getElementById('battery').textContent=d.battery.toFixed(2);"
    "document.getElementById('batPercent').textContent=d.battery_percent;"
    "document.getElementById('memory').textContent=(d.free_heap/1024).toFixed(1);"
    "document.getElementById('heapPercent').textContent=d.heap_percent;"
    "document.getElementById('cpuLoad').textContent=d.cpu_load.toFixed(0);"
    "document.getElementById('wifiQuality').textContent=d.wifi_quality;"
    "document.getElementById('wifiRssi').textContent=d.wifi_rssi;"
    "document.getElementById('health').textContent=d.health_text;"
    "document.getElementById('profile').textContent=d.profile_name;"
    "document.getElementById('runtime').textContent=d.runtime_text;"
    "const healthInd=document.getElementById('healthInd');"
    "healthInd.className='indicator '+d.health_class;"
    "const trendMap={'0':'→','1':'⬆','2':'⬇'};"
    "document.getElementById('trend').textContent=trendMap[d.trend]||'→';"
    "document.getElementById('trendText').textContent=d.trend_text;"
    "const uptimeMin=Math.floor(d.uptime/60),uptimeSec=d.uptime%60;"
    "document.getElementById('uptime').textContent=uptimeMin+'m '+uptimeSec+'s';"
    "document.getElementById('lastUpdate').textContent=new Date().toLocaleTimeString();"
    "}).catch(e=>console.error(e));"
    "fetch('/history').then(r=>r.json()).then(d=>{"
    "if(!tempChart)tempChart=document.getElementById('tempChart');"
    "if(!pressChart)pressChart=document.getElementById('pressChart');"
    "drawChart(tempChart,d.temperature,'Temp (°C)','#ff6b6b');"
    "drawChart(pressChart,d.pressure,'Press (hPa)','#4ecdc4');"
    "}).catch(e=>console.error(e))}"
    "function setProfile(p){fetch('/control',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({action:'profile',value:p})}).then(()=>updateData())}"
    "function toggleOLED(){fetch('/control',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({action:'oled_toggle'})}).then(()=>updateData())}"
    "function calibrateAltitude(){if(confirm('Calibrate altitude to current location?')){"
    "fetch('/calibrate',{method:'POST'}).then(r=>r.text()).then(alert)}}"
    "function restartESP(){if(confirm('Restart ESP8266?')){"
    "fetch('/restart',{method:'POST'}).then(()=>alert('Restarting...'))}}"
    "updateData();setInterval(updateData,2000);"
    "</script></body></html>");
    
    server.send(200, "text/html", html);
}

void handleData() {
    sensorData.uptime = (millis() - systemStartTime) / 1000;
    
    const char* healthTexts[] = {"Normal", "Degraded", "Critical"};
    const char* healthClasses[] = {"normal", "degraded", "critical"};
    const char* trendTexts[] = {"Stable", "Rising", "Falling"};
    
    String json = "{";
    json += "\"temperature\":" + String(sensorData.temperature, 1) + ",";
    json += "\"humidity\":" + String(sensorData.humidity, 0) + ",";
    json += "\"pressure\":" + String(sensorData.pressure, 1) + ",";
    json += "\"altitude\":" + String(sensorData.altitude, 0) + ",";
    json += "\"battery\":" + String(sensorData.battery_voltage, 2) + ",";
    json += "\"battery_percent\":" + String(sensorData.battery_percent) + ",";
    json += "\"free_heap\":" + String(sensorData.free_heap) + ",";
    json += "\"heap_percent\":" + String(sensorData.heap_percent) + ",";
    json += "\"cpu_load\":" + String(sensorData.cpu_load, 1) + ",";
    json += "\"wifi_rssi\":" + String(sensorData.wifi_rssi) + ",";
    json += "\"wifi_quality\":" + String(sensorData.wifi_quality) + ",";
    json += "\"uptime\":" + String(sensorData.uptime) + ",";
    json += "\"health\":" + String((int)sensorData.health) + ",";
    json += "\"health_text\":\"" + String(healthTexts[sensorData.health]) + "\",";
    json += "\"health_class\":\"" + String(healthClasses[sensorData.health]) + "\",";
    json += "\"trend\":" + String((int)sensorData.trend) + ",";
    json += "\"trend_text\":\"" + String(trendTexts[sensorData.trend]) + "\",";
    json += "\"pressure_rate\":" + String(sensorData.pressure_rate, 2) + ",";
    json += "\"profile\":" + String((int)systemState.currentProfile) + ",";
    json += "\"profile_name\":\"" + String(profiles[systemState.currentProfile].name) + "\",";
    json += "\"runtime_text\":\"";
    if (sensorData.estimated_runtime_min > 0) {
        json += String(sensorData.estimated_runtime_min) + " min";
    } else {
        json += "N/A";
    }
    json += "\",";
    json += "\"dht_valid\":" + String(sensorData.dht_valid ? "true" : "false") + ",";
    json += "\"bmp_valid\":" + String(sensorData.bmp_valid ? "true" : "false");
    json += "}";

    server.send(200, "application/json", json);
}

void handleHistory() {
    String json = "{";

    // Temperature history
    json += "\"temperature\":[";
    int count = history.full ? HISTORY_SIZE : history.index;
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += String(history.temperature[i], 1);
    }
    json += "],";

    // Pressure history
    json += "\"pressure\":[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += String(history.pressure[i], 1);
    }
    json += "],";

    // Battery history
    json += "\"battery\":[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += String(history.battery[i], 2);
    }
    json += "]";
    json += "}";
    server.send(200, "application/json", json);
}

void handleControl() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Bad Request");
        return;
    }

    String body = server.arg("plain");
    // Simple JSON parsing for control commands
    if (body.indexOf("\"action\":\"profile\"") >= 0) {
        int valuePos = body.indexOf("\"value\":");
        if (valuePos >= 0) {
            int profile = body.substring(valuePos + 8, valuePos + 9).toInt();
            if (profile >= 0 && profile <= 2) {
                applyPowerProfile((PowerProfile)profile);
                server.send(200, "text/plain", "Profile changed");
                return;
            }
        }
    } else if (body.indexOf("\"action\":\"oled_toggle\"") >= 0) {
        systemState.oledEnabled = !systemState.oledEnabled;
        if (!systemState.oledEnabled) {
            display.clearDisplay();
            display.display();
        }
        server.send(200, "text/plain", systemState.oledEnabled ? "OLED ON" : "OLED OFF");
        return;
    }
    server.send(400, "text/plain", "Unknown action");
}

void handleCalibrate() {
    if (!sensorData.bmp_valid) {
        server.send(500, "text/plain", "BMP180 not available");
        return;
    }

    int32_t pressurePa = bmp.readPressure();
    float pressureHpa = pressurePa / 100.0;
    float tempK = bmp.readTemperature() + 273.15;
    systemState.referencePressure = pressureHpa / pow(1.0 - (KNOWN_ALTITUDE_METERS * 0.0065) / tempK, 5.257);
    saveCalibrationToEEPROM();
    String msg = "Calibrated! Ref pressure: " + String(systemState.referencePressure, 2) + " hPa";
    server.send(200, "text/plain", msg);
}

void handleRestart() {
    server.send(200, "text/plain", "Restarting...");
    delay(1000);
    ESP.restart();
}

// ============================================================================
// EEPROM FUNCTIONS
// ============================================================================

void saveCalibrationToEEPROM() {
    uint16_t magic = EEPROM_MAGIC;
    EEPROM.put(EEPROM_ADDR_MAGIC, magic);
    EEPROM.put(EEPROM_ADDR_REF_PRESS, systemState.referencePressure);
    EEPROM.commit();
    Serial.println("[EEPROM] Calibration saved");
}

void loadCalibrationFromEEPROM() {
    uint16_t magic;
    EEPROM.get(EEPROM_ADDR_MAGIC, magic);

    if (magic == EEPROM_MAGIC) {
        EEPROM.get(EEPROM_ADDR_REF_PRESS, systemState.referencePressure);
        Serial.print("[EEPROM] Loaded ref pressure: ");
        Serial.print(systemState.referencePressure);
        Serial.println(" hPa");
    } else {
        systemState.referencePressure = 0;
        Serial.println("[EEPROM] No calibration found");
    }
}

// ============================================================================
// SENSOR FUNCTIONS
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
    float altitude = bmp.readAltitude(systemState.referencePressure * 100);
    
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
    sensorData.battery_percent = calculateBatteryPercent(sensorData.battery_voltage);
    sensorData.estimated_runtime_min = estimateRuntimeMinutes();
}

// ============================================================================
// SYSTEM STATISTICS & INTELLIGENCE
// ============================================================================

void updateSystemStats() {
    // Memory
    sensorData.free_heap = ESP.getFreeHeap();
    uint32_t total_heap = 81920;
    sensorData.heap_percent = 100 - ((sensorData.free_heap * 100) / total_heap);

    // WiFi
    if (WiFi.status() == WL_CONNECTED) {
        sensorData.wifi_rssi = WiFi.RSSI();
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

void updateCPULoad() {
    unsigned long currentTime = micros();
    unsigned long elapsed = currentTime - cpuTracker.lastMeasureTime;

    if (elapsed >= 1000000) { // Every 1 second
        float instantLoad = (cpuTracker.busyTime * 100.0) / cpuTracker.totalTime;
        // Smooth with exponential moving average
        if (cpuTracker.smoothedLoad == 0) {
            cpuTracker.smoothedLoad = instantLoad;
        } else {
            cpuTracker.smoothedLoad = (cpuTracker.smoothedLoad * 0.7) + (instantLoad * 0.3);
        }
        sensorData.cpu_load = cpuTracker.smoothedLoad;
        cpuTracker.busyTime = 0;
        cpuTracker.totalTime = 0;
        cpuTracker.lastMeasureTime = currentTime;
    }
}

void updateSystemHealth() {
    HealthState newHealth = HEALTH_NORMAL;

    // Check critical conditions
    if (!sensorData.bmp_valid || 
        sensorData.battery_voltage < BATTERY_CRITICAL_VOLTAGE ||
        sensorData.free_heap < 5000) {
        newHealth = HEALTH_CRITICAL;
    }
    // Check degraded conditions
    else if (!sensorData.dht_valid || 
             sensorData.wifi_quality < 30 || 
             sensorData.battery_voltage < BATTERY_LOW_VOLTAGE || 
             sensorData.free_heap < 10000 || 
             sensorData.cpu_load > 80) {
        newHealth = HEALTH_DEGRADED;
    }
    sensorData.health = newHealth;

    // Auto-adjust on critical battery
    if (sensorData.battery_voltage < BATTERY_CRITICAL_VOLTAGE && 
        systemState.currentProfile != PROFILE_ULTRA_LOW_POWER) {
        applyPowerProfile(PROFILE_ULTRA_LOW_POWER);
        Serial.println("[HEALTH] Critical battery - switched to ultra low power");
    }
}

void updateWeatherTrend() {
    static float lastPressure = 0;
    static unsigned long lastTrendUpdate = 0;

    unsigned long now = millis();
    if (now - lastTrendUpdate >= 3600000) { // Every hour
        if (lastPressure > 0) {
            float pressureChange = sensorData.pressure - lastPressure;
            sensorData.pressure_rate = pressureChange; // hPa per hour
            
            if (pressureChange > 0.5) {
                sensorData.trend = TREND_RISING;
            } else if (pressureChange < -0.5) {
                sensorData.trend = TREND_FALLING;
            } else {
                sensorData.trend = TREND_STABLE;
            }
        }
        lastPressure = sensorData.pressure;
        lastTrendUpdate = now;
    }
}

void updateHistory() {
    history.temperature[history.index] = sensorData.temperature;
    history.pressure[history.index] = sensorData.pressure;
    history.battery[history.index] = sensorData.battery_voltage;

    history.index++;
    if (history.index >= HISTORY_SIZE) {
        history.index = 0;
        history.full = true;
    }
}

float calculateBatteryPercent(float voltage) {
    if (voltage >= BATTERY_MAX_VOLTAGE) return 100;
    if (voltage <= BATTERY_MIN_VOLTAGE) return 0;

    float percent = ((voltage - BATTERY_MIN_VOLTAGE) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE)) * 100.0;
    return constrain(percent, 0, 100);
}

int estimateRuntimeMinutes() {
    // Estimate based on battery level and power profile
    float batteryCapacity = sensorData.battery_percent;

    // Estimated consumption per profile (mA)
    int consumption[] = {150, 100, 50}; // Performance, Balanced, Ultra Low Power
    
    // Assume 500mAh battery (adjust for your battery)
    float batteryMah = 500.0;
    float remainingMah = (batteryCapacity / 100.0) * batteryMah;
    int currentConsumption = consumption[systemState.currentProfile];
    if (currentConsumption == 0) return 0;
    float hoursRemaining = remainingMah / (float)currentConsumption;
    return (int)(hoursRemaining * 60);
}

void applyPowerProfile(PowerProfile profile) {
    systemState.currentProfile = profile;
    systemState.lastProfileChange = millis();

    Serial.print("[POWER] Profile changed to: ");
    Serial.println(profiles[profile].name);
}

// ============================================================================
// OLED DISPLAY
// ============================================================================

void updateOLED() {
    if (!systemState.oledEnabled) return;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    // Header
    display.setCursor(0, 0);
    display.print("ENV PRO v2.0");
    // Health indicator
    display.setCursor(95, 0);
    const char* healthChars[] = {"OK", "!", "!!"};
    display.print(healthChars[sensorData.health]);
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
    // Main data
    display.setCursor(0, 12);
    display.print("T:");
    display.print(sensorData.temperature, 1);
    display.print("C ");
    // Trend arrow
    const char* trendChars[] = {"-", "+", "-"};
    display.print(trendChars[sensorData.trend]);
    display.print(" H:");
    display.print(sensorData.humidity, 0);
    display.println("%");
    display.print("P:");
    display.print(sensorData.pressure, 0);
    display.print(" Alt:");
    display.print(sensorData.altitude, 0);
    display.println("m");
    // System stats
    display.drawLine(0, 30, 127, 30, SSD1306_WHITE);
    display.setCursor(0, 33);
    display.print("CPU:");
    display.print(sensorData.cpu_load, 0);
    display.print("% Mem:");
    display.print(sensorData.heap_percent);
    display.println("%");
    display.print("WiFi:");
    display.print(sensorData.wifi_quality);
    display.print("% Bat:");
    display.print(sensorData.battery_percent);
    display.println("%");
    // Profile & runtime
    display.drawLine(0, 51, 127, 51, SSD1306_WHITE);
    display.setCursor(0, 54);
    const char* profileShort[] = {"PERF", "BAL", "ULP"};
    display.print(profileShort[systemState.currentProfile]);
    if (sensorData.estimated_runtime_min > 0) {
        display.print(" RT:");
        display.print(sensorData.estimated_runtime_min);
        display.print("m");
    }
    display.display();
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
    loopStartTime = micros();
    unsigned long currentMillis = millis();

    // Handle OTA
    if (!systemState.otaInProgress) {
        ArduinoOTA.handle();
    }
    
    // Get active profile intervals
    ProfileConfig& profile = profiles[systemState.currentProfile];
    
    // Read sensors
    if (currentMillis - lastSensorRead >= profile.sensorInterval) {
        readBMP();
        readDHT();
        lastSensorRead = currentMillis;
    }
    
    // Read battery
    if (currentMillis - lastBatteryRead >= profile.batteryInterval) {
        readBattery();
        lastBatteryRead = currentMillis;
    }
    
    // Update system stats
    if (currentMillis - lastSystemStats >= profile.statsInterval) {
        updateSystemStats();
        updateSystemHealth();
        updateWeatherTrend();
        lastSystemStats = currentMillis;
    }
    
    // Update history (every 60 seconds)
    if (currentMillis - lastHistoryUpdate >= 60000) {
        updateHistory();
        lastHistoryUpdate = currentMillis;
    }
    
    // Update OLED
    if (currentMillis - lastOLEDUpdate >= profile.oledInterval) {
        updateOLED();
        lastOLEDUpdate = currentMillis;
    }
    
    // Handle web server
    server.handleClient();
    
    // Calculate CPU load timing
    unsigned long loopEndTime = micros();
    unsigned long busyTime = loopEndTime - loopStartTime;
    cpuTracker.busyTime += busyTime;
    
    yield();
    
    unsigned long afterYieldTime = micros();
    unsigned long totalLoopTime = afterYieldTime - loopStartTime;
    cpuTracker.totalTime += totalLoopTime;
    
    updateCPULoad();
}