/**
 * ESP32-OTA-Client Example: Basic
 * 
 * This example shows the simplest way to use OTA updates.
 * Just checks for updates at startup and updates if available.
 */

#include "ESP32OTAClient.h"
#include <WiFi.h>

// WiFi credentials
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// OTA Configuration
#define DEVICE_ID "YOUR_DEVICE_ID"
#define JSON_URL "https://your-server/api/update-list?deviceId=" DEVICE_ID
#define DEVICE_KEY ""  // project device key; "" when the project has none

// Root CA (PEM) of the OTA server; nullptr skips verification
const char* CA_CERT = nullptr;

// Current firmware version
#define VERSION "1.0.5"

OTAClient ota(JSON_URL, VERSION);

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== ESP32 OTA Basic Example ===");
    Serial.printf("Current version: %s\n\n", VERSION);
    
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

    // Check for updates and install if available
    Serial.println("Checking for updates...");
    int result = ota.checkUpdate();
    
    if (result == 0) {
        Serial.println("No updates available. Starting application...\n");
    }
    // If update found, device will reboot automatically
}

void loop() {
    // Your application code here
    delay(1000);
}
