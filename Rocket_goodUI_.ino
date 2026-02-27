/*
 * ESP8266 Mini Satellite "CanSat" Firmware
 * Version: 3.0 (NASA Edition)
 * * Hardware:
 * - ESP8266 (NodeMCU/Wemos)
 * - BMP180 (I2C)
 * - DHT11 (GPIO14)
 * - OLED SSD1306 (I2C)
 * - Internal Flash (LittleFS) for Data Logging
 * * Features:
 * - Black Box Data Logging (.csv)
 * - Dual WiFi Mode (AP + Station)
 * - Professional Mission Control Dashboard
 * - High-Precision Charts
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <LittleFS.h> // 

// ============================================================================
// CONFIGURATION
// ============================================================================

// 1. Home WiFi (Optional - for ground testing)
const char* WIFI_SSID = "-WIFI-";
const char* WIFI_PASS = "1XXXXXXXX2";

// 2. Satellite Hotspot (Field Mode)
const char* AP_SSID = "NASA-SAT-1";
const char* AP_PASS = "mission2024";

// Pins
#define I2C_SDA         4  // D2
#define I2C_SCL         5  // D1
#define DHT_PIN         14 // D5
#define BATTERY_PIN     A0

// Sensor & Display Config
#define DHT_TYPE        DHT11
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDR       0x3C

// Logging Config
#define LOG_FILENAME    "/mission_log.csv"
#define LOG_INTERVAL    2000 // Log every 2 seconds

// Objects
Adafruit_BMP085 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);

// Data Structure
struct MissionData {
    unsigned long timestamp;
    String missionTime;
    float temp;
    float humidity;
    float pressure;
    float altitude;
    float batteryV;
    int batteryPct;
    bool dht_ok;
    bool bmp_ok;
} data;

// Global Variables
unsigned long missionStartTime = 0;
unsigned long lastLogTime = 0;
float refPressure = 1013.25; // Will be auto-calibrated

// ============================================================================
// FILE SYSTEM (BLACK BOX)
// ============================================================================

void setupLogging() {
    if (!LittleFS.begin()) {
        Serial.println("[FS] Failed to mount file system");
        return;
    }
    Serial.println("[FS] File System Mounted");

    // Check if file exists, if not create headers
    if (!LittleFS.exists(LOG_FILENAME)) {
        File logFile = LittleFS.open(LOG_FILENAME, "w");
        if (logFile) {
            logFile.println("Time,MissionTime,Temp(C),Humidity(%),Pressure(hPa),Altitude(m),Battery(V)");
            logFile.close();
            Serial.println("[FS] Created new log file");
        }
    }
}

void logDataToStorage() {
    File logFile = LittleFS.open(LOG_FILENAME, "a"); // Append mode
    if (logFile) {
        logFile.print(millis());
        logFile.print(",");
        logFile.print(data.missionTime);
        logFile.print(",");
        logFile.print(data.temp);
        logFile.print(",");
        logFile.print(data.humidity);
        logFile.print(",");
        logFile.print(data.pressure);
        logFile.print(",");
        logFile.print(data.altitude);
        logFile.print(",");
        logFile.println(data.batteryV);
        logFile.close();
        // Serial.println("[FS] Data saved"); // Uncomment for debugging
    }
}

// ============================================================================
// UTILITIES
// ============================================================================

String getMissionTime() {
    unsigned long now = millis() - missionStartTime;
    unsigned long seconds = now / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    
    seconds %= 60;
    minutes %= 60;
    
    char buffer[20];
    sprintf(buffer, "T+%02lu:%02lu:%02lu", hours, minutes, seconds);
    return String(buffer);
}

float readBattery() {
    // 4.2V is max, using divider if needed. Adjust multiplier based on your resistor divider.
    // If direct connection (not recommended for >3.3V), max is 1.0V input = 1023
    // Assuming internal divider or specific hardware:
    int raw = analogRead(BATTERY_PIN);
    return (raw / 1023.0) * 4.2; 
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================

void handleRoot() {
    // HTML stored in PROGMEM to save RAM
    String html = F(R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>NASA-SAT Mission Control</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root { --bg: #0b0d17; --card: #15192b; --text: #e0e6ed; --accent: #00f2ff; --alert: #ff0055; --success: #00ff88; }
        body { background-color: var(--bg); color: var(--text); font-family: 'Segoe UI', 'Roboto Mono', monospace; margin: 0; padding: 20px; overflow-x: hidden; }
        .header { display: flex; justify-content: space-between; align-items: center; border-bottom: 2px solid var(--accent); padding-bottom: 15px; margin-bottom: 20px; }
        .header h1 { margin: 0; text-transform: uppercase; letter-spacing: 2px; font-size: 1.5rem; text-shadow: 0 0 10px var(--accent); }
        .blink { animation: blinker 1.5s linear infinite; color: var(--alert); font-weight: bold; }
        @keyframes blinker { 50% { opacity: 0; } }
        
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin-bottom: 20px; }
        .card { background: var(--card); border: 1px solid rgba(0, 242, 255, 0.2); padding: 15px; border-radius: 5px; position: relative; overflow: hidden; }
        .card::before { content: ''; position: absolute; top: 0; left: 0; width: 100%; height: 2px; background: linear-gradient(90deg, transparent, var(--accent), transparent); }
        .card-title { font-size: 0.8rem; text-transform: uppercase; color: #8b9bb4; margin-bottom: 5px; }
        .card-value { font-size: 2rem; font-weight: bold; }
        .card-unit { font-size: 1rem; color: var(--accent); }
        
        .charts-area { display: grid; grid-template-columns: repeat(auto-fit, minmax(400px, 1fr)); gap: 20px; }
        .chart-container { background: var(--card); padding: 15px; border-radius: 5px; border: 1px solid rgba(255,255,255,0.05); height: 300px; }
        
        .controls { margin-top: 30px; display: flex; gap: 10px; flex-wrap: wrap; }
        .btn { background: rgba(0, 242, 255, 0.1); border: 1px solid var(--accent); color: var(--accent); padding: 10px 20px; cursor: pointer; font-family: inherit; font-weight: bold; transition: 0.3s; text-decoration: none; display: inline-block; }
        .btn:hover { background: var(--accent); color: var(--bg); box-shadow: 0 0 15px var(--accent); }
        .btn-danger { border-color: var(--alert); color: var(--alert); background: rgba(255, 0, 85, 0.1); }
        .btn-danger:hover { background: var(--alert); color: white; box-shadow: 0 0 15px var(--alert); }

        /* Responsive */
        @media (max-width: 600px) { .header { flex-direction: column; text-align: center; } .charts-area { grid-template-columns: 1fr; } }
    </style>
