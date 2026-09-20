#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

extern const char* kDefaultBootAckBaseUrl;

struct OtaConfig {
  int currentVersion;
  const char* versionUrl;
  const char* firmwareUrl;
  const char* deviceId;
  const char* firebaseBootAckBaseUrl;
  const char* firebaseAuthToken;
};

void check_ota(const OtaConfig& config);
void send_ota_ack(const OtaConfig& config, JsonDocument& payload);