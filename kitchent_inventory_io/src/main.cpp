// ============================================================================
//  Kitchen Inventory System - XIAO ESP32C3 + HX711 + Firebase Integration
//  WITH NTP TIME SYNC - 10-digit timestamps (seconds)
//  NTP Servers: pool.ntp.org, time.google.com
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <HX711.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "firebase_credentials.h"

// ===================== HARDWARE PIN DEFINITIONS =====================
#define LOADCELL_DOUT_PIN  D4  // HX711 DT
#define LOADCELL_SCK_PIN   D5  // HX711 SCK
#define PIN_LED            D3  // Active HIGH
#define PIN_BAT_ADC        D0  // Battery voltage divider

// ===================== FACTORY CALIBRATION =====================
const float FACTORY_TARE_OFFSET = 51232.65f;
const float FACTORY_SCALE_FACTOR = 418.25f;

// ===================== SYSTEM CONSTANTS =====================
const float STABILITY_THRESHOLD_GRAMS = 3.0f;
const uint32_t SETTLE_TIME_MS = 3000;
const uint32_t WIFI_ATTEMPT_TIMEOUT_MS = 15000;
const float BATTERY_LOW_THRESHOLD_MV = 3500.0f;
const float BATTERY_CRITICAL_THRESHOLD_MV = 3200.0f;
const uint32_t IDLE_SLEEP_INTERVAL_MS = 1800000;
const uint32_t DEEP_SLEEP_DURATION_US = 30000000;
const uint32_t HEARTBEAT_INTERVAL_MS = 10000;

// ===================== NTP CONFIGURATION =====================
const char* NTP_SERVERS[] = {
    "pool.ntp.org",      // Primary
    "time.google.com"    // Google fallback
};
const int NTP_SERVER_COUNT = 2;
const long NTP_UPDATE_INTERVAL = 3600000; // Update every hour
const long NTP_TIMEOUT_MS = 10000;        // Timeout after 10 seconds
const long GMT_OFFSET_SEC = 0;             // UTC time
const int DAYLIGHT_OFFSET_SEC = 0;

// ===================== ENUMS =====================
enum class MasterState : uint8_t {
    BOOT, CONNECTING, PROVISIONING, STABLE, UNSTABLE,
    ABSENT, RECOVERING, SLEEP, ERROR
};

enum class WiFiState : uint8_t {
    IDLE, ATTEMPTING, CONNECTED, AP_ACTIVE
};

enum class LEDMode : uint8_t {
    OFF, SOLID, BLINK_FAST, BLINK_SLOW, PULSE_SHORT
};

// ===================== GLOBAL OBJECTS =====================
HX711 scale;
Preferences preferences;
DNSServer dnsServer;
WebServer webServer(80);

// ===================== GLOBAL VARIABLES =====================
const char* NVS_NS = "scale";
const char* KEY_TARE = "tare";
const char* KEY_SCALE = "scaleFac";
const char* KEY_SSID = "ssid";
const char* KEY_PASS = "pass";
const char* KEY_DEVICE_ID = "deviceId";
const char* KEY_LAST_WEIGHT = "lastWeight";
const char* KEY_DEVICE_CONFIGURED = "configured";

MasterState currentState = MasterState::BOOT;
WiFiState wifiState = WiFiState::IDLE;
float currentWeight = 0.0f;
float filteredWeight = 0.0f;
float lastStableWeight = 0.0f;
float batteryVoltageMV = 4200.0f;

// Stability Guardian
float weightBuffer[10];
uint8_t weightBufferIdx = 0;
uint8_t weightBufferCount = 0;
uint32_t stabilityCounter = 0;
bool isStableFlag = false;

// Timers
uint32_t lastLoopTime = 0;
uint32_t lastBatteryReadTime = 0;
uint32_t lastHeartbeatTime = 0;
uint32_t lastStableTime = 0;
uint32_t wifiAttemptStartTime = 0;
uint32_t lastNTPUpdateTime = 0;
bool ntpSynced = false;

// WiFi
String apSSID = "KitchenScale-";
IPAddress apIP(192, 168, 4, 1);
bool apModeInitialized = false;
bool wifiCredentialsStored = false;

