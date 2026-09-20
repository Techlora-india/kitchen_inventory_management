// ============================================================================
//  Kitchen Inventory Scale v16.0
//  - Button: short press → tare, long press (10 s) → AP mode
//  - WiFi on demand, fast reconnect, wake-counter heartbeat
//  - Correct baseline comparison on unstable settling
//  - Persistent retry across deep sleep (RTC flag)
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

#include "OtaService.h"  // For OTA update checking and boot acknowledgment

const int CURRENT_VERSION = 0;
const char* versionUrl = "https://raw.githubusercontent.com/Techlora-india/kitchen_inventory_management/83bb2c4616f2f022d1729405eda17db1be59c41e/kitchent_inventory_io/var.txt";
const char* firmwareUrl = "https://raw.githubusercontent.com/Techlora-india/kitchen_inventory_management/83bb2c4616f2f022d1729405eda17db1be59c41e/kitchent_inventory_io/.pio/build/seeed_xiao_esp32c3/firmware.bin";
const char* deviceId = "kim-001";

const char* firebaseBootAckBaseUrl = nullptr;
// Optional database secret/token if your DB rules require auth.
// Leave empty string if your rules allow write for this specific path.
const char* firebaseAuthToken = "";

const OtaConfig otaConfig = {
	CURRENT_VERSION,
	versionUrl,
	firmwareUrl,
	deviceId,
	firebaseBootAckBaseUrl,
	firebaseAuthToken,
};



// ===================== HARDWARE PINS =====================
#define LOADCELL_DOUT_PIN  D4
#define LOADCELL_SCK_PIN   D5
#define PIN_HX711_POWER    D6
#define PIN_LED            D3
#define PIN_BAT_ADC        D0
#define PIN_TARE_BUTTON    D2

// ===================== CALIBRATION =====================
const float FACTORY_TARE_OFFSET  = 49000.00f;
const float FACTORY_SCALE_FACTOR = 421.50f;

// ===================== SYSTEM CONSTANTS =====================
const float    STABILITY_THRESHOLD_GRAMS     = 3.0f;
const uint32_t SETTLE_TIME_MS                = 1000;
const float    CHANGE_THRESHOLD_GRAMS        = 3.0f;
const uint32_t NORMAL_SLEEP_SEC              = 60;
const uint32_t UNSTABLE_REPOLL_MS            = 5000;
const uint32_t FAST_POLL_INTERVAL_MS         = 100;
const float    BATTERY_LOW_THRESHOLD_MV      = 3500.0f;
const float    BATTERY_CRITICAL_THRESHOLD_MV = 3200.0f;
const uint32_t INITIAL_RETRY_INTERVAL_MS     = 60000;
const uint32_t MAX_RETRY_INTERVAL_MS         = 3600000;
const uint32_t LONG_PRESS_MS                 = 10000;
const uint32_t AP_CONNECT_TIMEOUT_MS         = 10000;
const uint32_t AP_EXIT_DELAY_MS              = 5000;
const uint32_t NTP_RESYNC_INTERVAL_SEC       = 86400;
const uint32_t BUTTON_RELEASE_TIMEOUT_MS     = 30000;

// ===================== NTP =====================
const char* NTP_SERVERS[] = {"pool.ntp.org", "time.google.com"};
const int   NTP_SERVER_COUNT = 2;
const long  NTP_TIMEOUT_MS = 10000;
const long  GMT_OFFSET_SEC = 0;
const int   DAYLIGHT_OFFSET_SEC = 0;

// ===================== RTC MEMORY =====================
RTC_NOINIT_ATTR uint8_t  saved_bssid[6];
RTC_NOINIT_ATTR uint8_t  saved_channel;
RTC_NOINIT_ATTR uint32_t magic_check;
RTC_NOINIT_ATTR uint32_t boot_count;
RTC_NOINIT_ATTR uint32_t rtc_ntp_sync_time;
RTC_NOINIT_ATTR uint32_t rtc_last_history_ts;
RTC_NOINIT_ATTR uint32_t rtc_fallback_ts;
RTC_NOINIT_ATTR uint32_t rtc_last_send_boot;
RTC_NOINIT_ATTR uint8_t  rtc_cloud_pending;
#define MEMORY_MAGIC_NUMBER 0xDEADBEEF

volatile bool wifi_connected = false;
bool wifi_powered_off = true;

// ===================== ENUMS =====================
enum class MasterState : uint8_t {
    BOOT, CONNECTING, PROVISIONING, STABLE, UNSTABLE,
    ABSENT, RECOVERING, SLEEP, ERROR
};
enum class WiFiState   : uint8_t { IDLE, ATTEMPTING, CONNECTED, AP_ACTIVE };
enum class LEDMode     : uint8_t { OFF, SOLID, BLINK_FAST, BLINK_SLOW, PULSE_LOW_BATTERY };
enum class ButtonEvent : uint8_t { NONE, SHORT_PRESS, LONG_PRESS };
enum class SendReason  : uint8_t { WEIGHT_CHANGE, HEARTBEAT, CALIBRATED, BOOT };

// ===================== OBJECTS =====================
HX711 scale;
Preferences preferences;
DNSServer dnsServer;
WebServer webServer(80);

// ===================== NVM KEYS =====================
const char* NVS_NS       = "scale";
const char* KEY_TARE     = "tare";
const char* KEY_SCALE    = "scaleFac";
const char* KEY_SSID     = "ssid";
const char* KEY_PASS     = "pass";
const char* KEY_DEVICE_ID= "deviceId";
const char* KEY_LAST_WEIGHT = "lastWeight";
const char* KEY_DEVICE_CONFIGURED = "configured";
const char* KEY_HEARTBEAT = "heartbeat";

