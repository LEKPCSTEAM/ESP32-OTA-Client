#include "ESP32OTAClient.h"
#include <WiFi.h>

// WiFi credentials
const char *ssid = "YOUR_SSID";
const char *password = "YOUR_PASSWORD";

// OTA configuration
const char *otaUrl = "http://your-server/api/update?device=esp32";
const char *currentVersion = "1.0.0";

OTAClient ota(otaUrl, currentVersion);

// Self-test flag (set this after your app validates itself)
bool appValidated = false;
unsigned long bootTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== ESP32 OTA Rollback Example ===");
  Serial.print("Firmware Version: ");
  Serial.println(ota.getVersion());
  Serial.print("Boot Partition: ");
  Serial.println(ota.getBootPartition());
  Serial.print("Next Update Partition: ");
  Serial.println(ota.getNextUpdatePartition());

  bootTime = millis();

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  // Perform self-test to verify firmware stability
  performSelfTest();

  // If self-test passed, mark firmware as valid
  if (appValidated) {
    Serial.println("Self-test PASSED - Marking firmware as valid");
    if (ota.markAsValid()) {
      Serial.println("Firmware validated successfully!");
    }
  } else {
    Serial.println("Self-test FAILED - Will rollback in 10 seconds");
    delay(10000);
    Serial.println("Performing rollback...");
    ota.rollback(); // Will reboot to previous firmware
  }

  // Check for updates
  Serial.println("\n--- Checking for updates ---");
  if (ota.hasUpdate()) {
    UpdateInfo info = ota.getUpdateInfo();
    Serial.print("Update available! Version: ");
    Serial.println(info.version);
    Serial.print("Download URL: ");
    Serial.println(info.url);

    // Ask user for confirmation (you could use a button press here)
    Serial.println("Press BOOT button to install update, or wait 30s to skip");

    bool installUpdate = waitForButton(30000); // Wait 30 seconds

    if (installUpdate) {
      Serial.println("Installing update...");

      // Set progress callback
      ota.onProgress([](int percent, int bytesWritten, int totalBytes) {
        Serial.printf("Progress: %d%% (%d/%d bytes)\n", percent, bytesWritten,
                      totalBytes);
      });

      // Perform update (will reboot if successful)
      int result = ota.update();
      if (result == 0) {
        Serial.println("No update performed");
      } else if (result < 0) {
        Serial.printf("Update failed with error: %d\n", result);
      }
    } else {
      Serial.println("Update skipped");
    }
  } else {
    Serial.println("Firmware is up to date!");
  }

  // Show rollback capability
  Serial.print("\nRollback available: ");
  Serial.println(ota.canRollback() ? "YES" : "NO");
}

void loop() {
  // Monitor button for manual rollback trigger
  static unsigned long lastPress = 0;
  static int pressCount = 0;

  if (digitalRead(0) == LOW) {         // BOOT button
    if (millis() - lastPress > 3000) { // Reset counter after 3s
      pressCount = 1;
    } else {
      pressCount++;
    }
    lastPress = millis();

    // Triple press to trigger rollback
    if (pressCount >= 3) {
      Serial.println("\n!!! Manual rollback triggered !!!");
      if (ota.canRollback()) {
        Serial.println("Rolling back to previous firmware...");
        ota.rollback(); // Will reboot
      } else {
        Serial.println("Rollback not available");
      }
      pressCount = 0;
    }

    delay(500); // Debounce
  }

  delay(100);
}

/**
 * Perform self-test to verify firmware stability
 * This should test critical functionality of your application
 */
void performSelfTest() {
  Serial.println("\n--- Performing Self-Test ---");

  bool allTestsPassed = true;

  // Test 1: Check WiFi connectivity
  Serial.print("Test 1: WiFi... ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("PASS");
  } else {
    Serial.println("FAIL");
    allTestsPassed = false;
  }

  // Test 2: Check free heap memory
  Serial.print("Test 2: Memory... ");
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("%d bytes free... ", freeHeap);
  if (freeHeap > 50000) { // At least 50KB free
    Serial.println("PASS");
  } else {
    Serial.println("FAIL");
    allTestsPassed = false;
  }

  // Test 3: Simulate critical sensor check
  Serial.print("Test 3: Sensors... ");
  // Add your sensor initialization/checks here
  // For this example, we'll just pass it
  Serial.println("PASS");

  // Test 4: Configuration validation
  Serial.print("Test 4: Configuration... ");
  // Check if critical configuration is valid
  // For this example, we'll just pass it
  Serial.println("PASS");

  // Set validation flag
  appValidated = allTestsPassed;

  Serial.println("------------------------");
  Serial.print("Overall Result: ");
  Serial.println(allTestsPassed ? "PASS ✓" : "FAIL ✗");
}

/**
 * Wait for BOOT button press with timeout
 * @param timeout Timeout in milliseconds
 * @return true if button pressed, false if timeout
 */
bool waitForButton(unsigned long timeout) {
  unsigned long start = millis();

  while (millis() - start < timeout) {
    if (digitalRead(0) == LOW) { // BOOT button pressed
      delay(50);                 // Debounce
      if (digitalRead(0) == LOW) {
        // Wait for release
        while (digitalRead(0) == LOW) {
          delay(10);
        }
        return true;
      }
    }
    delay(100);
  }

  return false;
}
