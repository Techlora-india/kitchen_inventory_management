// ============================================================================
//  Kitchen Inventory Scale – WiFi ON DEMAND ONLY
//  - WiFi only connects when there is data to send
//  - No WiFi on normal wake-ups
//  - Fast reconnect with cached BSSID/channel
//  - Deep sleep with 1-min idle wake
//  - Adaptive polling, LED indicators
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
#include <esp_sleep.h>
#include "firebase_credentials.h"

// ===================== HARDWARE PIN DEFINITIONS =====================
#define LOADCELL_DOUT_PIN  D4
#define LOADCELL_SCK_PIN   D5
#define PIN_HX711_POWER    D6
#define PIN_LED            D3
#define PIN_BAT_ADC        D0

// ===================== FACTORY CALIBRATION =====================
const float FACTORY_TARE_OFFSET = 51232.65f;
const float FACTORY_SCALE_FACTOR = 418.25f;

// ===================== SYSTEM CONSTANTS =====================
const float STABILITY_THRESHOLD_GRAMS = 3.0f;
const uint32_t SETTLE_TIME_MS = 1000;
const float CHANGE_THRESHOLD_GRAMS = 3.0f;
const uint32_t NORMAL_SLEEP_SEC = 60;
const uint32_t UNSTABLE_REPOLL_MS = 5000;
const uint32_t FAST_POLL_INTERVAL_MS = 100;
const float BATTERY_LOW_THRESHOLD_MV = 3500.0f;
const float BATTERY_CRITICAL_THRESHOLD_MV = 3200.0f;

// WiFi retry backoff
const uint32_t INITIAL_RETRY_INTERVAL_MS = 60000;
const uint32_t MAX_RETRY_INTERVAL_MS = 3600000;

// ===================== NTP =====================
const char* NTP_SERVERS[] = {"pool.ntp.org", "time.google.com"};
const int NTP_SERVER_COUNT = 2;
const long NTP_TIMEOUT_MS = 10000;
const long GMT_OFFSET_SEC = 0;
const int DAYLIGHT_OFFSET_SEC = 0;

// ===================== RTC MEMORY (persists across deep sleep) =====================
RTC_NOINIT_ATTR uint8_t saved_bssid[6];
RTC_NOINIT_ATTR uint8_t saved_channel;
RTC_NOINIT_ATTR uint32_t magic_check;
RTC_NOINIT_ATTR uint32_t boot_count;
RTC_NOINIT_ATTR uint8_t rtc_ntp_synced;
RTC_NOINIT_ATTR uint32_t rtc_ntp_sync_time;

#define MEMORY_MAGIC_NUMBER 0xDEADBEEF

// WiFi event flag
volatile bool wifi_connected = false;
bool wifi_powered_off = true;  // start with WiFi off

// ===================== ENUMS =====================
enum class MasterState : uint8_t {
    BOOT, CONNECTING, PROVISIONING, STABLE, UNSTABLE,
    ABSENT, RECOVERING, SLEEP, ERROR
};

enum class WiFiState : uint8_t {
    IDLE, ATTEMPTING, CONNECTED, AP_ACTIVE
};

enum class LEDMode : uint8_t {
    OFF, SOLID, BLINK_FAST, BLINK_SLOW, PULSE_SHORT, PULSE_LOW_BATTERY
};

// ===================== GLOBAL OBJECTS =====================
HX711 scale;
Preferences preferences;
DNSServer dnsServer;
WebServer webServer(80);

// ===================== NVM KEYS =====================
const char* NVS_NS = "scale";
const char* KEY_TARE = "tare";
const char* KEY_SCALE = "scaleFac";
const char* KEY_SSID = "ssid";
const char* KEY_PASS = "pass";
const char* KEY_DEVICE_ID = "deviceId";
const char* KEY_LAST_WEIGHT = "lastWeight";
const char* KEY_INVENTORY = "inventory";
const char* KEY_DEVICE_CONFIGURED = "configured";

// ===================== GLOBAL VARIABLES =====================
MasterState currentState = MasterState::BOOT;
WiFiState wifiState = WiFiState::IDLE;
float currentWeight = 0.0f;
float filteredWeight = 0.0f;
float lastStableWeight = 0.0f;
float totalInventory = 0.0f;
float batteryVoltageMV = 4200.0f;