// ===================== GLOBAL STATE =====================
MasterState currentState = MasterState::BOOT;
WiFiState   wifiState    = WiFiState::IDLE;

float currentWeight     = 0.0f;
float filteredWeight    = 0.0f;
float lastStableWeight  = 0.0f;
float batteryVoltageMV  = 4200.0f;
float currentTareOffset = FACTORY_TARE_OFFSET;

bool calibrating   = false;
bool setupComplete = false;

float weightBuffer[10];
uint8_t weightBufferIdx = 0, weightBufferCount = 0;
uint32_t stabilityCounter = 0;
bool isStableFlag = false;

uint32_t lastLoopTime = 0, lastBatteryReadTime = 0;
uint32_t lastStableTime = 0, wifiAttemptStartTime = 0;

String apSSID = "SmartJar-Setup";
IPAddress apIP(192, 168, 4, 1);
bool apModeInitialized = false;
bool wifiCredentialsStored = false;
String deviceId = "";
bool deviceConfigured = false;

bool cloudPending = false;
uint32_t lastRetryTime = 0;
uint32_t retryInterval = INITIAL_RETRY_INTERVAL_MS;

bool     apConnecting       = false;
uint32_t apConnectingStart  = 0;
String   apTempSSID         = "";
String   apTempPass         = "";
bool     apConnectionFailed = false;
bool     apExitPending      = false;
uint32_t apExitTime         = 0;

uint32_t heartbeatIntervalSec = 600;

volatile ButtonEvent pendingButtonEvent = ButtonEvent::NONE;

// ===================== PROTOTYPES =====================
void initNVM();
void saveToNVM(const char* k, float v);
void saveStringToNVM(const char* k, const String& v);
void saveBoolToNVM(const char* k, bool v);
float  loadFloatFromNVM(const char* k, float d);
String loadStringFromNVM(const char* k, const String& d);
bool   loadBoolFromNVM(const char* k, bool d);
void   updateLED();
void   checkStability();
void   checkBattery();
void   handleWiFi();
void   startAPMode();
void   stopAPMode();
String stateToString(MasterState s);
void   goToDeepSleep(uint32_t seconds);
void   handleRoot();
void   handleConnect();
void   handleConnecting();
void   handleSetup();
void   handleConfig();
float  readWeight();
float  getAverageWeight(int samples);
bool   isWeightStable();
void   powerHX711(bool on);
void   processStableWeight(float weight);
void   processUnstableWeight(float weight);
bool   syncNTPTime();
uint32_t getCurrentTimestamp();
void   attemptSend(SendReason reason);
void   onWifiGotIP(WiFiEvent_t e, WiFiEventInfo_t i);
void   powerDownWiFi();
bool   connectWiFiIfNeeded();
ButtonEvent checkTareButtonEvent();
ButtonEvent waitForButtonRelease();
void   performTareCalibration();
void   enterProvisioningMode();
float  readBatteryVoltage();
void   sendHeartbeatIfDue();
String htmlEscape(const String& s);
String reasonToString(SendReason r);
void   handlePendingButton();
void   printIST(const char* prefix);

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== KITCHEN SCALE v16.0 ===\n");

    if (magic_check != MEMORY_MAGIC_NUMBER) {
        magic_check = MEMORY_MAGIC_NUMBER;
        boot_count = 1;
        saved_channel = 0;
        rtc_ntp_sync_time = 0;
        rtc_last_history_ts = 0;
        rtc_fallback_ts = 1735689600;
        rtc_last_send_boot = 0;
        rtc_cloud_pending = 0;
        Serial.println("--- Cold boot ---");
    } else {
        boot_count++;
        Serial.printf("--- Wake #%u ---\n", boot_count);
    }

    pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BAT_ADC, ANALOG);
    pinMode(PIN_HX711_POWER, OUTPUT); powerHX711(false);
    pinMode(PIN_TARE_BUTTON, INPUT_PULLUP);

    // ── Handle wake from button IMMEDIATELY ──
    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    if (wake == ESP_SLEEP_WAKEUP_GPIO) {
        Serial.println("[WAKE] button");
        delay(50);
        if (digitalRead(PIN_TARE_BUTTON) == LOW) {
            pendingButtonEvent = waitForButtonRelease();
            Serial.printf("[BTN] wake event: %s\n",
                pendingButtonEvent == ButtonEvent::LONG_PRESS ? "LONG" : "SHORT");
        } else {
            Serial.println("[BTN] already released - ignoring");
        }
    }

    initNVM();
    Serial.println("[✓] NVM ready");

    currentTareOffset = loadFloatFromNVM(KEY_TARE, FACTORY_TARE_OFFSET);
    heartbeatIntervalSec = (uint32_t)loadFloatFromNVM(KEY_HEARTBEAT, 600.0f);
    if (heartbeatIntervalSec < 60) heartbeatIntervalSec = 60;
    Serial.printf("[✓] Tare %.2f | heartbeat %u s\n", currentTareOffset, heartbeatIntervalSec);

    lastStableWeight = loadFloatFromNVM(KEY_LAST_WEIGHT, 0.0f);
    deviceId         = loadStringFromNVM(KEY_DEVICE_ID, "");
    deviceConfigured = loadBoolFromNVM(KEY_DEVICE_CONFIGURED, false);

    cloudPending = (rtc_cloud_pending == 1);
    if (cloudPending) Serial.println("[STATE] restoring pending send flag");

    Serial.printf("[✓] LastStable %.2f g\n", lastStableWeight);
    if (deviceConfigured && deviceId.length() > 0)
        Serial.printf("[✓] Device: %s\n", deviceId.c_str());

    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(FACTORY_SCALE_FACTOR);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(onWifiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    powerDownWiFi();

    if (!deviceConfigured || deviceId.length() == 0) {
        Serial.println("[!] Not configured → AP mode.");
        currentState = MasterState::PROVISIONING;
        setupComplete = false;
        startAPMode();
    } else {
        setupComplete = true;
        String ssid = loadStringFromNVM(KEY_SSID, "");
        String pass = loadStringFromNVM(KEY_PASS, "");
        wifiCredentialsStored = (ssid.length() > 0 && pass.length() > 0);
    }

    // Seed weight buffer
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

    Serial.println("[✓] Setup done.");

    if (setupComplete && pendingButtonEvent != ButtonEvent::NONE) {
        handlePendingButton();
    }
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

    if (setupComplete && pendingButtonEvent != ButtonEvent::NONE) {
        handlePendingButton();
        return;
    }

    if (apExitPending && now > apExitTime) {
        apExitPending = false;
        Serial.println("[AP] Exit — connection verified.");
        dnsServer.stop();
        webServer.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        apModeInitialized = false;
        wifiState = WiFiState::CONNECTED;
        currentState = MasterState::STABLE;
        setupComplete = true;
        cloudPending = true;
        rtc_cloud_pending = 1;
        Serial.println("[AP] AP stopped, resuming normal.");
    }

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

    if (!calibrating && !apConnecting && !apExitPending) {
        ButtonEvent ev = checkTareButtonEvent();
        if (ev == ButtonEvent::SHORT_PRESS) { performTareCalibration(); return; }
        if (ev == ButtonEvent::LONG_PRESS)  { enterProvisioningMode();  return; }
    }

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
            currentState = MasterState::STABLE;
            processStableWeight(currentWeight);
            if (currentState != MasterState::PROVISIONING &&
                currentState != MasterState::CONNECTING)
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

// ===================== PENDING BUTTON =====================
void handlePendingButton() {
    ButtonEvent ev = pendingButtonEvent;
    pendingButtonEvent = ButtonEvent::NONE;

    if (ev == ButtonEvent::SHORT_PRESS) {
        Serial.println("[BTN] → tare calibration");
        performTareCalibration();
    } else if (ev == ButtonEvent::LONG_PRESS) {
        Serial.println("[BTN] → AP mode");
        enterProvisioningMode();
    }
}

// ===================== WIFI =====================
void onWifiGotIP(WiFiEvent_t, WiFiEventInfo_t) { wifi_connected = true; }

void powerDownWiFi() {
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifi_powered_off = true;
        wifi_connected = false;
        wifiState = WiFiState::IDLE;
        Serial.println("[WIFI] off");
    }
}