</head>
<body>
    <div class="header">
        <div>
            <h1>🛰️ Mission Control</h1>
            <small>ESP8266 GROUND STATION | <span style="color:var(--success)">CONNECTED</span></small>
        </div>
        <div style="text-align: right;">
            <div id="missionTime" style="font-size: 1.5rem; font-weight: bold;">T+00:00:00</div>
            <div id="recStatus" class="blink">● RECORDING</div>
        </div>
    </div>

    <div class="grid">
        <div class="card">
            <div class="card-title">Altitude</div>
            <span id="alt" class="card-value">--</span> <span class="card-unit">m</span>
        </div>
        <div class="card">
            <div class="card-title">Pressure</div>
            <span id="press" class="card-value">--</span> <span class="card-unit">hPa</span>
        </div>
        <div class="card">
            <div class="card-title">Temperature</div>
            <span id="temp" class="card-value">--</span> <span class="card-unit">°C</span>
        </div>
        <div class="card">
            <div class="card-title">Humidity</div>
            <span id="hum" class="card-value">--</span> <span class="card-unit">%</span>
        </div>
        <div class="card">
            <div class="card-title">Battery</div>
            <span id="bat" class="card-value">--</span> <span class="card-unit">V</span>
        </div>
    </div>

    <div class="charts-area">
        <div class="chart-container"><canvas id="altChart"></canvas></div>
        <div class="chart-container"><canvas id="tempChart"></canvas></div>
    </div>

    <div class="controls">
        <a href="/download" class="btn">⬇ DOWNLOAD FLIGHT LOG (CSV)</a>
        <a href="/clear" class="btn btn-danger" onclick="return confirm('Are you sure you want to delete flight data?')">⚠ CLEAR BLACKBOX</a>
        <button class="btn" onclick="calibrate()">⌖ CALIBRATE ALTITUDE</button>
    </div>

    <script>
        // Charts Setup
        Chart.defaults.color = '#8b9bb4';
        Chart.defaults.borderColor = 'rgba(255,255,255,0.05)';
        
        const commonOptions = {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            elements: { point: { radius: 1 } },
            plugins: { legend: { display: true } },
            scales: { x: { display: false } }
        };

        const ctxAlt = document.getElementById('altChart').getContext('2d');
        const altChart = new Chart(ctxAlt, {
            type: 'line',
            data: { labels: [], datasets: [{ label: 'Altitude (m)', data: [], borderColor: '#00f2ff', backgroundColor: 'rgba(0, 242, 255, 0.1)', fill: true, tension: 0.3 }] },
            options: commonOptions
        });

        const ctxTemp = document.getElementById('tempChart').getContext('2d');
        const tempChart = new Chart(ctxTemp, {
            type: 'line',
            data: { labels: [], datasets: [
                { label: 'Temp (°C)', data: [], borderColor: '#ff0055', tension: 0.3, yAxisID: 'y' },
                { label: 'Pressure (hPa)', data: [], borderColor: '#00ff88', tension: 0.3, yAxisID: 'y1' }
            ]},
            options: {
                ...commonOptions,
                scales: {
                    y: { type: 'linear', display: true, position: 'left' },
                    y1: { type: 'linear', display: true, position: 'right', grid: { drawOnChartArea: false } }
                }
            }
        });

        function updateData() {
            fetch('/data').then(response => response.json()).then(data => {
                document.getElementById('missionTime').innerText = data.time;
                document.getElementById('alt').innerText = data.alt.toFixed(1);
                document.getElementById('press').innerText = data.press.toFixed(1);
                document.getElementById('temp').innerText = data.temp.toFixed(1);
                document.getElementById('hum').innerText = data.hum.toFixed(0);
                document.getElementById('bat').innerText = data.bat.toFixed(2);

                // Update Charts
                const now = new Date().toLocaleTimeString();
                if(altChart.data.labels.length > 50) {
                    altChart.data.labels.shift();
                    altChart.data.datasets[0].data.shift();
                    tempChart.data.labels.shift();
                    tempChart.data.datasets[0].data.shift();
                    tempChart.data.datasets[1].data.shift();
                }

                altChart.data.labels.push(now);
                altChart.data.datasets[0].data.push(data.alt);
                altChart.update();

                tempChart.data.labels.push(now);
                tempChart.data.datasets[0].data.push(data.temp);
                tempChart.data.datasets[1].data.push(data.press);
                tempChart.update();
            });
        }

        function calibrate() {
            fetch('/calibrate').then(alert('Altitude Calibrated to 0m reference.'));
        }

        setInterval(updateData, 1000);
    </script>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void handleData() {
    String json = "{";
    json += "\"time\":\"" + data.missionTime + "\",";
    json += "\"alt\":" + String(data.altitude) + ",";
    json += "\"press\":" + String(data.pressure) + ",";
    json += "\"temp\":" + String(data.temp) + ",";
    json += "\"hum\":" + String(data.humidity) + ",";
    json += "\"bat\":" + String(data.batteryV);
    json += "}";
    server.send(200, "application/json", json);
}

