/*
 * Testirockett - Fixed Version
 * ESP8266 Dashboard System
 * 
 * Fixed compilation errors:
 * - Added wifi_ssid to SensorData struct
 * - Added pressure_rate to SensorData struct
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30100_PulseOximeter.h>
#include <FastLED.h>

// ==================== CONFIGURATION ====================
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Pin definitions
#define BMP_SDA D2
#define BMP_SCL D1
#define ONE_WIRE_BUS D4
#define LED_PIN D3
#define NUM_LEDS 8
#define BUZZER_PIN D5
#define RELAY_PIN D6

// ==================== GLOBAL OBJECTS ====================
ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Adafruit_BMP280 bmp;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
PulseOximeter pox;
CRGB leds[NUM_LEDS];

// ==================== STRUCTURES ====================
struct SensorData {
    // Time
    unsigned long timestamp;
    
    // Environmental
    float temperature;
    float humidity;
    float pressure;
    float altitude;
    float pressure_rate;  // FIXED: Added missing member
    
    // Health
    float heart_rate;
    float spo2;
    float body_temp;
    
    // System
    float cpu_temp;
    int wifi_rssi;
    String wifi_ssid;  // FIXED: Added missing member
    String ip_address;
    float battery_voltage;
    
    // Advanced metrics
    float vertical_speed;
    float g_force;
    float acceleration;
    float velocity;
};

struct ConfigData {
    bool led_enabled = true;
    bool buzzer_enabled = false;
    bool relay_enabled = false;
    int update_interval = 1000;
    int alarm_threshold_temp = 50;
    int alarm_threshold_hr = 120;
};

// ==================== GLOBAL VARIABLES ====================
SensorData sensorData;
ConfigData config;
unsigned long lastUpdate = 0;
unsigned long lastPressureCalc = 0;
float lastPressure = 0;
float lastAltitude = 0;
unsigned long lastHealthUpdate = 0;
bool isConnected = false;

// ==================== SETUP FUNCTIONS ====================
void setup() {
    Serial.begin(115200);
    Serial.println("\n\nStarting Testirockett System...");
    
    // Initialize pins
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RELAY_PIN, LOW);
    
    // Initialize LED strip
    FastLED.addLeds<WS2812, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.clear();
    FastLED.show();
    
    // Initialize I2C for BMP280
    Wire.begin(BMP_SDA, BMP_SCL);
    
    // Initialize BMP280
    if (!bmp.begin(0x76)) {
        Serial.println("Could not find a valid BMP280 sensor, check wiring!");
    } else {
        Serial.println("BMP280 initialized successfully");
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                       Adafruit_BMP280::SAMPLING_X2,
                       Adafruit_BMP280::SAMPLING_X16,
                       Adafruit_BMP280::FILTER_X16,
                       Adafruit_BMP280::STANDBY_MS_500);
    }
    
    // Initialize temperature sensors
    sensors.begin();
    sensors.setResolution(12);
    
    // Initialize MAX30100
    if (!pox.begin()) {
        Serial.println("FAILED: MAX30100 initialization failed");
    } else {
        Serial.println("MAX30100 initialized successfully");
        pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
        pox.setSampleRate(MAX30100_SAMPLERATE_100HZ);
        pox.setPulseWidth(MAX30100_PULSEWIDTH_1600US_ADC_2048);
    }
    
    // Connect to WiFi
    connectWiFi();
    
    // Setup web server
    setupWebServer();
    
    // Setup WebSocket
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    
    // Setup mDNS
    if (MDNS.begin("testirockett")) {
        Serial.println("mDNS responder started: http://testirockett.local");
    }
    
    Serial.println("Setup complete! System ready.");
    Serial.println("Access the dashboard at: http://" + WiFi.localIP().toString());
    
    // Set initial sensor values
    initializeSensorData();
}

void connectWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
    
    Serial.println("\nWiFi connected!");
    Serial.println("IP address: " + WiFi.localIP().toString());
    digitalWrite(LED_BUILTIN, HIGH);
    isConnected = true;
}

void setupWebServer() {
    // Serve main page
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", getMainPage());
    });
    
    // API endpoints
    server.on("/api/data", HTTP_GET, []() {
        String json = getSensorDataJSON();
        server.send(200, "application/json", json);
    });
    
    server.on("/api/config", HTTP_GET, []() {
        String json = getConfigJSON();
        server.send(200, "application/json", json);
    });
    
    server.on("/api/config", HTTP_POST, []() {
        if (server.hasArg("plain")) {
            String body = server.arg("plain");
            updateConfig(body);
            server.send(200, "application/json", "{\"status\":\"success\"}");
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No data provided\"}");
        }
    });
    
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not Found");
    });
    
    server.begin();
    Serial.println("HTTP server started");
}

void initializeSensorData() {
    sensorData.timestamp = millis();
    sensorData.temperature = 0;
    sensorData.humidity = 0;
    sensorData.pressure = 0;
    sensorData.altitude = 0;
    sensorData.pressure_rate = 0;  // FIXED: Initialize new member
    sensorData.heart_rate = 0;
    sensorData.spo2 = 0;
    sensorData.body_temp = 0;
    sensorData.cpu_temp = 0;
    sensorData.wifi_rssi = 0;
    sensorData.wifi_ssid = "";  // FIXED: Initialize new member
    sensorData.ip_address = "";
    sensorData.battery_voltage = 0;
    sensorData.vertical_speed = 0;
    sensorData.g_force = 0;
    sensorData.acceleration = 0;
    sensorData.velocity = 0;
}

// ==================== MAIN LOOP ====================
void loop() {
    // Handle web server
    server.handleClient();
    webSocket.loop();
    
    // Update sensor data at configured interval
    if (millis() - lastUpdate >= config.update_interval) {
        readAllSensors();
        calculateAdvancedMetrics();
        readSystemStats();
        updateLEDs();
        checkAlarms();
        
        // Send data to WebSocket clients
        broadcastSensorData();
        
        lastUpdate = millis();
    }
    
    // Update health sensors more frequently
    if (millis() - lastHealthUpdate >= 100) {
        updateHealthSensors();
        lastHealthUpdate = millis();
    }
    
    delay(10);
}

// ==================== SENSOR READING ====================
void readAllSensors() {
    // Read environmental sensors
    readEnvironmentalSensors();
    
    // Read body temperature
    readBodyTemperature();
    
    sensorData.timestamp = millis();
}

void readEnvironmentalSensors() {
    // Read BMP280
    sensorData.temperature = bmp.readTemperature();
    sensorData.pressure = bmp.readPressure() / 100.0F; // Convert to hPa
    sensorData.altitude = bmp.readAltitude(1013.25); // Adjust based on sea level pressure
}

void readBodyTemperature() {
    sensors.requestTemperatures();
    sensorData.body_temp = sensors.getTempCByIndex(0);
    
    // Handle sensor errors
    if (sensorData.body_temp == -127.0 || sensorData.body_temp == 85.0) {
        sensorData.body_temp = 0; // Set to 0 if sensor error
    }
}

void updateHealthSensors() {
    // Update MAX30100
    pox.update();
    
    if (pox.begin()) {
        sensorData.heart_rate = pox.getHeartRate();
        sensorData.spo2 = pox.getSpO2();
    }
}

void readSystemStats() {
    // Read WiFi info
    if (WiFi.status() == WL_CONNECTED) {
        sensorData.wifi_rssi = WiFi.RSSI();
        sensorData.wifi_ssid = WiFi.SSID();  // FIXED: Now valid
        sensorData.ip_address = WiFi.localIP().toString();
    } else {
        sensorData.wifi_rssi = -100;
        sensorData.wifi_ssid = "Disconnected";  // FIXED: Now valid
        sensorData.ip_address = "Disconnected";
        
        // Try to reconnect
        if (!isConnected) {
            connectWiFi();
        }
    }
    
    // CPU temperature (ESP8266 doesn't have built-in temp sensor, so we estimate)
    sensorData.cpu_temp = 25.0; // Placeholder
    
    // Battery voltage (if battery monitor is connected to analog pin)
    // sensorData.battery_voltage = analogRead(A0) * (3.3 / 1024.0) * 2; // Adjust voltage divider ratio
}

void calculateAdvancedMetrics() {
    static float prevAltitude = 0;
    static unsigned long prevTime = 0;
    
    unsigned long currentTime = millis();
    float deltaTime = (currentTime - prevTime) / 1000.0; // Convert to seconds
    
    if (deltaTime > 0) {
        // Calculate vertical speed (rate of altitude change)
        sensorData.vertical_speed = (sensorData.altitude - prevAltitude) / deltaTime;
        
        // Calculate pressure rate (rate of pressure change)
        if (lastPressureCalc > 0) {
            float pressureChange = (sensorData.pressure - lastPressure) / deltaTime;
            sensorData.pressure_rate = pressureChange;  // FIXED: Now valid
        } else {
            sensorData.pressure_rate = 0;  // FIXED: Now valid
        }
        
        // Estimate G-force (simplified calculation)
        float velocityChange = sensorData.vertical_speed - (prevAltitude / deltaTime);
        sensorData.acceleration = velocityChange / deltaTime;
        sensorData.g_force = sensorData.acceleration / 9.81; // Convert to G
        
        // Calculate velocity (magnitude)
        sensorData.velocity = abs(sensorData.vertical_speed);
        
        prevAltitude = sensorData.altitude;
        prevTime = currentTime;
        lastPressure = sensorData.pressure;
        lastPressureCalc = currentTime;
    }
}

// ==================== LED CONTROL ====================
void updateLEDs() {
    if (!config.led_enabled) {
        FastLED.clear();
        FastLED.show();
        return;
    }
    
    // Health status LED (first 4 LEDs)
    updateHealthLEDs();
    
    // System status LED (last 4 LEDs)
    updateSystemLEDs();
    
    FastLED.show();
}

void updateHealthLEDs() {
    // Heart rate visualization
    int hr = constrain(sensorData.heart_rate, 40, 200);
    int hrLED = map(hr, 40, 200, 0, 255);
    
    // SpO2 visualization
    int spo2 = constrain(sensorData.spo2, 70, 100);
    int spo2LED = map(spo2, 70, 100, 0, 255);
    
    // Temperature visualization
    float temp = constrain(sensorData.body_temp, 30, 45);
    int tempLED = map(temp * 10, 300, 450, 0, 255);
    
    // Set colors
    leds[0] = CRGB(hrLED, 0, 0);        // Red for HR
    leds[1] = CRGB(0, spo2LED, 0);      // Green for SpO2
    leds[2] = CRGB(0, 0, tempLED);      // Blue for Temp
    leds[3] = CRGB(hrLED, spo2LED, 0);  // Yellow for combined
}

void updateSystemLEDs() {
    // WiFi signal strength
    int rssi = constrain(sensorData.wifi_rssi, -100, -50);
    int wifiLED = map(rssi, -100, -50, 0, 255);
    
    // Pressure change rate
    float pressureRate = constrain(abs(sensorData.pressure_rate), 0, 10);
    int pressureLED = map(pressureRate * 10, 0, 100, 0, 255);
    
    // Altitude
    float altitude = constrain(sensorData.altitude, 0, 5000);
    int altitudeLED = map(altitude, 0, 5000, 0, 255);
    
    // System status
    int statusLED = isConnected ? 255 : 50;
    
    // Set colors
    leds[4] = CRGB(wifiLED, 0, 0);           // Red for WiFi
    leds[5] = CRGB(0, pressureLED, 0);       // Green for pressure rate
    leds[6] = CRGB(0, 0, altitudeLED);       // Blue for altitude
    leds[7] = CRGB(statusLED, statusLED, 0); // Yellow for status
}

// ==================== ALARM SYSTEM ====================
void checkAlarms() {
    bool alarm = false;
    
    // Temperature alarm
    if (sensorData.body_temp > config.alarm_threshold_temp && sensorData.body_temp < 50) {
        alarm = true;
    }
    
    // Heart rate alarm
    if (sensorData.heart_rate > config.alarm_threshold_hr && sensorData.heart_rate < 250) {
        alarm = true;
    }
    
    // SpO2 alarm
    if (sensorData.spo2 < 90 && sensorData.spo2 > 50) {
        alarm = true;
    }
    
    if (alarm && config.buzzer_enabled) {
        tone(BUZZER_PIN, 1000, 200);
        delay(200);
        tone(BUZZER_PIN, 1500, 200);
        delay(200);
    }
    
    // Relay control based on alarm
    digitalWrite(RELAY_PIN, alarm && config.relay_enabled ? HIGH : LOW);
}

// ==================== WEBSOCKET ====================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.printf("WebSocket client #%u connected\n", num);
            break;
        case WStype_DISCONNECTED:
            Serial.printf("WebSocket client #%u disconnected\n", num);
            break;
        case WStype_TEXT:
            // Handle incoming WebSocket messages
            break;
        default:
            break;
    }
}

void broadcastSensorData() {
    String json = getSensorDataJSON();
    webSocket.broadcastTXT(json);
}

// ==================== JSON GENERATION ====================
String getSensorDataJSON() {
    StaticJsonDocument<1024> doc;
    
    doc["timestamp"] = sensorData.timestamp;
    doc["temperature"] = sensorData.temperature;
    doc["humidity"] = sensorData.humidity;
    doc["pressure"] = sensorData.pressure;
    doc["altitude"] = sensorData.altitude;
    doc["pressure_rate"] = sensorData.pressure_rate;  // FIXED: Now included
    doc["heart_rate"] = sensorData.heart_rate;
    doc["spo2"] = sensorData.spo2;
    doc["body_temp"] = sensorData.body_temp;
    doc["cpu_temp"] = sensorData.cpu_temp;
    doc["wifi_rssi"] = sensorData.wifi_rssi;
    doc["wifi_ssid"] = sensorData.wifi_ssid;  // FIXED: Now included
    doc["ip_address"] = sensorData.ip_address;
    doc["battery_voltage"] = sensorData.battery_voltage;
    doc["vertical_speed"] = sensorData.vertical_speed;
    doc["g_force"] = sensorData.g_force;
    doc["acceleration"] = sensorData.acceleration;
    doc["velocity"] = sensorData.velocity;
    
    String json;
    serializeJson(doc, json);
    return json;
}

String getConfigJSON() {
    StaticJsonDocument<256> doc;
    
    doc["led_enabled"] = config.led_enabled;
    doc["buzzer_enabled"] = config.buzzer_enabled;
    doc["relay_enabled"] = config.relay_enabled;
    doc["update_interval"] = config.update_interval;
    doc["alarm_threshold_temp"] = config.alarm_threshold_temp;
    doc["alarm_threshold_hr"] = config.alarm_threshold_hr;
    
    String json;
    serializeJson(doc, json);
    return json;
}

void updateConfig(String json) {
    StaticJsonDocument<256> doc;
    deserializeJson(doc, json);
    
    config.led_enabled = doc["led_enabled"] | config.led_enabled;
    config.buzzer_enabled = doc["buzzer_enabled"] | config.buzzer_enabled;
    config.relay_enabled = doc["relay_enabled"] | config.relay_enabled;
    config.update_interval = doc["update_interval"] | config.update_interval;
    config.alarm_threshold_temp = doc["alarm_threshold_temp"] | config.alarm_threshold_temp;
    config.alarm_threshold_hr = doc["alarm_threshold_hr"] | config.alarm_threshold_hr;
}

// ==================== HTML PAGE ====================
String getMainPage() {
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>Testirockett Dashboard</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f0f0f0; }
        .container { max-width: 1200px; margin: 0 auto; }
        .card { background: white; border-radius: 10px; padding: 20px; margin: 10px 0; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; }
        .value { font-size: 2em; font-weight: bold; color: #333; }
        .label { color: #666; font-size: 0.9em; }
        .status { padding: 5px 10px; border-radius: 5px; color: white; font-weight: bold; }
        .status.ok { background: #4CAF50; }
        .status.warning { background: #FF9800; }
        .status.error { background: #F44336; }
        .gauge { width: 100%; height: 200px; background: #e0e0e0; border-radius: 10px; display: flex; align-items: center; justify-content: center; position: relative; overflow: hidden; }
        .gauge-fill { position: absolute; bottom: 0; left: 0; right: 0; background: linear-gradient(to top, #4CAF50, #8BC34A); transition: height 0.3s; }
        .gauge-value { position: relative; z-index: 1; font-size: 1.5em; font-weight: bold; }
        .config { display: flex; flex-wrap: wrap; gap: 10px; }
        .config-item { flex: 1; min-width: 200px; }
        .config-item label { display: block; margin-bottom: 5px; font-weight: bold; }
        .config-item input, .config-item select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }
        button { background: #2196F3; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; font-size: 16px; }
        button:hover { background: #1976D2; }
        .ws-indicator { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-left: 10px; }
        .ws-indicator.connected { background: #4CAF50; animation: pulse 1s infinite; }
        .ws-indicator.disconnected { background: #F44336; }
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <h1>Testirockett Dashboard <span class="ws-indicator" id="wsIndicator"></span></h1>
            <p>Real-time monitoring system for Dashduino8266</p>
        </div>
        
        <div class="grid">
            <div class="card">
                <div class="label">Heart Rate</div>
                <div class="value" id="heartRate">--</div>
                <div class="label">BPM</div>
                <span class="status" id="hrStatus">Unknown</span>
            </div>
            
            <div class="card">
                <div class="label">SpO2</div>
                <div class="value" id="spo2">--</div>
                <div class="label">%</div>
                <span class="status" id="spo2Status">Unknown</span>
            </div>
            
            <div class="card">
                <div class="label">Body Temperature</div>
                <div class="value" id="bodyTemp">--</div>
                <div class="label">°C</div>
                <span class="status" id="tempStatus">Unknown</span>
            </div>
            
            <div class="card">
                <div class="label">Environmental Temperature</div>
                <div class="value" id="envTemp">--</div>
                <div class="label">°C</div>
            </div>
            
            <div class="card">
                <div class="label">Pressure</div>
                <div class="value" id="pressure">--</div>
                <div class="label">hPa</div>
            </div>
            
            <div class="card">
                <div class="label">Altitude</div>
                <div class="value" id="altitude">--</div>
                <div class="label">m</div>
            </div>
            
            <div class="card">
                <div class="label">Pressure Rate</div>
                <div class="value" id="pressureRate">--</div>
                <div class="label">hPa/s</div>
            </div>
            
            <div class="card">
                <div class="label">Vertical Speed</div>
                <div class="value" id="verticalSpeed">--</div>
                <div class="label">m/s</div>
            </div>
            
            <div class="card">
                <div class="label">G-Force</div>
                <div class="value" id="gForce">--</div>
                <div class="label">G</div>
            </div>
            
            <div class="card">
                <div class="label">WiFi Status</div>
                <div class="value" id="wifiSSID">Disconnected</div>
                <div class="label" id="wifiRSSI">RSSI: -- dBm</div>
                <span class="status" id="wifiStatus">Disconnected</span>
            </div>
            
            <div class="card">
                <div class="label">IP Address</div>
                <div class="value" id="ipAddress">--</div>
                <div class="label">Local Network</div>
            </div>
            
            <div class="card">
                <div class="label">System Uptime</div>
                <div class="value" id="uptime">00:00:00</div>
                <div class="label">HH:MM:SS</div>
            </div>
        </div>
        
        <div class="card">
            <h2>Configuration</h2>
            <div class="config">
                <div class="config-item">
                    <label>LED Enabled</label>
                    <select id="ledEnabled">
                        <option value="true">On</option>
                        <option value="false">Off</option>
                    </select>
                </div>
                
                <div class="config-item">
                    <label>Buzzer Enabled</label>
                    <select id="buzzerEnabled">
                        <option value="true">On</option>
                        <option value="false">Off</option>
                    </select>
                </div>
                
                <div class="config-item">
                    <label>Relay Enabled</label>
                    <select id="relayEnabled">
                        <option value="true">On</option>
                        <option value="false">Off</option>
                    </select>
                </div>
                
                <div class="config-item">
                    <label>Update Interval (ms)</label>
                    <input type="number" id="updateInterval" min="100" max="10000" step="100">
                </div>
                
                <div class="config-item">
                    <label>Temperature Alarm (°C)</label>
                    <input type="number" id="alarmTemp" min="30" max="60">
                </div>
                
                <div class="config-item">
                    <label>Heart Rate Alarm (BPM)</label>
                    <input type="number" id="alarmHR" min="80" max="200">
                </div>
                
                <div class="config-item">
                    <button onclick="saveConfig()">Save Configuration</button>
                </div>
            </div>
        </div>
    </div>
    
    <script>
        let ws;
        let startTime = Date.now();
        
        function connectWebSocket() {
            ws = new WebSocket('ws://' + window.location.hostname + ':81');
            
            ws.onopen = function() {
                console.log('WebSocket connected');
                document.getElementById('wsIndicator').className = 'ws-indicator connected';
            };
            
            ws.onmessage = function(event) {
                const data = JSON.parse(event.data);
                updateDashboard(data);
            };
            
            ws.onclose = function() {
                console.log('WebSocket disconnected');
                document.getElementById('wsIndicator').className = 'ws-indicator disconnected';
                setTimeout(connectWebSocket, 3000);
            };
            
            ws.onerror = function(error) {
                console.error('WebSocket error:', error);
            };
        }
        
        function updateDashboard(data) {
            // Update values
            document.getElementById('heartRate').textContent = data.heart_rate.toFixed(0);
            document.getElementById('spo2').textContent = data.spo2.toFixed(0);
            document.getElementById('bodyTemp').textContent = data.body_temp.toFixed(1);
            document.getElementById('envTemp').textContent = data.temperature.toFixed(1);
            document.getElementById('pressure').textContent = data.pressure.toFixed(2);
            document.getElementById('altitude').textContent = data.altitude.toFixed(1);
            document.getElementById('pressureRate').textContent = data.pressure_rate.toFixed(3);
            document.getElementById('verticalSpeed').textContent = data.vertical_speed.toFixed(2);
            document.getElementById('gForce').textContent = data.g_force.toFixed(2);
            document.getElementById('wifiSSID').textContent = data.wifi_ssid;
            document.getElementById('wifiRSSI').textContent = 'RSSI: ' + data.wifi_rssi + ' dBm';
            document.getElementById('ipAddress').textContent = data.ip_address;
            
            // Update uptime
            const uptime = Math.floor((Date.now() - startTime) / 1000);
            const hours = Math.floor(uptime / 3600);
            const minutes = Math.floor((uptime % 3600) / 60);
            const seconds = uptime % 60;
            document.getElementById('uptime').textContent = 
                String(hours).padStart(2, '0') + ':' +
                String(minutes).padStart(2, '0') + ':' +
                String(seconds).padStart(2, '0');
            
            // Update status indicators
            updateStatus('hrStatus', data.heart_rate, 60, 100, 120);
            updateStatus('spo2Status', data.spo2, 90, 95, 100);
            updateStatus('tempStatus', data.body_temp, 35, 37.5, 39);
            updateWiFiStatus(data.wifi_rssi);
        }
        
        function updateStatus(elementId, value, low, normal, high) {
            const element = document.getElementById(elementId);
            if (value < low || value > high) {
                element.textContent = 'Critical';
                element.className = 'status error';
            } else if (value < normal) {
                element.textContent = 'Warning';
                element.className = 'status warning';
            } else {
                element.textContent = 'Normal';
                element.className = 'status ok';
            }
        }
        
        function updateWiFiStatus(rssi) {
            const element = document.getElementById('wifiStatus');
            if (rssi > -50) {
                element.textContent = 'Excellent';
                element.className = 'status ok';
            } else if (rssi > -70) {
                element.textContent = 'Good';
                element.className = 'status ok';
            } else if (rssi > -85) {
                element.textContent = 'Fair';
                element.className = 'status warning';
            } else {
                element.textContent = 'Poor';
                element.className = 'status error';
            }
        }
        
        function loadConfig() {
            fetch('/api/config')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('ledEnabled').value = data.led_enabled;
                    document.getElementById('buzzerEnabled').value = data.buzzer_enabled;
                    document.getElementById('relayEnabled').value = data.relay_enabled;
                    document.getElementById('updateInterval').value = data.update_interval;
                    document.getElementById('alarmTemp').value = data.alarm_threshold_temp;
                    document.getElementById('alarmHR').value = data.alarm_threshold_hr;
                });
        }
        
        function saveConfig() {
            const config = {
                led_enabled: document.getElementById('ledEnabled').value === 'true',
                buzzer_enabled: document.getElementById('buzzerEnabled').value === 'true',
                relay_enabled: document.getElementById('relayEnabled').value === 'true',
                update_interval: parseInt(document.getElementById('updateInterval').value),
                alarm_threshold_temp: parseInt(document.getElementById('alarmTemp').value),
                alarm_threshold_hr: parseInt(document.getElementById('alarmHR').value)
            };
            
            fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(config)
            })
            .then(response => response.json())
            .then(data => {
                if (data.status === 'success') {
                    alert('Configuration saved!');
                } else {
                    alert('Error saving configuration');
                }
            });
        }
        
        // Initialize
        window.onload = function() {
            loadConfig();
            connectWebSocket();
        };
    </script>
</body>
</html>
    )HTML";
    
    return html;
}