#include "OtaService.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

const char* kDefaultBootAckBaseUrl =
  "https://techlora-369-default-rtdb.asia-southeast1.firebasedatabase.app/boot_ack";

static bool perform_ota_update(const OtaConfig& config) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);

  if (!http.begin(client, config.firmwareUrl)) {
    Serial.println("OTA: firmware URL open failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("OTA: firmware download failed (%d)\n", httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("OTA: invalid firmware size");
    http.end();
    return false;
  }

  if (!Update.begin(contentLength)) {
    Serial.println("OTA: not enough space for update");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("OTA: stream unavailable");
    Update.abort();
    http.end();
    return false;
  }

  size_t written = Update.writeStream(*stream);
  bool success = (written == static_cast<size_t>(contentLength)) && Update.end() && Update.isFinished();

  if (!success) {
    Serial.printf("OTA: update failed (err=%d)\n", Update.getError());
  }

  http.end();
  return success;
}

void check_ota(const OtaConfig& config) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: skipped (no WiFi)");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(client, config.versionUrl)) {
    Serial.println("OTA: version URL open failed");
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("OTA: version check failed (%d)\n", httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  payload.trim();
  payload.replace("\r", "");
  payload.replace("\n", "");

  int latestVersion = payload.toInt();
  if (latestVersion <= config.currentVersion) {
    Serial.println("OTA: up to date");
    return;
  }

  Serial.println("OTA: update available");
  if (perform_ota_update(config)) {
    Serial.println("OTA: update success, rebooting");
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("OTA: update failed");
  }
}

void send_ota_ack(const OtaConfig& config, JsonDocument& payload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("BOOT_ACK: skipped (no WiFi)");
    return;
  }

  const char* baseUrl = config.firebaseBootAckBaseUrl ? config.firebaseBootAckBaseUrl
                                                      : kDefaultBootAckBaseUrl;
  String url = String(baseUrl);
  url += "/";
  url += config.deviceId;
  url += ".json";
  if (config.firebaseAuthToken && strlen(config.firebaseAuthToken) > 0) {
    url += (url.indexOf('?') >= 0) ? "&auth=" : "?auth=";
    url += config.firebaseAuthToken;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.println("BOOT_ACK: URL open failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  if (!payload.containsKey("device_id")) {
    payload["device_id"] = config.deviceId;
  }

  String body;
  serializeJson(payload, body);

  int code = http.PUT(body);
  if (code > 0) {
    Serial.printf("BOOT_ACK: sent (HTTP %d)\n", code);
  } else {
    Serial.printf("BOOT_ACK: send failed (%d)\n", code);
  }

  http.end();
}