bool connectWiFiIfNeeded() {
    if (WiFi.status() == WL_CONNECTED) { wifi_powered_off = false; return true; }
    if (!wifiCredentialsStored) { Serial.println("[WIFI] no creds"); return false; }

    Serial.println("[WIFI] connecting...");
    String ssid = loadStringFromNVM(KEY_SSID, "");
    String pass = loadStringFromNVM(KEY_PASS, "");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(onWifiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    wifi_connected = false;
    wifi_powered_off = false;

    if (saved_channel > 0 && saved_channel <= 14)
        WiFi.begin(ssid.c_str(), pass.c_str(), saved_channel, saved_bssid);
    else
        WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (!wifi_connected && (millis() - start) < 5000) delay(10);

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] connected %lu ms, IP %s\n",
                      millis() - start, WiFi.localIP().toString().c_str());
        memcpy(saved_bssid, WiFi.BSSID(), 6);
        saved_channel = WiFi.channel();
        return true;
    }
    Serial.println("[WIFI] failed");
    powerDownWiFi();
    return false;
}

void handleWiFi() {
    uint32_t now = millis();

    if (wifiState == WiFiState::ATTEMPTING) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiState = WiFiState::CONNECTED;
            wifi_connected = true;
            wifi_powered_off = false;
            memcpy(saved_bssid, WiFi.BSSID(), 6);
            saved_channel = WiFi.channel();
            return;
        }
        if (now - wifiAttemptStartTime >= 15000) {
            wifiState = WiFiState::IDLE;
            if (currentState == MasterState::CONNECTING) currentState = MasterState::PROVISIONING;
            startAPMode();
        }
    }

    if (wifiState == WiFiState::CONNECTED && WiFi.status() != WL_CONNECTED) {
        wifiState = WiFiState::IDLE;
        wifi_connected = false;
    }
}

// ===================== AP MODE =====================
void startAPMode() {
    if (apModeInitialized) return;
    Serial.println("[AP] start");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSSID.c_str());
    dnsServer.start(53, "*", apIP);
    webServer.on("/",           HTTP_GET,  handleRoot);
    webServer.on("/connect",    HTTP_POST, handleConnect);
    webServer.on("/connecting", HTTP_GET,  handleConnecting);
    webServer.on("/setup",      HTTP_GET,  handleSetup);
    webServer.on("/setup",      HTTP_POST, handleSetup);
    webServer.on("/config",     HTTP_POST, handleConfig);
    webServer.begin();
    apModeInitialized = true;
    wifiState = WiFiState::AP_ACTIVE;
    currentState = MasterState::PROVISIONING;
    setupComplete = false;
    wifi_powered_off = false;
    apConnecting = false;
    apConnectionFailed = false;
    Serial.printf("[AP] SSID %s IP %s\n", apSSID.c_str(), apIP.toString().c_str());
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
    Serial.println("[AP] stopped");
}