// Device
String deviceId = "";
bool deviceConfigured = false;
bool cloudPending = false;

// ===================== FUNCTION DECLARATIONS =====================
void initNVM();
void saveToNVM(const char* key, float value);
void saveStringToNVM(const char* key, const String& value);
void saveBoolToNVM(const char* key, bool value);
float loadFloatFromNVM(const char* key, float defaultValue);
String loadStringFromNVM(const char* key, const String& defaultValue);
bool loadBoolFromNVM(const char* key, bool defaultValue);
void updateLED();
void processStateMachine();
void processStabilityGuardian();
void checkBattery();
void handleWiFi();
void startAPMode();
void stopAPMode();
void sendToFirebase(float weight, String state);
String stateToString(MasterState s);
void goToDeepSleep();
void handleRoot();
void handleConnect();
void handleSetup();
float getWeight();
bool syncNTPTime();
uint32_t getCurrentTimestamp();
String formatTimestamp(uint32_t timestamp);

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n╔═══════════════════════════════════════╗");
    Serial.println("║      KITCHEN SCALE v7.0 (NTP)       ║");
    Serial.println("╚═══════════════════════════════════════╝\n");

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BAT_ADC, INPUT);

    initNVM();
    Serial.println("[✓] NVM initialized");

    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(FACTORY_SCALE_FACTOR);
    
    if (!scale.is_ready()) {
        Serial.println("[✗] HX711 not responding!");
        currentState = MasterState::ERROR;
        return;
    }
    Serial.println("[✓] HX711 ready");

    // Load Device ID
    deviceId = loadStringFromNVM(KEY_DEVICE_ID, "");
    deviceConfigured = loadBoolFromNVM(KEY_DEVICE_CONFIGURED, false);
    
    if (deviceConfigured && deviceId.length() > 0) {
        Serial.printf("[✓] Device ID: %s\n", deviceId.c_str());
    } else {
        Serial.println("[!] Device not configured. Will prompt for Device ID.");
    }

    // WiFi
    String ssid = loadStringFromNVM(KEY_SSID, "");
    String pass = loadStringFromNVM(KEY_PASS, "");
    if (ssid.length() > 0 && pass.length() > 0) {
        wifiCredentialsStored = true;
        wifiState = WiFiState::ATTEMPTING;
        wifiAttemptStartTime = millis();
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("[✓] WiFi attempt: %s\n", ssid.c_str());
        currentState = MasterState::CONNECTING;
    } else {
        Serial.println("[!] No WiFi credentials. Entering AP mode.");
        currentState = MasterState::PROVISIONING;
        startAPMode();
    }

    // Get initial weight
    currentWeight = getWeight();
    filteredWeight = currentWeight;
    
    for (int i = 0; i < 10; i++) {
        weightBuffer[weightBufferIdx] = currentWeight;
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;
        delay(20);
    }

    lastLoopTime = millis();
    lastBatteryReadTime = millis();
    lastHeartbeatTime = millis();
    lastStableTime = millis();
    lastNTPUpdateTime = millis();

    Serial.println("[✓] Setup complete. Ready!\n");
}

