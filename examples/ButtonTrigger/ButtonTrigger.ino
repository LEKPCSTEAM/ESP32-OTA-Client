/**
 * ESP32-OTA-Client Example: Button Trigger
 * 
 * This example shows how to check for updates when a button is pressed.
 * Press once to check, press again to confirm update.
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

// Button pin (GPIO0 = BOOT button on most ESP32 boards)
#define BTN_PIN 0

OTAClient ota(JSON_URL, VERSION);
bool updatePending = false;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(BTN_PIN, INPUT_PULLUP);
    
    Serial.println("\n=== ESP32 OTA Button Trigger Example ===");
    Serial.printf("Current version: %s\n", VERSION);
    Serial.println("Press BOOT button to check for updates\n");
    
    // Connect to WiFi
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (!WiFi.isConnected()) {
        Serial.print(".");
        delay(500);
    }
    Serial.printf("\nConnected! IP: %s\n\n", WiFi.localIP().toString().c_str());
    
    // Set progress callback
    ota.onProgress([](int percent, int current, int total) {
        Serial.printf("Downloading: %d%% (%d/%d bytes)\n", percent, current, total);
    });
}

void loop() {
    // Check if button pressed
    if (digitalRead(BTN_PIN) == LOW) {
        delay(50);  // Debounce
        while (digitalRead(BTN_PIN) == LOW) delay(10);  // Wait for release
        
        if (!updatePending) {
            // First press: Check for update
            Serial.println("\nChecking for updates...");
            
            if (ota.hasUpdate()) {
                UpdateInfo info = ota.getUpdateInfo();
                Serial.println("=====================================");
                Serial.printf("  Update available: v%s\n", info.version.c_str());
                Serial.println("  Press BOOT again to install");
                Serial.println("=====================================\n");
                updatePending = true;
            } else {
                Serial.println("Already on the latest version.\n");
            }
        } else {
            // Second press: Confirm and update
            Serial.println("Starting update...\n");
            ota.update();
            // If failed, clear pending flag
            updatePending = false;
        }
    }
    
    // Your application code here
    delay(10);
}
