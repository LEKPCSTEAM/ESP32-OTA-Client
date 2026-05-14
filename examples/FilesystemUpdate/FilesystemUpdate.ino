/**
 * ESP32-OTA-Client Example: Firmware + Filesystem (LittleFS) Update
 *
 * Shows how to OTA-update both the application firmware AND the
 * LittleFS / SPIFFS filesystem image in a single call.
 *
 * Server JSON should look like:
 * {
 *   "updater": [
 *     {
 *       "device": "ESP32-S3",
 *       "version": "1.1.0",
 *       "force": false,
 *       "url":             "https://server/firmware-v1.1.0.bin",
 *       "filesystem_url":  "https://server/littlefs-v1.1.0.bin",
 *       "filesystem_version": "1.1.0"
 *     }
 *   ]
 * }
 *
 * Behavior:
 *   - If only "url" is present  → behaves like the legacy firmware-only flow.
 *   - If only "filesystem_url"  → flashes filesystem and reboots.
 *   - If both are present       → flashes filesystem first (no reboot), then
 *                                 firmware (auto-reboots into the new image
 *                                 with the new filesystem already in place).
 *
 * Build the filesystem image with PlatformIO:
 *     pio run --target buildfs
 * The output is at .pio/build/<env>/littlefs.bin — upload that to your server
 * and reference it from "filesystem_url".
 */

#include "ESP32OTAClient.h"
#include <LittleFS.h>
#include <WiFi.h>

const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASS = "YOUR_PASSWORD";

#define PROJECT_ID "06afd666-8542-4b61-bcb7-ace3116902e1"
#define DEVICE_NAME "ESP32-S3"
#define JSON_URL                                                               \
  "http://192.168.1.250:3000/api/update-list?projectId=" PROJECT_ID            \
  "&device=" DEVICE_NAME

#define VERSION "1.0.5"

OTAClient ota(JSON_URL, VERSION);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== ESP32 OTA Firmware + Filesystem Example ===");
  Serial.printf("Current version: %s\n", VERSION);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.printf("LittleFS used: %u / %u bytes\n", LittleFS.usedBytes(),
                  LittleFS.totalBytes());
  }

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (!WiFi.isConnected()) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\nConnected! IP: %s\n\n",
                WiFi.localIP().toString().c_str());

  // Stage-aware progress callback — distinguishes firmware vs filesystem.
  ota.onProgress(
      [](OTAStage stage, int percent, int written, int total) {
        const char *kind =
            (stage == OTA_STAGE_FIRMWARE) ? "FIRMWARE" : "FILESYSTEM";
        Serial.printf("[%s] %d%% (%d/%d bytes)\n", kind, percent, written,
                      total);
      });

  if (ota.hasUpdate()) {
    UpdateInfo info = ota.getUpdateInfo();
    Serial.println("=====================================");
    if (info.available) {
      Serial.printf("  Firmware update:   v%s\n", info.version.c_str());
      Serial.printf("  Firmware file:     %s\n", info.filename.c_str());
    }
    if (info.filesystemAvailable) {
      Serial.printf("  Filesystem update: %s\n",
                    info.filesystemFilename.c_str());
      if (info.filesystemVersion.length() > 0) {
        Serial.printf("  Filesystem ver:    %s\n",
                      info.filesystemVersion.c_str());
      }
    }
    Serial.println("=====================================");

    // Important: the running firmware should stop touching LittleFS before
    // the filesystem image is flashed. Close any open files / unmount.
    if (info.filesystemAvailable) {
      Serial.println("Unmounting LittleFS before filesystem flash...");
      LittleFS.end();
    }

    // Single call handles both stages and reboots when done.
    int result = ota.update();
    if (result < 0) {
      Serial.printf("Update failed: %d\n", result);
      // Re-mount filesystem so the app can keep running
      LittleFS.begin(true);
    }
  } else {
    Serial.println("Already up to date.\n");
  }
}

void loop() { delay(1000); }