// Stability buffer
float weightBuffer[10];
uint8_t weightBufferIdx = 0;
uint8_t weightBufferCount = 0;
uint32_t stabilityCounter = 0;
bool isStableFlag = false;
bool firstBoot = true;
bool setupComplete = false;

// Timers
uint32_t lastLoopTime = 0;
uint32_t lastBatteryReadTime = 0;
uint32_t lastStableTime = 0;
uint32_t wifiAttemptStartTime = 0;

// WiFi & cloud
String apSSID = "KitchenScale-";
IPAddress apIP(192, 168, 4, 1);
bool apModeInitialized = false;
bool wifiCredentialsStored = false;
String deviceId = "";
bool deviceConfigured = false;

// Cloud pending & retry
bool cloudPending = false;
uint32_t lastRetryTime = 0;
uint32_t retryInterval = INITIAL_RETRY_INTERVAL_MS;

// ===================== FUNCTION PROTOTYPES =====================
void initNVM();
void saveToNVM(const char* key, float value);
void saveStringToNVM(const char* key, const String& value);
void saveBoolToNVM(const char* key, bool value);
float loadFloatFromNVM(const char* key, float defaultValue);
String loadStringFromNVM(const char* key, const String& defaultValue);
bool loadBoolFromNVM(const char* key, bool defaultValue);
void updateLED();
void checkStability();
void checkBattery();
void handleWiFi();
void startAPMode();
void stopAPMode();
void sendToFirebase(float weight, String state);
String stateToString(MasterState s);
void goToDeepSleep(uint32_t seconds);
void handleRoot();
void handleConnect();
void handleSetup();
float readWeight();
float getAverageWeight(int samples);
bool isWeightStable();
void powerHX711(bool on);
void processStableWeight(float weight);
void processUnstableWeight(float weight);
bool syncNTPTime();
uint32_t getCurrentTimestamp();
void attemptSend();
void onWifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
void powerDownWiFi();
bool connectWiFiIfNeeded();

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n╔═══════════════════════════════════════╗");
    Serial.println("║  KITCHEN SCALE v10.0 (WiFi on Demand)║");
    Serial.println("╚═══════════════════════════════════════╝\n");

    // Check cold boot vs deep sleep wake
    if (magic_check != MEMORY_MAGIC_NUMBER) {
        magic_check = MEMORY_MAGIC_NUMBER;
        boot_count = 1;
        saved_channel = 0;
        rtc_ntp_synced = 0;
        rtc_ntp_sync_time = 0;
        Serial.println("--- Cold Boot / Power Loss Detected ---");
    } else {
        boot_count++;
        Serial.printf("--- Wakeup Cycle #%u ---\n", boot_count);
    }

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BAT_ADC, INPUT);
    pinMode(PIN_HX711_POWER, OUTPUT);
    powerHX711(false);

    initNVM();
    Serial.println("[✓] NVM initialized");

    totalInventory = loadFloatFromNVM(KEY_INVENTORY, 0.0f);
    lastStableWeight = loadFloatFromNVM(KEY_LAST_WEIGHT, 0.0f);
    deviceId = loadStringFromNVM(KEY_DEVICE_ID, "");
    deviceConfigured = loadBoolFromNVM(KEY_DEVICE_CONFIGURED, false);
    Serial.printf("[✓] Last stable: %.2f g, Inventory: %.2f g\n", lastStableWeight, totalInventory);
    if (deviceConfigured && deviceId.length() > 0)
        Serial.printf("[✓] Device ID: %s\n", deviceId.c_str());
    else
        Serial.println("[!] Device not configured.");

    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(FACTORY_SCALE_FACTOR);

    // ============================================================
    // WIFI CONNECTION IS DEFERRED – only when needed (in attemptSend)
    // ============================================================
    // We only set up WiFi event callback, but do NOT connect here.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(onWifiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    // WiFi is off initially
    powerDownWiFi();

    // If device is not configured, enter AP mode and stay awake for provisioning
    if (!deviceConfigured || deviceId.length() == 0) {
        Serial.println("[!] Device not configured. Entering AP mode.");
        currentState = MasterState::PROVISIONING;
        startAPMode();
        setupComplete = false;
    } else {
        setupComplete = true;
        // WiFi credentials might be missing? Check NVM
        String ssid = loadStringFromNVM(KEY_SSID, "");
        String pass = loadStringFromNVM(KEY_PASS, "");
        if (ssid.length() > 0 && pass.length() > 0) {
            wifiCredentialsStored = true;
            // saved_channel = WiFi.channel();; // from RTC
        } else {
            wifiCredentialsStored = false;
        }
    }

    // Initial reading
    powerHX711(true);
    currentWeight = readWeight();
    powerHX711(false);
    filteredWeight = currentWeight;
    if (lastStableWeight == 0.0f) lastStableWeight = currentWeight;

    for (int i = 0; i < 10; i++) {
        powerHX711(true);
        weightBuffer[weightBufferIdx] = readWeight();
        powerHX711(false);
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;
        delay(20);
    }

    lastLoopTime = millis();
    lastBatteryReadTime = millis();
    lastStableTime = millis();

    if (firstBoot) {
        firstBoot = false;
        if (isWeightStable()) processStableWeight(currentWeight);
    }

    // Provisioning loop (stays awake until configured)
    if (!setupComplete) {
        Serial.println("[SETUP] Running provisioning loop...");
        while (!setupComplete) {
            handleWiFi();
            updateLED();
            if (wifiState == WiFiState::AP_ACTIVE) {
                dnsServer.processNextRequest();
                webServer.handleClient();
            }
            delay(10);
        }
        // After setup, go to sleep
        goToDeepSleep(NORMAL_SLEEP_SEC);
    }

    Serial.println("[✓] Setup complete. WiFi is OFF until needed.");
}