// ===================== HX711 =====================
void powerHX711(bool on) {
    digitalWrite(PIN_HX711_POWER, on ? HIGH : LOW);
    if (on) delay(50);
}

float readWeight() {
    if (!scale.is_ready()) return currentWeight;
    long raw = scale.read();
    return ((float)raw + currentTareOffset) / FACTORY_SCALE_FACTOR;
}

float getAverageWeight(int n) {
    if (n < 1) return 0.0f;
    float sum = 0; int valid = 0;
    for (int i = 0; i < n; i++) {
        float w = readWeight();
        if (w > -1000 && w < 10000) { sum += w; valid++; }
        delay(20);
    }
    return (valid == 0) ? 0.0f : sum / valid;
}

// ===================== STABILITY =====================
bool isWeightStable() {
    if (weightBufferCount < 10) return false;
    float mn = 9999, mx = -9999;
    for (int i = 0; i < 10; i++) {
        if (weightBuffer[i] < mn) mn = weightBuffer[i];
        if (weightBuffer[i] > mx) mx = weightBuffer[i];
    }
    return (mx - mn) < STABILITY_THRESHOLD_GRAMS;
}

void checkStability() {
    if (isWeightStable()) stabilityCounter++;
    else stabilityCounter = 0;
    isStableFlag = (stabilityCounter >= (SETTLE_TIME_MS / 100));
}

// ===================== PROCESS =====================
void processStableWeight(float w) {
    float delta = w - lastStableWeight;

    if (fabs(delta) < CHANGE_THRESHOLD_GRAMS) {
        lastStableWeight = w;
        saveToNVM(KEY_LAST_WEIGHT, w);

        if (cloudPending) {
            Serial.println("[PROCESS] retrying pending send");
            attemptSend(SendReason::WEIGHT_CHANGE);
        } else {
            sendHeartbeatIfDue();
        }
        return;
    }

    Serial.printf("[PROCESS] Δ %.2f g → %.2f g\n", delta, w);
    lastStableWeight = w;
    saveToNVM(KEY_LAST_WEIGHT, w);
    attemptSend(SendReason::WEIGHT_CHANGE);
}

void processUnstableWeight(float startW) {
    currentState = MasterState::UNSTABLE;
    Serial.printf("[UNSTABLE] fast poll (baseline %.2f g)\n", lastStableWeight);
    uint32_t start = millis();
    float finalW = startW;

    while (true) {
        ButtonEvent ev = checkTareButtonEvent();
        if (ev != ButtonEvent::NONE) {
            Serial.println("[UNSTABLE] button event during poll - deferring");
            pendingButtonEvent = ev;
            powerHX711(false);
            currentState = MasterState::STABLE;
            return;
        }

        powerHX711(true);
        float r = readWeight();
        powerHX711(false);
        filteredWeight = (filteredWeight * 0.7f) + (r * 0.3f);
        currentWeight = filteredWeight;
        weightBuffer[weightBufferIdx] = currentWeight;
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;
        checkStability();

        if (isStableFlag) {
            powerHX711(true);
            finalW = getAverageWeight(5);
            powerHX711(false);
            Serial.printf("[UNSTABLE] settled at %.2f g\n", finalW);
            break;
        }
        if (millis() - start > UNSTABLE_REPOLL_MS) {
            Serial.printf("[UNSTABLE] repoll after %u ms\n", UNSTABLE_REPOLL_MS);
            powerHX711(false);
            esp_sleep_enable_timer_wakeup(UNSTABLE_REPOLL_MS * 1000ULL);
            esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TARE_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
            esp_deep_sleep_start();
            return;
        }
        delay(FAST_POLL_INTERVAL_MS);
    }

    currentState = MasterState::STABLE;

    // FIX: compare against original baseline, not startW
    float delta = finalW - lastStableWeight;
    Serial.printf("[UNSTABLE] baseline %.2f → final %.2f, Δ %.2f g\n",
                  lastStableWeight, finalW, delta);

    if (fabs(delta) >= CHANGE_THRESHOLD_GRAMS) {
        lastStableWeight = finalW;
        saveToNVM(KEY_LAST_WEIGHT, finalW);
        attemptSend(SendReason::WEIGHT_CHANGE);
    } else {
        lastStableWeight = finalW;
        saveToNVM(KEY_LAST_WEIGHT, finalW);
        if (cloudPending) attemptSend(SendReason::WEIGHT_CHANGE);
        else              sendHeartbeatIfDue();
    }
    goToDeepSleep(NORMAL_SLEEP_SEC);
}

// ===================== HEARTBEAT =====================
void sendHeartbeatIfDue() {
    if (rtc_last_send_boot == 0) {
        rtc_last_send_boot = boot_count;
        return;
    }
    uint32_t elapsedWakes = boot_count - rtc_last_send_boot;
    uint32_t elapsedSec   = elapsedWakes * NORMAL_SLEEP_SEC;

    if (elapsedSec >= heartbeatIntervalSec) {
        Serial.printf("[HEARTBEAT] %u wakes / %u s → send\n", elapsedWakes, elapsedSec);
        attemptSend(SendReason::HEARTBEAT);
    }
}

// ===================== CLOUD SEND =====================
String reasonToString(SendReason r) {
    switch (r) {
        case SendReason::WEIGHT_CHANGE: return "change";
        case SendReason::HEARTBEAT:     return "heartbeat";
        case SendReason::CALIBRATED:    return "calibrated";
        case SendReason::BOOT:          return "boot";
        default:                        return "unknown";
    }
}