// ===================== MAIN LOOP =====================
void loop() {
    uint32_t now = millis();

    if (currentState == MasterState::ERROR) {
        digitalWrite(PIN_LED, (now / 200) % 2);
        delay(100);
        return;
    }

    if (currentState == MasterState::SLEEP) {
        return;
    }

    // --- Sync NTP periodically ---
    if (wifiState == WiFiState::CONNECTED && (now - lastNTPUpdateTime >= NTP_UPDATE_INTERVAL || !ntpSynced)) {
        if (syncNTPTime()) {
            lastNTPUpdateTime = now;
            ntpSynced = true;
        }
    }

    // --- Main 100ms loop ---
    if (now - lastLoopTime >= 100) {
        lastLoopTime = now;

        float rawWeight = getWeight();
        filteredWeight = (filteredWeight * 0.7f) + (rawWeight * 0.3f);
        currentWeight = filteredWeight;

        weightBuffer[weightBufferIdx] = currentWeight;
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;

        processStabilityGuardian();
        processStateMachine();

        if (cloudPending && wifiState == WiFiState::CONNECTED && deviceConfigured) {
            sendToFirebase(currentWeight, stateToString(currentState));
        }

        if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
            lastHeartbeatTime = now;
            if (currentState == MasterState::STABLE && deviceConfigured) {
                cloudPending = true;
                Serial.println("[DEBUG] Heartbeat trigger.");
            }
        }

        if (currentState == MasterState::STABLE && (now - lastStableTime) >= IDLE_SLEEP_INTERVAL_MS) {
            if (!cloudPending && deviceConfigured) {
                goToDeepSleep();
            }
        }
    }

    if (now - lastBatteryReadTime >= 60000) {
        lastBatteryReadTime = now;
        checkBattery();
    }

    handleWiFi();
    updateLED();

    if (wifiState == WiFiState::AP_ACTIVE) {
        dnsServer.processNextRequest();
        webServer.handleClient();
    }

    delay(1);
}

// ===================== GET WEIGHT =====================
float getWeight() {
    if (!scale.is_ready()) {
        return currentWeight;
    }
    
    long raw = scale.read();
    float weight = ((float)raw + FACTORY_TARE_OFFSET) / FACTORY_SCALE_FACTOR;
    
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        lastDebug = millis();
        Serial.printf("[DEBUG] Raw: %ld, Weight: %.2f g\n", raw, weight);
    }
    
    return weight;
}

// ===================== NTP TIME SYNC =====================
bool syncNTPTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NTP] WiFi not connected, skipping sync.");
        return false;
    }

    Serial.println("[NTP] Syncing time...");
    
    // Try each NTP server
    for (int i = 0; i < NTP_SERVER_COUNT; i++) {
        Serial.printf("[NTP] Trying %s...\n", NTP_SERVERS[i]);
        
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVERS[i]);
        
        uint32_t startTime = millis();
        while (millis() - startTime < NTP_TIMEOUT_MS) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                // Success! Time is synced
                char buffer[32];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
                Serial.printf("[NTP] ✓ Time synced: %s (Server: %s)\n", buffer, NTP_SERVERS[i]);
                return true;
            }
            delay(100);
        }
        Serial.printf("[NTP] ✗ %s timed out\n", NTP_SERVERS[i]);
    }
    
    Serial.println("[NTP] All servers failed! Using fallback time.");
    return false;
}

// ===================== GET CURRENT TIMESTAMP (10-digit seconds) =====================
uint32_t getCurrentTimestamp() {
    struct tm timeinfo;
    uint32_t timestamp = 0;
    
    if (getLocalTime(&timeinfo)) {
        // Convert to Unix timestamp (10 digits, seconds)
        timestamp = (uint32_t)mktime(&timeinfo);
        
        // Debug: Print timestamp every 60 seconds
        static uint32_t lastDebug = 0;
        if (millis() - lastDebug > 60000) {
            lastDebug = millis();
            char buffer[32];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            Serial.printf("[TIME] %s (Timestamp: %u s)\n", buffer, timestamp);
        }
    } else {
        // Fallback: use device uptime if NTP not synced
        static uint32_t bootTime = 0;
        if (bootTime == 0) {
            bootTime = millis() / 1000;
        }
        // Use Jan 1, 2025 as reference for fallback
        uint32_t fallbackTime = 1735689600 + (millis() / 1000);
        timestamp = fallbackTime;
        Serial.printf("[TIME] Fallback timestamp: %u s\n", timestamp);
    }
    
    return timestamp;
}

// ===================== STABILITY GUARDIAN =====================
void processStabilityGuardian() {
    if (weightBufferCount < 10) return;

    float minVal = 9999.0f, maxVal = -9999.0f;
    for (int i = 0; i < 10; i++) {
        if (weightBuffer[i] < minVal) minVal = weightBuffer[i];
        if (weightBuffer[i] > maxVal) maxVal = weightBuffer[i];
    }
    float range = maxVal - minVal;

    if (range < STABILITY_THRESHOLD_GRAMS) {
        stabilityCounter++;
    } else {
        stabilityCounter = 0;
    }

    isStableFlag = (stabilityCounter >= (SETTLE_TIME_MS / 100));
}

