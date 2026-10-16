/**
 * ESP32-OTA-Client Example: Periodic Check
 * 
 * This example shows how to automatically check for updates periodically.
 * Uses setCheckInterval() and loop() for background checking.
 */

#include "ESP32OTAClient.h"
#include <WiFi.h>

// WiFi credentials
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// OTA Configuration
#define DEVICE_ID "YOUR_DEVICE_ID"
#define JSON_URL "https://your-server/api/update-list?deviceId=" DEVICE_ID
#define DEVICE_KEY ""  // device key from the web manager; "" when the device has none

// Root CA (PEM) of the OTA server; nullptr skips verification
const char* CA_CERT = nullptr;

// Current firmware version
#define VERSION "1.0.5"

// Check interval (5 minutes)
#define CHECK_INTERVAL_MS (5 * 60 * 1000)

OTAClient ota(JSON_URL, VERSION);

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== ESP32 OTA Periodic Check Example ===");
    Serial.printf("Current version: %s\n", VERSION);
    Serial.printf("Auto-check interval: %d minutes\n\n", CHECK_INTERVAL_MS / 60000);
    
    // Connect to WiFi
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (!WiFi.isConnected()) {
        Serial.print(".");
        delay(500);
    }
    Serial.printf("\nConnected! IP: %s\n\n", WiFi.localIP().toString().c_str());

    ota.setDeviceKey(DEVICE_KEY);
    ota.setCACert(CA_CERT);

    // Set progress callback
    ota.onProgress([](int percent, int current, int total) {
        Serial.printf("Update progress: %d%%\n", percent);
    });
    
    // Set auto-check interval
    ota.setCheckInterval(CHECK_INTERVAL_MS);
    
    // Initial check
    Serial.println("Initial update check...");
    ota.checkUpdate();
    
    Serial.println("Application started. Will auto-check for updates.\n");
}

void loop() {
    // Must call ota.loop() for periodic checking to work
    ota.loop();
    
    // Your application code here
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 10000) {
        lastPrint = millis();
        Serial.printf("Running... (uptime: %lu seconds)\n", millis() / 1000);
    }
    
    delay(10);
}
