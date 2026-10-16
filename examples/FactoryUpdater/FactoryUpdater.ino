#include "ESP32OTAClient.h"
#include <WiFi.h>
#include <esp_ota_ops.h>

#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"

#define DEVICE_ID "YOUR_DEVICE_ID"
#define JSON_URL "https://your-server/api/update-list?deviceId=" DEVICE_ID
#define DEVICE_KEY "" // project device key; "" when the project has none

// Root CA (PEM) of the OTA server; nullptr skips verification
const char *CA_CERT = nullptr;

#define WIFI_TIMEOUT_MS 20000
#define RETRY_INTERVAL_MS 60000

const esp_partition_t *appPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

bool appBootable(const esp_partition_t *app) {
  if (app == NULL) {
    return false;
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(app, &state) == ESP_OK &&
      (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
    return false;
  }
  esp_app_desc_t desc;
  return esp_ota_get_partition_description(app, &desc) == ESP_OK;
}

void bootApp() {
  const esp_partition_t *app = appPartition();
  if (!appBootable(app) || esp_ota_set_boot_partition(app) != ESP_OK) {
    Serial.println("No bootable app in ota_0, staying in the updater");
    return;
  }
  Serial.println("Booting ota_0...");
  delay(200);
  ESP.restart();
}

bool joinWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void runUpdater() {
  if (!joinWiFi()) {
    Serial.println("Wi-Fi connection failed");
    bootApp();
    return;
  }

  String appVersion = appBootable(appPartition())
                          ? OTAClient::getPartitionVersion("ota_0")
                          : String();
  if (appVersion.isEmpty()) {
    appVersion = "0.0.0";
  }
  Serial.printf("ota_0 app version: %s\n", appVersion.c_str());

  OTAClient ota(JSON_URL, appVersion.c_str());
  ota.setDeviceKey(DEVICE_KEY);
  ota.setCACert(CA_CERT);
  ota.setAutoReboot(false);
  ota.begin();

  if (!ota.hasUpdate()) {
    bootApp();
    return;
  }

  int result = ota.update();
  if (result != OTA_SUCCESS) {
    Serial.printf("Update failed: %d (%s) %s\n", result,
                  OTAClient::errorToString(result),
                  ota.getLastError().c_str());
    bootApp();
    return;
  }

  if (esp_ota_set_boot_partition(appPartition()) != ESP_OK) {
    Serial.println("Could not select ota_0 as boot partition");
    return;
  }
  Serial.println("ota_0 updated, restarting...");
  delay(200);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32 OTA Factory Updater ===");
  runUpdater();
}

void loop() {
  delay(RETRY_INTERVAL_MS);
  runUpdater();
}