// ===================== STATE MACHINE =====================
void processStateMachine() {
    MasterState newState = currentState;

    if (currentWeight < -10.0f) {
        if (currentState == MasterState::STABLE || 
            currentState == MasterState::UNSTABLE || 
            currentState == MasterState::RECOVERING) {
            
            if (currentState == MasterState::STABLE) {
                saveToNVM(KEY_LAST_WEIGHT, currentWeight);
                Serial.printf("[STATE] Jar lifted. Last stable: %.2f g\n", currentWeight);
            }
            newState = MasterState::ABSENT;
        } else {
            newState = MasterState::ABSENT;
        }
    } else {
        if (currentState == MasterState::ABSENT) {
            newState = MasterState::RECOVERING;
            Serial.println("[STATE] Jar placed back. Waiting to stabilize...");
        } else if (currentState == MasterState::RECOVERING) {
            if (isStableFlag) {
                saveToNVM(KEY_LAST_WEIGHT, currentWeight);
                lastStableTime = millis();
                Serial.printf("[STATE] Stable: %.2f g\n", currentWeight);
                newState = MasterState::STABLE;
                cloudPending = true;
            } else {
                newState = MasterState::RECOVERING;
            }
        } else if (currentState == MasterState::STABLE) {
            if (!isStableFlag) {
                newState = MasterState::UNSTABLE;
            } else {
                newState = MasterState::STABLE;
            }
        } else if (currentState == MasterState::UNSTABLE) {
            if (isStableFlag) {
                newState = MasterState::STABLE;
                saveToNVM(KEY_LAST_WEIGHT, currentWeight);
                lastStableTime = millis();
                Serial.printf("[STATE] Stable: %.2f g\n", currentWeight);
                cloudPending = true;
            } else {
                newState = MasterState::UNSTABLE;
            }
        } else {
            if (isStableFlag) {
                newState = MasterState::STABLE;
                saveToNVM(KEY_LAST_WEIGHT, currentWeight);
                lastStableTime = millis();
                Serial.printf("[STATE] Initial stable: %.2f g\n", currentWeight);
            } else {
                newState = MasterState::UNSTABLE;
            }
        }
    }

    if (currentState != newState) {
        Serial.printf("[STATE] %s -> %s\n", 
            stateToString(currentState).c_str(), 
            stateToString(newState).c_str());
        currentState = newState;
    }
}

// ===================== BATTERY =====================
void checkBattery() {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += analogRead(PIN_BAT_ADC);
        delay(2);
    }
    float avgAdc = sum / 10.0f;
    float dividerRatio = 2.0f;
    batteryVoltageMV = (avgAdc / 4095.0f) * 3300.0f * dividerRatio;
    Serial.printf("[BAT] %.2f mV\n", batteryVoltageMV);

    if (batteryVoltageMV < BATTERY_CRITICAL_THRESHOLD_MV) {
        Serial.println("[BAT] CRITICAL! Entering deep sleep.");
        saveToNVM(KEY_LAST_WEIGHT, currentWeight);
        esp_sleep_enable_timer_wakeup(3600000000ULL);
        esp_deep_sleep_start();
    }
}

