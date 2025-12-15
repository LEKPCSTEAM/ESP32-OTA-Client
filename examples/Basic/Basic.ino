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
#define PROJECT_ID "06afd666-8542-4b61-bcb7-ace3116902e1"
#define DEVICE_NAME "ESP32-S3"
#define JSON_URL "http://192.168.1.250:3000/api/update-list?projectId=" PROJECT_ID "&device=" DEVICE_NAME

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