void handleDownload() {
    File file = LittleFS.open(LOG_FILENAME, "r");
    if (!file) {
        server.send(404, "text/plain", "Log file not found");
        return;
    }
    server.streamFile(file, "text/csv");
    file.close();
}

void handleClear() {
    LittleFS.remove(LOG_FILENAME);
    setupLogging(); // Recreate header
    server.send(200, "text/plain", "Logs Cleared");
}

void handleCalibrate() {
    int32_t p = bmp.readPressure();
    refPressure = p / 100.0;
    server.send(200, "text/plain", "OK");
}

// ============================================================================
// MAIN SETUP & LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA, I2C_SCL);
    
    // Init Hardware
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0,20);
    display.println(F("BOOTING SYSTEM..."));
    display.println(F("INIT FILESYSTEM..."));
    display.display();

    setupLogging();
    
    if(!bmp.begin()) Serial.println("BMP Error");
    dht.begin();

    // WiFi Configuration (Dual Mode)
    WiFi.mode(WIFI_AP_STA); // AP + Station
    WiFi.softAP(AP_SSID, AP_PASS); // Create Hotspot
    WiFi.begin(WIFI_SSID, WIFI_PASS); // Connect to Router
    
    display.clearDisplay();
    display.setCursor(0,0);
    display.println(F(">> SYSTEM READY <<"));
    display.print(F("AP: ")); display.println(AP_SSID);
    display.print(F("IP: ")); display.println(WiFi.softAPIP());
    display.display();
    delay(2000);

    // Web Routes
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/download", handleDownload);
    server.on("/clear", handleClear);
    server.on("/calibrate", handleCalibrate);
    server.begin();

    missionStartTime = millis();
    
    // Auto-calibrate Altitude
    int32_t p = bmp.readPressure();
    refPressure = p / 100.0;
}

void loop() {
    server.handleClient();
    
    unsigned long currentMillis = millis();

    // Read Sensors & Log
    if (currentMillis - lastLogTime >= LOG_INTERVAL) {
        lastLogTime = currentMillis;
        
        // Read BMP180
        data.temp = bmp.readTemperature();
        data.pressure = bmp.readPressure() / 100.0;
        data.altitude = bmp.readAltitude(refPressure * 100);
        
        // Read DHT
        float h = dht.readHumidity();
        if(!isnan(h)) data.humidity = h;

        // Read Battery & Time
        data.batteryV = readBattery();
        data.missionTime = getMissionTime();

        // 1. Log to Internal Storage
        logDataToStorage();

        // 2. Serial Monitor Log
        Serial.print(data.missionTime); Serial.print(",");
        Serial.print(data.altitude); Serial.println("m");

        // 3. Update OLED
        display.clearDisplay();
        
        // Header
        display.setTextSize(1);
        display.setCursor(0,0);
        display.print(data.missionTime);
        display.setCursor(80,0);
        display.print(data.batteryV, 1); display.print("V");
        display.drawLine(0, 9, 128, 9, WHITE);

        // Main Data
        display.setTextSize(2);
        display.setCursor(0, 15);
        display.print(data.altitude, 1); 
        display.setTextSize(1); display.print("m");

        display.setTextSize(1);
        display.setCursor(70, 15);
        display.print("P:"); display.print(data.pressure, 0);
        
        display.setCursor(0, 40);
        display.print("T:"); display.print(data.temp, 1); display.print("C");
        display.setCursor(70, 40);
        display.print("H:"); display.print(data.humidity, 0); display.print("%");

        // Footer
        display.drawLine(0, 52, 128, 52, WHITE);
        display.setCursor(20, 55);
        display.print("REC: ON | WIFI");

        display.display();
    }
}