// ===================== MAIN LOOP =====================
void loop() {
    uint32_t now = millis();

    if (currentState == MasterState::ERROR) {
        digitalWrite(PIN_LED, (now / 200) % 2);
        delay(100);
        return;
    }

    if (currentState == MasterState::SLEEP) return;

    if (!setupComplete) {
        handleWiFi();
        updateLED();
        if (wifiState == WiFiState::AP_ACTIVE) {
            dnsServer.processNextRequest();
            webServer.handleClient();
        }
        delay(10);
        return;
    }

    // Main 100ms loop
    if (now - lastLoopTime >= 100) {
        lastLoopTime = now;

        powerHX711(true);
        float raw = readWeight();
        powerHX711(false);
        filteredWeight = (filteredWeight * 0.7f) + (raw * 0.3f);
        currentWeight = filteredWeight;

        weightBuffer[weightBufferIdx] = currentWeight;
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;

        checkStability();

        if (isStableFlag) {
            processStableWeight(currentWeight);
            if (currentState != MasterState::PROVISIONING && currentState != MasterState::CONNECTING)
                goToDeepSleep(NORMAL_SLEEP_SEC);
        } else {
            processUnstableWeight(currentWeight);
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

// ===================== WIFI EVENT CALLBACK =====================
void onWifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
    wifi_connected = true;
}

// ===================== SAFE WIFI POWER DOWN =====================
void powerDownWiFi() {
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifi_powered_off = true;
        wifi_connected = false;
        wifiState = WiFiState::IDLE;
        Serial.println("[WIFI] Powered off.");
    }
}

// ===================== CONNECT WIFI IF NEEDED (for send) =====================
bool connectWiFiIfNeeded() {
    if (WiFi.status() == WL_CONNECTED) {
        wifi_powered_off = false;
        return true;
    }
    if (!wifiCredentialsStored) {
        Serial.println("[WIFI] No credentials.");
        return false;
    }

    Serial.println("[WIFI] Connecting on demand...");
    String ssid = loadStringFromNVM(KEY_SSID, "");
    String pass = loadStringFromNVM(KEY_PASS, "");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(onWifiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    wifi_connected = false;
    wifi_powered_off = false;

    if (saved_channel > 0 && saved_channel <= 14) {
        WiFi.begin(ssid.c_str(), pass.c_str(), saved_channel, saved_bssid);
    } else {
        WiFi.begin(ssid.c_str(), pass.c_str());
    }

    uint32_t start = millis();
    while (!wifi_connected && (millis() - start) < 5000) {
        delay(10);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] Connected in %lu ms\n", millis() - start);
        Serial.print("[WIFI] IP: ");
        Serial.println(WiFi.localIP());
        memcpy(saved_bssid, WiFi.BSSID(), 6);
        saved_channel = WiFi.channel();
        wifi_powered_off = false;
        return true;
    } else {
        Serial.println("[WIFI] Connection failed.");
        powerDownWiFi();
        return false;
    }
}

// ===================== HX711 POWER =====================
void powerHX711(bool on) {
    digitalWrite(PIN_HX711_POWER, on ? HIGH : LOW);
    if (on) delay(50);
}

// ===================== WEIGHT READING =====================
float readWeight() {
    if (!scale.is_ready()) return currentWeight;
    long raw = scale.read();
    float w = ((float)raw + FACTORY_TARE_OFFSET) / FACTORY_SCALE_FACTOR;
    return w;
}

float getAverageWeight(int samples) {
    if (samples < 1) return 0.0f;
    float sum = 0.0f;
    int valid = 0;
    for (int i = 0; i < samples; i++) {
        float w = readWeight();
        if (w > -1000 && w < 10000) { sum += w; valid++; }
        delay(20);
    }
    return (valid == 0) ? 0.0f : sum / valid;
}

// ===================== STABILITY =====================
bool isWeightStable() {
    if (weightBufferCount < 10) return false;
    float minVal = 9999.0f, maxVal = -9999.0f;
    for (int i = 0; i < 10; i++) {
        if (weightBuffer[i] < minVal) minVal = weightBuffer[i];
        if (weightBuffer[i] > maxVal) maxVal = weightBuffer[i];
    }
    return (maxVal - minVal) < STABILITY_THRESHOLD_GRAMS;
}

void checkStability() {
    if (isWeightStable()) stabilityCounter++;
    else stabilityCounter = 0;
    isStableFlag = (stabilityCounter >= (SETTLE_TIME_MS / 100));
}

// ===================== PROCESS STABLE =====================
void processStableWeight(float weight) {
    float delta = weight - lastStableWeight;

    if (fabs(delta) < CHANGE_THRESHOLD_GRAMS) {
        lastStableWeight = weight;
        saveToNVM(KEY_LAST_WEIGHT, lastStableWeight);
        if (cloudPending) attemptSend();
        return;
    }

    Serial.printf("[PROCESS] Delta: %.2f g\n", delta);
    totalInventory += delta;
    if (totalInventory < 0) totalInventory = 0;
    lastStableWeight = weight;

    saveToNVM(KEY_INVENTORY, totalInventory);
    saveToNVM(KEY_LAST_WEIGHT, lastStableWeight);

    attemptSend(); // this will connect WiFi and send
}

// ===================== PROCESS UNSTABLE =====================
void processUnstableWeight(float unstableStartWeight) {
    Serial.println("[UNSTABLE] Fast polling...");
    float firstWeight = unstableStartWeight;
    uint32_t startTime = millis();
    bool stableFound = false;
    float finalStableWeight = firstWeight;

    while (!stableFound) {
        powerHX711(true);
        float w = readWeight();
        powerHX711(false);
        filteredWeight = (filteredWeight * 0.7f) + (w * 0.3f);
        currentWeight = filteredWeight;
        weightBuffer[weightBufferIdx] = currentWeight;
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;
        checkStability();

        if (isStableFlag) {
            powerHX711(true);
            finalStableWeight = getAverageWeight(5);
            powerHX711(false);
            stableFound = true;
            Serial.printf("[UNSTABLE] Stable at %.2f g\n", finalStableWeight);
            break;
        }

        if (millis() - startTime > UNSTABLE_REPOLL_MS) {
            Serial.printf("[UNSTABLE] Repoll after %u ms\n", UNSTABLE_REPOLL_MS);
            powerHX711(false);
            esp_sleep_enable_timer_wakeup(UNSTABLE_REPOLL_MS * 1000ULL);
            esp_deep_sleep_start();
            return;
        }
        delay(FAST_POLL_INTERVAL_MS);
    }

    float delta = finalStableWeight - firstWeight;
    Serial.printf("[UNSTABLE] Delta: %.2f g\n", delta);

    if (fabs(delta) >= CHANGE_THRESHOLD_GRAMS) {
        totalInventory += delta;
        if (totalInventory < 0) totalInventory = 0;
        lastStableWeight = finalStableWeight;
        saveToNVM(KEY_INVENTORY, totalInventory);
        saveToNVM(KEY_LAST_WEIGHT, lastStableWeight);
        attemptSend();
    } else {
        lastStableWeight = finalStableWeight;
        saveToNVM(KEY_LAST_WEIGHT, lastStableWeight);
        if (cloudPending) attemptSend();
    }

    goToDeepSleep(NORMAL_SLEEP_SEC);
}

// ===================== SEND ATTEMPT (with backoff and WiFi on demand) =====================
void attemptSend() {
    if (!deviceConfigured || deviceId.length() == 0) {
        cloudPending = false;
        return;
    }

    if (cloudPending) {
        uint32_t now = millis();
        if (now - lastRetryTime < retryInterval) {
            return;
        }
    }

    // Connect WiFi if needed (this will power on WiFi and connect)
    if (!connectWiFiIfNeeded()) {
        // WiFi failed, mark pending and backoff
        cloudPending = true;
        lastRetryTime = millis();
        retryInterval = min(retryInterval * 2, MAX_RETRY_INTERVAL_MS);
        powerDownWiFi();
        return;
    }

    // NTP sync only once per cold boot (when we have WiFi)
    if (rtc_ntp_synced == 0) {
        Serial.println("[NTP] Syncing time (first time)...");
        if (syncNTPTime()) {
            rtc_ntp_synced = 1;
            rtc_ntp_sync_time = getCurrentTimestamp();
            Serial.println("[NTP] Sync successful.");
        } else {
            Serial.println("[NTP] Sync failed, will retry later.");
            // Keep rtc_ntp_synced = 0, will retry on next send
        }
    }

    // Build and send data
    float weightToSend = lastStableWeight;
    float inventoryToSend = totalInventory;
    uint32_t ts = getCurrentTimestamp();

    String url = String(FIREBASE_HOST) + "telemetry/" + deviceId + ".json";
    StaticJsonDocument<384> doc;
    doc["deviceId"] = deviceId;
    doc["currentWeight"] = weightToSend < 0 ? 0 : weightToSend;
    doc["batteryVoltage"] = batteryVoltageMV;
    doc["deviceState"] = stateToString(currentState);
    doc["totalInventory"] = inventoryToSend;
    doc["lastUpdated"] = ts;
    String historyKey = "history/" + String(ts);
    doc[historyKey] = weightToSend < 0 ? 0 : weightToSend;

    String jsonString;
    serializeJson(doc, jsonString);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.PATCH(jsonString);

    if (httpCode == 200) {
        Serial.printf("[SEND] ✓ Sent: %.2f g\n", weightToSend);
        cloudPending = false;
        retryInterval = INITIAL_RETRY_INTERVAL_MS;
    } else {
        String response = https.getString();
        Serial.printf("[SEND] ✗ Error %d: %s\n", httpCode, response.c_str());
        cloudPending = true;
        lastRetryTime = millis();
        retryInterval = min(retryInterval * 2, MAX_RETRY_INTERVAL_MS);
    }
    https.end();

    // Power down WiFi
    powerDownWiFi();
}

// ===================== DEEP SLEEP =====================
void goToDeepSleep(uint32_t seconds) {
    if (!setupComplete) {
        Serial.println("[SLEEP] Skipped (setup in progress).");
        return;
    }
    Serial.printf("[SLEEP] Deep sleep for %u s\n", seconds);
    currentState = MasterState::SLEEP;
    digitalWrite(PIN_LED, LOW);
    powerHX711(false);
    powerDownWiFi(); // ensure WiFi off
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
}

// ===================== NTP =====================
bool syncNTPTime() {
    if (WiFi.status() != WL_CONNECTED) return false;
    for (int i = 0; i < NTP_SERVER_COUNT; i++) {
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVERS[i]);
        uint32_t start = millis();
        while (millis() - start < NTP_TIMEOUT_MS) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
                Serial.printf("[NTP] ✓ %s\n", buf);
                return true;
            }
            delay(100);
        }
    }
    return false;
}