void attemptSend(SendReason reason) {
    if (!deviceConfigured || deviceId.length() == 0) {
        cloudPending = false; rtc_cloud_pending = 0; return;
    }

    if (!connectWiFiIfNeeded()) {
        cloudPending = true; rtc_cloud_pending = 1;
        powerDownWiFi();
        return;
    }

    struct tm ti;
    bool haveTime = (getLocalTime(&ti) && ti.tm_year >= 120);
    if (!haveTime) {
        Serial.println("[NTP] sync requested");
        if (syncNTPTime()) {
            rtc_ntp_sync_time = getCurrentTimestamp();
            haveTime = true;
        }
    }
    if (!haveTime) rtc_fallback_ts++;

    uint32_t ts = getCurrentTimestamp();
    if (ts <= rtc_last_history_ts) ts = rtc_last_history_ts + 1;
    rtc_last_history_ts = ts;

    batteryVoltageMV = readBatteryVoltage();

    float w = (lastStableWeight < 0) ? 0.0f : lastStableWeight;
    String url = String(FIREBASE_HOST) + "telemetry/" + deviceId + ".json";

    StaticJsonDocument<256> doc;
    doc["currentWeight"]  = w;
    doc["batteryVoltage"] = batteryVoltageMV;
    doc["lastUpdated"]    = ts;
    doc["heartbeatSec"]   = heartbeatIntervalSec;
    doc["history/" + String(ts)] = w;

    String body; serializeJson(doc, body);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");
    int code = https.PATCH(body);

    if (code == 200) {
        Serial.printf("[SEND] ✓ %.2f g (%.0f mV, %s)\n",
                      w, batteryVoltageMV, reasonToString(reason).c_str());
        cloudPending = false;
        rtc_cloud_pending = 0;
        retryInterval = INITIAL_RETRY_INTERVAL_MS;
        rtc_last_send_boot = boot_count;
    } else {
        Serial.printf("[SEND] ✗ %d — will retry on next wake\n", code);
        cloudPending = true;
        rtc_cloud_pending = 1;
        lastRetryTime = millis();
        retryInterval = min(retryInterval * 2, MAX_RETRY_INTERVAL_MS);
    }
    https.end();
    powerDownWiFi();
}

// ===================== SLEEP =====================
void goToDeepSleep(uint32_t seconds) {
    if (!setupComplete) return;
    Serial.printf("[SLEEP] %u s\n", seconds);
    currentState = MasterState::SLEEP;
    digitalWrite(PIN_LED, LOW);
    powerHX711(false);
    powerDownWiFi();

    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TARE_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

// ===================== NTP =====================
void printIST(const char* prefix) {
    struct tm ti;
    if (!getLocalTime(&ti)) return;
    time_t utc = mktime(&ti);
    time_t ist = utc + 19800;
    struct tm istTm;
    gmtime_r(&ist, &istTm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &istTm);
    Serial.printf("%s %s IST\n", prefix, buf);
}

bool syncNTPTime() {
    if (WiFi.status() != WL_CONNECTED) return false;
    static bool alreadySynced = false;
    if (alreadySynced) return true;

    for (int i = 0; i < NTP_SERVER_COUNT; i++) {
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVERS[i]);
        uint32_t start = millis();
        while (millis() - start < NTP_TIMEOUT_MS) {
            struct tm ti;
            if (getLocalTime(&ti) && ti.tm_year >= 120) {
                printIST("[NTP] ✓");
                alreadySynced = true;
                return true;
            }
            delay(250);
        }
    }
    return false;
}

uint32_t getCurrentTimestamp() {
    struct tm ti;
    if (getLocalTime(&ti) && ti.tm_year >= 120) return (uint32_t)mktime(&ti);
    return rtc_fallback_ts;
}

// ===================== BATTERY =====================
float readBatteryVoltage() {
    static bool adcInit = false;
    if (!adcInit) {
        (void)analogRead(PIN_BAT_ADC);
        delay(5);
        analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
        delay(5);
        adcInit = true;
    }
    uint32_t samples[15];
    for (int i = 0; i < 15; i++) { samples[i] = analogReadMilliVolts(PIN_BAT_ADC); delay(2); }
    for (int i = 0; i < 14; i++)
        for (int j = i + 1; j < 15; j++)
            if (samples[i] > samples[j]) {
                uint32_t t = samples[i]; samples[i] = samples[j]; samples[j] = t;
            }
    uint32_t pinmV = (samples[6] + samples[7] + samples[8]) / 3;
    return pinmV * 2.0f;
}

void checkBattery() {
    batteryVoltageMV = readBatteryVoltage();
    Serial.printf("[BAT] %.0f mV\n", batteryVoltageMV);
    if (batteryVoltageMV < BATTERY_CRITICAL_THRESHOLD_MV) {
        saveToNVM(KEY_LAST_WEIGHT, lastStableWeight);
        esp_sleep_enable_timer_wakeup(3600000000ULL);
        esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TARE_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
        esp_deep_sleep_start();
    }
}

// ===================== LED =====================
void updateLED() {
    uint32_t now = millis();
    bool lowBat = (batteryVoltageMV < BATTERY_LOW_THRESHOLD_MV);

    LEDMode m = LEDMode::OFF;

    if (calibrating || apConnecting) {
        m = LEDMode::BLINK_FAST;
    } else if (!setupComplete) {
        m = LEDMode::SOLID;
    } else {
        switch (currentState) {
            case MasterState::STABLE:      m = LEDMode::SOLID;      break;
            case MasterState::UNSTABLE:
            case MasterState::CONNECTING:
            case MasterState::ERROR:       m = LEDMode::BLINK_FAST; break;
            default:                        m = LEDMode::OFF;        break;
        }
    }

    if (lowBat) m = LEDMode::PULSE_LOW_BATTERY;

    bool on = false;
    switch (m) {
        case LEDMode::OFF:               on = false; break;
        case LEDMode::SOLID:             on = true;  break;
        case LEDMode::BLINK_FAST:        on = ((now / 200) % 2) == 0; break;
        case LEDMode::BLINK_SLOW:        on = ((now / 500) % 2) == 0; break;
        case LEDMode::PULSE_LOW_BATTERY: on = ((now % 1000) < 100);    break;
    }
    digitalWrite(PIN_LED, on ? HIGH : LOW);
}