// ===================== WiFi HANDLING =====================
void handleWiFi() {
    uint32_t now = millis();

    if (wifiState == WiFiState::ATTEMPTING) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiState = WiFiState::CONNECTED;
            Serial.printf("[WIFI] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
            if (currentState == MasterState::CONNECTING) {
                currentState = MasterState::UNSTABLE;
            }
            if (apModeInitialized) stopAPMode();
            return;
        }
        if (now - wifiAttemptStartTime >= WIFI_ATTEMPT_TIMEOUT_MS) {
            Serial.println("[WIFI] Timeout. Entering AP mode.");
            wifiState = WiFiState::IDLE;
            if (currentState == MasterState::CONNECTING) {
                currentState = MasterState::PROVISIONING;
            }
            startAPMode();
        }
    }

    if (wifiState == WiFiState::CONNECTED && WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Lost connection. Reconnecting...");
        wifiState = WiFiState::IDLE;
        String ssid = loadStringFromNVM(KEY_SSID, "");
        String pass = loadStringFromNVM(KEY_PASS, "");
        if (ssid.length() > 0) {
            wifiState = WiFiState::ATTEMPTING;
            wifiAttemptStartTime = millis();
            WiFi.begin(ssid.c_str(), pass.c_str());
        }
    }

    if (wifiState == WiFiState::IDLE && wifiCredentialsStored) {
        wifiState = WiFiState::ATTEMPTING;
        wifiAttemptStartTime = millis();
        String ssid = loadStringFromNVM(KEY_SSID, "");
        String pass = loadStringFromNVM(KEY_PASS, "");
        WiFi.begin(ssid.c_str(), pass.c_str());
        if (currentState != MasterState::PROVISIONING && currentState != MasterState::CONNECTING) {
            currentState = MasterState::CONNECTING;
        }
    }
}

// ===================== AP MODE & PROVISIONING =====================
void startAPMode() {
    if (apModeInitialized) return;
    Serial.println("[AP] Starting SoftAP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSSID.c_str());
    dnsServer.start(53, "*", apIP);
    
    webServer.on("/", handleRoot);
    webServer.on("/connect", HTTP_POST, handleConnect);
    webServer.on("/setup", HTTP_GET, handleSetup);
    webServer.on("/setup", HTTP_POST, handleSetup);
    
    webServer.begin();
    apModeInitialized = true;
    wifiState = WiFiState::AP_ACTIVE;
    currentState = MasterState::PROVISIONING;
    Serial.printf("[AP] SSID: %s, IP: %s\n", apSSID.c_str(), apIP.toString().c_str());
}

void stopAPMode() {
    if (!apModeInitialized) return;
    dnsServer.stop();
    webServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apModeInitialized = false;
    wifiState = WiFiState::IDLE;
    Serial.println("[AP] Stopped.");
}

// ===================== WEB PAGES =====================
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><title>Scale Setup</title>"
                  "<meta charset='UTF-8'></head><body>"
                  "<h2>Kitchen Scale Setup</h2>";
    
    if (!deviceConfigured) {
        html += "<p style='color:red;'><b>⚠️ Device not configured!</b></p>";
        html += "<p>Please set your Device ID first:</p>";
        html += "<form method='POST' action='/setup'>";
        html += "Device ID: <input type='text' name='deviceId' required><br><br>";
        html += "<input type='submit' value='Save Device ID'>";
        html += "</form>";
        html += "<hr>";
    } else {
        html += "<p><b>✅ Device ID: " + deviceId + "</b></p>";
    }
    
    html += "<h3>Wi-Fi Settings</h3>";
    html += "<form method='POST' action='/connect'>";
    html += "SSID: <input type='text' name='ssid' required><br><br>";
    html += "Password: <input type='password' name='pass' required><br><br>";
    html += "<input type='submit' value='Connect WiFi'>";
    html += "</form>";
    
    if (deviceConfigured) {
        html += "<hr><p><b>Current Status:</b><br>";
        html += "Weight: " + String(currentWeight, 2) + " g<br>";
        html += "State: " + stateToString(currentState) + "<br>";
        html += "Battery: " + String(batteryVoltageMV, 0) + " mV<br>";
        html += "NTP Synced: " + String(ntpSynced ? "Yes" : "No") + "</p>";
    }
    
    html += "</body></html>";
    webServer.send(200, "text/html", html);
}