uint32_t getCurrentTimestamp() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) return (uint32_t)mktime(&timeinfo);
    // fallback (if NTP never synced)
    return 1735689600 + (millis() / 1000);
}

// ===================== BATTERY =====================
void checkBattery() {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += analogRead(PIN_BAT_ADC);
        delay(2);
    }
    float avgAdc = sum / 10.0f;
    batteryVoltageMV = (avgAdc / 4095.0f) * 3300.0f * 2.0f;
    Serial.printf("[BAT] %.2f mV\n", batteryVoltageMV);

    if (batteryVoltageMV < BATTERY_CRITICAL_THRESHOLD_MV) {
        Serial.println("[BAT] CRITICAL! Permanent sleep.");
        saveToNVM(KEY_LAST_WEIGHT, lastStableWeight);
        saveToNVM(KEY_INVENTORY, totalInventory);
        esp_sleep_enable_timer_wakeup(3600000000ULL);
        esp_deep_sleep_start();
    }
}

// ===================== LED =====================
void updateLED() {
    uint32_t now = millis();
    bool lowBattery = (batteryVoltageMV < BATTERY_LOW_THRESHOLD_MV);

    LEDMode mode = LEDMode::OFF;

    if (!setupComplete) {
        mode = LEDMode::BLINK_SLOW;
    } else {
        if (currentState == MasterState::STABLE) mode = LEDMode::SOLID;
        else if (currentState == MasterState::UNSTABLE || currentState == MasterState::RECOVERING) mode = LEDMode::BLINK_FAST;
        else if (currentState == MasterState::CONNECTING) mode = LEDMode::BLINK_FAST;
        else if (currentState == MasterState::ERROR) mode = LEDMode::BLINK_FAST;
        else mode = LEDMode::OFF;
    }

    if (lowBattery) {
        mode = LEDMode::PULSE_LOW_BATTERY;
    }

    bool output = false;
    switch (mode) {
        case LEDMode::OFF: output = false; break;
        case LEDMode::SOLID: output = true; break;
        case LEDMode::BLINK_FAST: output = ((now / 200) % 2) == 0; break;
        case LEDMode::BLINK_SLOW: output = ((now / 500) % 2) == 0; break;
        case LEDMode::PULSE_SHORT: output = ((now % 1000) < 100); break;
        case LEDMode::PULSE_LOW_BATTERY: output = ((now % 1000) < 100); break;
        default: output = false;
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

// ===================== WiFi & AP =====================
void handleWiFi() {
    uint32_t now = millis();

    if (wifiState == WiFiState::ATTEMPTING) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiState = WiFiState::CONNECTED;
            wifi_connected = true;
            wifi_powered_off = false;
            Serial.printf("[WIFI] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
            if (currentState == MasterState::CONNECTING) currentState = MasterState::STABLE;
            if (apModeInitialized) stopAPMode();
            memcpy(saved_bssid, WiFi.BSSID(), 6);
            saved_channel = WiFi.channel();
            return;
        }
        if (now - wifiAttemptStartTime >= 15000) {
            Serial.println("[WIFI] Timeout. AP mode.");
            wifiState = WiFiState::IDLE;
            if (currentState == MasterState::CONNECTING) currentState = MasterState::PROVISIONING;
            startAPMode();
        }
    }

    if (wifiState == WiFiState::CONNECTED && WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Lost. Reconnecting...");
        wifiState = WiFiState::IDLE;
        wifi_connected = false;
        wifi_powered_off = false;
        String ssid = loadStringFromNVM(KEY_SSID, "");
        String pass = loadStringFromNVM(KEY_PASS, "");
        if (ssid.length() > 0) {
            wifiState = WiFiState::ATTEMPTING;
            wifiAttemptStartTime = millis();
            if (saved_channel > 0 && saved_channel <= 14) {
                WiFi.begin(ssid.c_str(), pass.c_str(), saved_channel, saved_bssid);
            } else {
                WiFi.begin(ssid.c_str(), pass.c_str());
            }
        }
    }

    if (wifiState == WiFiState::IDLE && wifiCredentialsStored && !wifi_powered_off) {
        // This is only for AP mode fallback; normally we connect on demand.
        // We keep this for completeness.
        wifiState = WiFiState::ATTEMPTING;
        wifiAttemptStartTime = millis();
        String ssid = loadStringFromNVM(KEY_SSID, "");
        String pass = loadStringFromNVM(KEY_PASS, "");
        if (saved_channel > 0 && saved_channel <= 14) {
            WiFi.begin(ssid.c_str(), pass.c_str(), saved_channel, saved_bssid);
        } else {
            WiFi.begin(ssid.c_str(), pass.c_str());
        }
    }
}

// ===================== AP MODE FUNCTIONS =====================
void startAPMode() {
    if (apModeInitialized) return;
    Serial.println("[AP] Starting...");
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
    setupComplete = false;
    wifi_powered_off = false;
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
    wifi_powered_off = true;
    Serial.println("[AP] Stopped.");
}

void handleRoot() {
    String html = "<!DOCTYPE html><html><head><title>Scale Setup</title>"
                  "<meta charset='UTF-8'></head><body>"
                  "<h2>Kitchen Scale Setup</h2>";
    if (!deviceConfigured) {
        html += "<p style='color:red;'><b>⚠️ Device not configured!</b></p>";
        html += "<form method='POST' action='/setup'>";
        html += "Device ID: <input type='text' name='deviceId' required><br><br>";
        html += "<input type='submit' value='Save Device ID'>";
        html += "</form><hr>";
    } else {
        html += "<p><b>✅ Device ID: " + deviceId + "</b></p>";
    }
    html += "<h3>Wi-Fi</h3><form method='POST' action='/connect'>";
    html += "SSID: <input type='text' name='ssid' required><br><br>";
    html += "Password: <input type='password' name='pass' required><br><br>";
    html += "<input type='submit' value='Connect WiFi'>";
    html += "</form>";
    if (deviceConfigured) {
        html += "<hr><p><b>Status:</b><br>Weight: " + String(currentWeight, 2) + " g<br>";
        html += "State: " + stateToString(currentState) + "<br>";
        html += "Battery: " + String(batteryVoltageMV, 0) + " mV</p>";
    }
    html += "</body></html>";
    webServer.send(200, "text/html", html);
}

void handleSetup() {
    if (webServer.method() == HTTP_POST) {
        String newId = webServer.arg("deviceId");
        newId.trim();
        if (newId.length() > 0) {
            deviceId = newId;
            deviceConfigured = true;
            saveStringToNVM(KEY_DEVICE_ID, deviceId);
            saveBoolToNVM(KEY_DEVICE_CONFIGURED, true);
            Serial.printf("[SETUP] Device ID: %s\n", deviceId.c_str());
            webServer.send(200, "text/html", "<html><body><h2>✅ Saved!</h2><a href='/'>Back</a></body></html>");
            setupComplete = true;
        } else {
            webServer.send(400, "text/html", "<html><body><h2>❌ Invalid</h2><a href='/'>Try again</a></body></html>");
        }
    } else {
        String html = "<html><body><h2>Set Device ID</h2><form method='POST'><input type='text' name='deviceId' required><br><input type='submit'></form></body></html>";
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
        webServer.send(200, "text/html", "<html><body><h2>✅ WiFi saved!</h2><a href='/'>Back</a></body></html>");
        stopAPMode();
        // We don't connect here – will connect on demand
        setupComplete = true;
        wifi_powered_off = true;
        Serial.printf("[AP] WiFi saved: %s\n", ssid.c_str());
    } else {
        webServer.send(400, "text/html", "<html><body><h2>❌ Invalid</h2><a href='/'>Try again</a></body></html>");
    }
}

// ===================== NVM HELPERS =====================
void initNVM() {
    preferences.begin(NVS_NS, false);
    if (!preferences.isKey(KEY_TARE)) preferences.putFloat(KEY_TARE, FACTORY_TARE_OFFSET);
    if (!preferences.isKey(KEY_SCALE)) preferences.putFloat(KEY_SCALE, FACTORY_SCALE_FACTOR);
    preferences.end();
}
void saveToNVM(const char* key, float value) { preferences.begin(NVS_NS, false); preferences.putFloat(key, value); preferences.end(); }
void saveStringToNVM(const char* key, const String& value) { preferences.begin(NVS_NS, false); preferences.putString(key, value); preferences.end(); }
void saveBoolToNVM(const char* key, bool value) { preferences.begin(NVS_NS, false); preferences.putBool(key, value); preferences.end(); }
float loadFloatFromNVM(const char* key, float defaultValue) { preferences.begin(NVS_NS, true); float val = preferences.getFloat(key, defaultValue); preferences.end(); return val; }
String loadStringFromNVM(const char* key, const String& defaultValue) { preferences.begin(NVS_NS, true); String val = preferences.getString(key, defaultValue); preferences.end(); return val; }
bool loadBoolFromNVM(const char* key, bool defaultValue) { preferences.begin(NVS_NS, true); bool val = preferences.getBool(key, defaultValue); preferences.end(); return val; }