String stateToString(MasterState s) {
    switch (s) {
        case MasterState::BOOT:         return "BOOT";
        case MasterState::CONNECTING:   return "CONNECTING";
        case MasterState::PROVISIONING: return "PROVISIONING";
        case MasterState::STABLE:       return "STABLE";
        case MasterState::UNSTABLE:     return "UNSTABLE";
        case MasterState::ABSENT:       return "ABSENT";
        case MasterState::RECOVERING:   return "RECOVERING";
        case MasterState::SLEEP:        return "SLEEP";
        case MasterState::ERROR:        return "ERROR";
        default:                        return "UNKNOWN";
    }
}

// ===================== HELPERS =====================
String htmlEscape(const String& s) {
    String o = s;
    o.replace("&", "&amp;"); o.replace("<", "&lt;");
    o.replace(">", "&gt;"); o.replace("\"", "&quot;");
    o.replace("'", "&#39;");
    return o;
}

// ===================== WEB UI =====================
void handleRoot() {
    String html = R"HTML(<!DOCTYPE html><html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Kitchen Scale</title>
<style>
  :root{font-family:system-ui,-apple-system,sans-serif}
  body{margin:0;padding:24px;background:#f4f6f8;color:#222}
  .card{max-width:480px;margin:0 auto;background:#fff;border-radius:14px;
        padding:24px;box-shadow:0 2px 12px rgba(0,0,0,.06)}
  h2{margin-top:0;font-size:20px}
  h3{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:#888;margin-top:28px}
  label{display:block;font-size:13px;margin:12px 0 4px;color:#555}
  select,input[type=text],input[type=password],input[type=number]{
      width:100%;padding:10px 12px;font-size:15px;
      border:1px solid #d0d5da;border-radius:8px;box-sizing:border-box}
  input[type=submit]{width:100%;margin-top:16px;padding:12px;
      background:#1976d2;color:#fff;font-size:15px;border:none;
      border-radius:8px;cursor:pointer}
  input[type=submit]:hover{background:#1565c0}
  .alert{background:#fdecea;color:#b71c1c;padding:12px;border-radius:8px;font-size:14px;margin:12px 0}
  .ok{background:#e8f5e9;color:#1b5e20;padding:12px;border-radius:8px;font-size:14px;margin:12px 0}
  .chip{display:inline-block;padding:4px 10px;background:#eef2f7;
        border-radius:999px;font-size:12px;color:#555;margin:2px 4px 2px 0}
  a{color:#1976d2;text-decoration:none;font-size:13px}
</style></head><body><div class='card'><h2>🫙 Smart IoT Jar Setup</h2>)HTML";

    if (apConnectionFailed) {
        html += "<div class='alert'>❌ Connection failed. Please try again.</div>";
        apConnectionFailed = false;
    }

    if (!deviceConfigured) {
        html += "<div class='alert'>⚠️ Device ID not set.</div>";
        html += "<form method='POST' action='/setup'>";
        html += "<label>Device ID</label><input type='text' name='deviceId' required>";
        html += "<input type='submit' value='Save Device ID'></form>";
    } else {
        html += "<div class='ok'>✅ Device: <b>" + htmlEscape(deviceId) + "</b></div>";
    }

    html += "<h3>Wi-Fi Network</h3>";
    int n = WiFi.scanNetworks();
    html += "<form method='POST' action='/connect'>";
    html += "<label>Available networks</label><select name='ssid' required>";
    html += "<option value=''>— choose —</option>";
    for (int i = 0; i < n; i++) {
        String s = htmlEscape(WiFi.SSID(i));
        html += "<option value='" + s + "'>" + s +
                " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    html += "</select>";
    html += "<label>Password</label><input type='password' name='pass'>";
    html += "<input type='submit' value='Connect'></form>";
    html += "<p><a href='/'>🔄 Rescan</a></p>";

    html += "<h3>Device Settings</h3>";
    html += "<form method='POST' action='/config'>";
    html += "<label>Heartbeat interval (minutes)</label>";
    html += "<input type='number' name='hb' min='1' max='1440' value='";
    html += String(heartbeatIntervalSec / 60);
    html += "'><input type='submit' value='Save'></form>";

    if (deviceConfigured) {
        html += "<h3>Status</h3>";
        html += "<span class='chip'>⚖️ " + String(currentWeight, 1) + " g</span>";
        html += "<span class='chip'>🔋 " + String(batteryVoltageMV, 0) + " mV</span>";
        html += "<span class='chip'>📡 " + stateToString(currentState) + "</span>";
    }
    html += "</div></body></html>";
    webServer.send(200, "text/html", html);
}

void handleSetup() {
    if (webServer.method() == HTTP_POST) {
        String id = webServer.arg("deviceId"); id.trim();
        if (id.length() > 0) {
            deviceId = id;
            deviceConfigured = true;
            saveStringToNVM(KEY_DEVICE_ID, deviceId);
            saveBoolToNVM(KEY_DEVICE_CONFIGURED, true);
            Serial.printf("[SETUP] device=%s\n", deviceId.c_str());
            webServer.sendHeader("Location", "/");
            webServer.send(302, "text/plain", "");
        } else webServer.send(400, "text/plain", "Invalid");
    } else {
        webServer.send(200, "text/html",
            "<html><body><h2>Set Device ID</h2>"
            "<form method='POST'><input type='text' name='deviceId' required>"
            "<br><input type='submit'></form></body></html>");
    }
}

void handleConnect() {
    apTempSSID = webServer.arg("ssid");
    apTempPass = webServer.arg("pass");
    apTempSSID.trim();
    if (apTempSSID.length() == 0) { webServer.send(400, "text/plain", "SSID required"); return; }

    Serial.printf("[AP] try STA → %s\n", apTempSSID.c_str());
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(apTempSSID.c_str(), apTempPass.c_str());
    apConnecting = true;
    apConnectingStart = millis();
    apConnectionFailed = false;
    webServer.sendHeader("Location", "/connecting");
    webServer.send(302, "text/plain", "");
}

void handleConnecting() {
    if (!apConnecting) {
        webServer.sendHeader("Location", "/"); webServer.send(302, "text/plain", ""); return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[AP] ✓ STA connected");
        saveStringToNVM(KEY_SSID, apTempSSID);
        saveStringToNVM(KEY_PASS, apTempPass);
        wifiCredentialsStored = true;
        memcpy(saved_bssid, WiFi.BSSID(), 6);
        saved_channel = WiFi.channel();
        apConnecting = false;
        wifiState = WiFiState::CONNECTED;
        apExitPending = true;
        apExitTime = millis() + AP_EXIT_DELAY_MS;

        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
        html += "<style>body{font-family:system-ui;margin:24px;background:#f4f6f8}"
                ".card{max-width:480px;margin:0 auto;background:#fff;padding:24px;"
                "border-radius:14px;box-shadow:0 2px 12px rgba(0,0,0,.06)}"
                ".ok{background:#e8f5e9;color:#1b5e20;padding:12px;border-radius:8px}</style>";
        html += "</head><body><div class='card'><h2>✅ Connected!</h2>";
        html += "<div class='ok'>Wi-Fi credentials saved.</div>";
        html += "<p><b>SSID:</b> " + htmlEscape(apTempSSID) + "</p>";
        html += "<p><b>IP:</b> " + WiFi.localIP().toString() + "</p>";
        html += "<p>You may disconnect from the AP. Device resumes normal operation.</p>";
        html += "</div></body></html>";
        webServer.send(200, "text/html", html);
        return;
    }

    if (millis() - apConnectingStart > AP_CONNECT_TIMEOUT_MS) {
        Serial.println("[AP] ✗ timeout");
        apConnecting = false;
        apConnectionFailed = true;
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);

        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
        html += "<style>body{font-family:system-ui;margin:24px;background:#f4f6f8}"
                ".card{max-width:480px;margin:0 auto;background:#fff;padding:24px;"
                "border-radius:14px;box-shadow:0 2px 12px rgba(0,0,0,.06)}"
                ".err{background:#fdecea;color:#b71c1c;padding:12px;border-radius:8px}"
                "a{display:inline-block;margin-top:16px;padding:10px 16px;"
                "background:#1976d2;color:#fff;text-decoration:none;border-radius:8px}</style>";
        html += "</head><body><div class='card'><h2>❌ Connection Failed</h2>";
        html += "<div class='err'>Could not connect to <b>" + htmlEscape(apTempSSID)
              + "</b>. Check the password and try again.</div>";
        html += "<a href='/'>🔙 Try Again</a></div></body></html>";
        webServer.send(200, "text/html", html);
        return;
    }

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='1'>";
    html += "<style>body{font-family:system-ui;margin:24px;background:#f4f6f8}"
            ".card{max-width:480px;margin:0 auto;background:#fff;padding:24px;"
            "border-radius:14px;box-shadow:0 2px 12px rgba(0,0,0,.06);text-align:center}"
            ".spin{width:32px;height:32px;border:4px solid #ddd;border-top-color:#1976d2;"
            "border-radius:50%;animation:s 1s linear infinite;display:inline-block}"
            "@keyframes s{to{transform:rotate(360deg)}}</style>";
    html += "</head><body><div class='card'><h2>⏳ Connecting…</h2>";
    html += "<div class='spin'></div>";
    html += "<p>Attempting to join <b>" + htmlEscape(apTempSSID) + "</b></p>";
    html += "<p><small>Please wait…</small></p></div></body></html>";
    webServer.send(200, "text/html", html);
}

void handleConfig() {
    int mins = webServer.arg("hb").toInt();
    if (mins < 1)    mins = 1;
    if (mins > 1440) mins = 1440;
    heartbeatIntervalSec = (uint32_t)mins * 60;
    saveToNVM(KEY_HEARTBEAT, (float)heartbeatIntervalSec);
    Serial.printf("[CONFIG] heartbeat = %u s\n", heartbeatIntervalSec);
    webServer.sendHeader("Location", "/"); webServer.send(302, "text/plain", "");
}

// ===================== BUTTON STATE MACHINE =====================
ButtonEvent checkTareButtonEvent() {
    static bool rawLast = false, stableLast = false;
    static uint32_t lastRawChange = 0, pressStart = 0;
    static bool longFired = false;
    const uint32_t DEBOUNCE_MS = 50;

    bool raw = (digitalRead(PIN_TARE_BUTTON) == LOW);
    uint32_t now = millis();

    if (raw != rawLast) { lastRawChange = now; rawLast = raw; }

    if ((now - lastRawChange) >= DEBOUNCE_MS) {
        if (raw != stableLast) {
            stableLast = raw;
            if (stableLast) { pressStart = now; longFired = false; }
            else {
                if (!longFired) return ButtonEvent::SHORT_PRESS;
                longFired = false;
            }
        }
    }

    if (stableLast && !longFired && (now - pressStart) >= LONG_PRESS_MS) {
        longFired = true;
        return ButtonEvent::LONG_PRESS;
    }
    return ButtonEvent::NONE;
}

ButtonEvent waitForButtonRelease() {
    uint32_t start = millis();
    bool longReported = false;
    while (digitalRead(PIN_TARE_BUTTON) == LOW) {
        uint32_t held = millis() - start;
        if (!longReported && held >= LONG_PRESS_MS) {
            digitalWrite(PIN_LED, LOW);
            Serial.printf("[BTN] long press fired at %u ms (no release yet)\n", held);
            return ButtonEvent::LONG_PRESS;
        }
        digitalWrite(PIN_LED, ((millis() / 150) % 2) ? HIGH : LOW);
        if (held > BUTTON_RELEASE_TIMEOUT_MS) {
            Serial.println("[BTN] release timeout — SHORT");
            digitalWrite(PIN_LED, LOW);
            return ButtonEvent::SHORT_PRESS;
        }
        delay(20);
    }
    digitalWrite(PIN_LED, LOW);
    uint32_t held = millis() - start;
    Serial.printf("[BTN] released after %u ms → SHORT\n", held);
    return ButtonEvent::SHORT_PRESS;
}

// ===================== TARE =====================
void performTareCalibration() {
    Serial.println("[CAL] start, settling 10 s...");
    calibrating = true;
    powerHX711(true);

    uint32_t start = millis();
    while (millis() - start < 10000) {
        uint32_t e = millis() - start;
        digitalWrite(PIN_LED, ((e / 150) % 2) ? HIGH : LOW);
        if (wifiState == WiFiState::AP_ACTIVE) {
            dnsServer.processNextRequest();
            webServer.handleClient();
        }
        delay(20);
    }
    digitalWrite(PIN_LED, LOW);

    for (int i = 0; i < 5; i++) { if (scale.is_ready()) scale.read(); delay(30); }

    long sum = 0; int valid = 0;
    for (int i = 0; i < 20; i++) {
        if (scale.is_ready()) { sum += scale.read(); valid++; }
        delay(50);
    }
    powerHX711(false);

    if (valid < 10) {
        Serial.printf("[CAL] ✗ only %d/20 samples\n", valid);
        digitalWrite(PIN_LED, HIGH); delay(1500); digitalWrite(PIN_LED, LOW);
        calibrating = false;
        return;
    }

    long avg = sum / valid;
    float newOffset = -(float)avg;
    Serial.printf("[CAL] avgRaw=%ld → offset=%.2f (was %.2f)\n",
                  avg, newOffset, currentTareOffset);

    currentTareOffset = newOffset;
    saveToNVM(KEY_TARE, currentTareOffset);

    filteredWeight = currentWeight = lastStableWeight = 0.0f;
    saveToNVM(KEY_LAST_WEIGHT, 0.0f);

    weightBufferCount = weightBufferIdx = stabilityCounter = 0;
    isStableFlag = false;

    powerHX711(true);
    for (int i = 0; i < 10; i++) {
        weightBuffer[weightBufferIdx] = readWeight();
        weightBufferIdx = (weightBufferIdx + 1) % 10;
        if (weightBufferCount < 10) weightBufferCount++;
        delay(50);
    }
    powerHX711(false);

    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_LED, HIGH); delay(120);
        digitalWrite(PIN_LED, LOW);  delay(120);
    }
    Serial.println("[CAL] ✓ saved.");
    calibrating = false;

    attemptSend(SendReason::CALIBRATED);
}

// ===================== PROVISIONING TRIGGER =====================
void enterProvisioningMode() {
    Serial.println("[PROVISION] long press → AP mode");
    calibrating = false;
    apConnecting = false;
    apConnectionFailed = false;
    apExitPending = false;
    currentState = MasterState::PROVISIONING;
    setupComplete = false;
    startAPMode();
}

// ===================== NVM HELPERS =====================
void initNVM() {
    preferences.begin(NVS_NS, false);
    if (!preferences.isKey(KEY_TARE))       preferences.putFloat(KEY_TARE, FACTORY_TARE_OFFSET);
    if (!preferences.isKey(KEY_SCALE))      preferences.putFloat(KEY_SCALE, FACTORY_SCALE_FACTOR);
    if (!preferences.isKey(KEY_HEARTBEAT))  preferences.putFloat(KEY_HEARTBEAT, 600.0f);
    preferences.end();
}
void saveToNVM(const char* k, float v) { preferences.begin(NVS_NS, false); preferences.putFloat(k, v); preferences.end(); }
void saveStringToNVM(const char* k, const String& v) { preferences.begin(NVS_NS, false); preferences.putString(k, v); preferences.end(); }
void saveBoolToNVM(const char* k, bool v) { preferences.begin(NVS_NS, false); preferences.putBool(k, v); preferences.end(); }
float loadFloatFromNVM(const char* k, float d) { preferences.begin(NVS_NS, true); float v = preferences.getFloat(k, d); preferences.end(); return v; }
String loadStringFromNVM(const char* k, const String& d) { preferences.begin(NVS_NS, true); String v = preferences.getString(k, d); preferences.end(); return v; }
bool loadBoolFromNVM(const char* k, bool d) { preferences.begin(NVS_NS, true); bool v = preferences.getBool(k, d); preferences.end(); return v; }