void handleSetup() {
    if (webServer.method() == HTTP_POST) {
        String newDeviceId = webServer.arg("deviceId");
        newDeviceId.trim();
        
        if (newDeviceId.length() > 0) {
            deviceId = newDeviceId;
            deviceConfigured = true;
            saveStringToNVM(KEY_DEVICE_ID, deviceId);
            saveBoolToNVM(KEY_DEVICE_CONFIGURED, true);
            Serial.printf("[SETUP] Device ID saved: %s\n", deviceId.c_str());
            
            webServer.send(200, "text/html", "<html><body><h2>✅ Device ID Saved!</h2>"
                          "<p>Device ID: <b>" + deviceId + "</b></p>"
                          "<a href='/'>Back to Setup</a></body></html>");
        } else {
            webServer.send(400, "text/html", "<html><body><h2>❌ Invalid Device ID</h2>"
                          "<a href='/'>Try again</a></body></html>");
        }
    } else {
        String html = "<!DOCTYPE html><html><head><title>Set Device ID</title>"
                      "<meta charset='UTF-8'></head><body>"
                      "<h2>Set Device ID</h2>"
                      "<form method='POST' action='/setup'>"
                      "Device ID: <input type='text' name='deviceId' required><br><br>"
                      "<input type='submit' value='Save'>"
                      "</form>"
                      "<a href='/'>Cancel</a>"
                      "</body></html>";
        webServer.send(200, "text/html", html);
    }
}

void handleConnect() {
    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");
    
    if (ssid.length() > 0 && pass.length() > 0) {
        saveStringToNVM(KEY_SSID, ssid);
        saveStringToNVM(KEY_PASS, pass);
        wifiCredentialsStored = true;
        
        webServer.send(200, "text/html", "<html><body><h2>✅ WiFi Saved!</h2>"
                      "<p>SSID: " + ssid + "</p>"
                      "<p>Attempting to connect...</p>"
                      "<a href='/'>Back</a></body></html>");
        
        stopAPMode();
        wifiState = WiFiState::ATTEMPTING;
        wifiAttemptStartTime = millis();
        WiFi.begin(ssid.c_str(), pass.c_str());
        currentState = MasterState::CONNECTING;
        Serial.printf("[AP] WiFi saved: %s\n", ssid.c_str());
    } else {
        webServer.send(400, "text/html", "<html><body><h2>❌ Invalid credentials</h2>"
                      "<a href='/'>Try again</a></body></html>");
    }
}

// ===================== SEND TO FIREBASE (WITH NTP TIMESTAMPS) =====================
void sendToFirebase(float weight, String state) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[FIREBASE] No WiFi.");
        return;
    }
    
    if (!deviceConfigured || deviceId.length() == 0) {
        Serial.println("[FIREBASE] Device not configured. Skipping.");
        return;
    }

    // Send 0.0 when weight is negative (jar absent)
    float sendWeight = (weight < 0.0f) ? 0.0f : weight;

    // Get timestamp from NTP (10 digits, seconds)
    uint32_t currentTime = getCurrentTimestamp();

    String url = String(FIREBASE_HOST) + "telemetry/" + deviceId + ".json";
    
    StaticJsonDocument<384> doc;
    
    // Update main fields
    doc["deviceId"] = deviceId;
    doc["currentWeight"] = sendWeight;
    doc["batteryVoltage"] = batteryVoltageMV;
    doc["deviceState"] = state;
    
    // Use NTP timestamp for lastUpdated (10 digits, seconds)
    doc["lastUpdated"] = currentTime;
    
    // For history, use the SAME timestamp as the key
    if (state == "STABLE" && weight >= 0.0f) {
        String historyKey = "history/" + String(currentTime);
        doc[historyKey] = sendWeight;
        
        Serial.printf("[FIREBASE] History at %u s: %.2f g\n", currentTime, sendWeight);
    }

    String jsonString;
    serializeJson(doc, jsonString);
    
    Serial.printf("[FIREBASE] Payload: %s\n", jsonString.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");
    
    // Use PATCH to update only specified fields
    int httpCode = https.PATCH(jsonString);

    if (httpCode == 200) {
        Serial.printf("[FIREBASE] ✓ Update: %.2f g, State: %s\n", sendWeight, state.c_str());
        cloudPending = false;
    } else if (httpCode > 0) {
        String response = https.getString();
        Serial.printf("[FIREBASE] Error %d: %s\n", httpCode, response.c_str());
    } else {
        Serial.printf("[FIREBASE] HTTP error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
}

// ===================== LED INDICATOR =====================
void updateLED() {
    uint32_t now = millis();
    bool lowBattery = (batteryVoltageMV < BATTERY_LOW_THRESHOLD_MV);

    LEDMode mode = LEDMode::SOLID;
    
    switch (currentState) {
        case MasterState::BOOT:
        case MasterState::CONNECTING:
            mode = LEDMode::BLINK_FAST;
            break;
        case MasterState::PROVISIONING:
            mode = LEDMode::BLINK_SLOW;
            break;
        case MasterState::STABLE:
            mode = LEDMode::SOLID;
            break;
        case MasterState::UNSTABLE:
            mode = LEDMode::BLINK_FAST;
            break;
        case MasterState::ABSENT:
            mode = LEDMode::SOLID;
            break;
        case MasterState::RECOVERING:
            mode = LEDMode::BLINK_FAST;
            break;
        case MasterState::SLEEP:
            mode = LEDMode::OFF;
            break;
        case MasterState::ERROR:
            mode = LEDMode::BLINK_FAST;
            break;
        default:
            mode = LEDMode::OFF;
    }

    if (lowBattery) {
        mode = LEDMode::PULSE_SHORT;
    }

    bool output = false;
    
    switch (mode) {
        case LEDMode::OFF:
            output = false;
            break;
        case LEDMode::SOLID:
            output = true;
            break;
        case LEDMode::BLINK_FAST:
            output = ((now / 200) % 2) == 0;
            break;
        case LEDMode::BLINK_SLOW:
            output = ((now / 500) % 2) == 0;
            break;
        case LEDMode::PULSE_SHORT:
            output = ((now % 1000) < 100);
            break;
    }

    digitalWrite(PIN_LED, output ? HIGH : LOW);
}

// ===================== STATE TO STRING =====================
String stateToString(MasterState s) {
    switch (s) {
        case MasterState::BOOT: return "BOOT";
        case MasterState::CONNECTING: return "CONNECTING";
        case MasterState::PROVISIONING: return "PROVISIONING";
        case MasterState::STABLE: return "STABLE";
        case MasterState::UNSTABLE: return "UNSTABLE";
        case MasterState::ABSENT: return "ABSENT";
        case MasterState::RECOVERING: return "RECOVERING";
        case MasterState::SLEEP: return "SLEEP";
        case MasterState::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// ===================== DEEP SLEEP =====================
void goToDeepSleep() {
    Serial.println("[SLEEP] Entering deep sleep for 30s.");
    saveToNVM(KEY_LAST_WEIGHT, currentWeight);
    currentState = MasterState::SLEEP;
    digitalWrite(PIN_LED, LOW);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_US);
    esp_deep_sleep_start();
}

// ===================== NVM HELPERS =====================
void initNVM() {
    preferences.begin(NVS_NS, false);
    if (!preferences.isKey(KEY_TARE)) preferences.putFloat(KEY_TARE, FACTORY_TARE_OFFSET);
    if (!preferences.isKey(KEY_SCALE)) preferences.putFloat(KEY_SCALE, FACTORY_SCALE_FACTOR);
    preferences.end();
}

void saveToNVM(const char* key, float value) {
    preferences.begin(NVS_NS, false);
    preferences.putFloat(key, value);
    preferences.end();
}

void saveStringToNVM(const char* key, const String& value) {
    preferences.begin(NVS_NS, false);
    preferences.putString(key, value);
    preferences.end();
}

void saveBoolToNVM(const char* key, bool value) {
    preferences.begin(NVS_NS, false);
    preferences.putBool(key, value);
    preferences.end();
}

float loadFloatFromNVM(const char* key, float defaultValue) {
    preferences.begin(NVS_NS, true);
    float val = preferences.getFloat(key, defaultValue);
    preferences.end();
    return val;
}

String loadStringFromNVM(const char* key, const String& defaultValue) {
    preferences.begin(NVS_NS, true);
    String val = preferences.getString(key, defaultValue);
    preferences.end();
    return val;
}

bool loadBoolFromNVM(const char* key, bool defaultValue) {
    preferences.begin(NVS_NS, true);
    bool val = preferences.getBool(key, defaultValue);
    preferences.end();
    return val;
}