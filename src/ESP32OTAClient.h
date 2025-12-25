/*
 * ESP32-OTA-Client
 *
 * A lightweight OTA (Over-The-Air) update library for ESP32.
 * Supports JSON-based update server with version checking.
 *
 * Author: LEKPCSTEAM
 * Version: 1.0.3
 * License: MIT
 *
 * GitHub: https://github.com/LEKPCSTEAM/ESP32-OTA-Client
 *
 * Features:
 *   - Check for updates without updating (hasUpdate)
 *   - Update on demand (update)
 *   - Auto-update on check (checkUpdate)
 *   - Rollback to previous firmware version
 *   - Progress callback support
 *   - Periodic auto-check with setCheckInterval
 *   - EEPROM-based duplicate prevention for force updates
 *
 * Server Response Format:
 *   {
 *     "updater": [
 *       {
 *         "device": "ESP32-S3",
 *         "version": "1.0.1",
 *         "force": false,
 *         "url": "http://example.com/firmware-v1.0.1-1766657621922.bin"
 *       }
 *     ]
 *   }
 */

#ifndef ESP32_OTA_CLIENT_H
#define ESP32_OTA_CLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <functional>

// EEPROM
#define OTA_EEPROM_SIZE 128
#define OTA_EEPROM_START_ADDR 0
#define OTA_EEPROM_MAGIC 0xAA55

// Progress callback: (percent, bytesWritten, totalBytes)
typedef std::function<void(int, int, int)> OTAProgressCallback;

/**
 * @brief Update information structure
 */
struct UpdateInfo {
  bool available = false;
  bool force = false;
  String version = "";
  String url = "";
  String filename = "";
};

/**
 * @brief ESP32 OTA Client class
 *
 * Example usage:
 * @code
 * #include "ESP32OTAClient.h"
 *
 * OTAClient ota("http://server/api/update?device=esp32", "1.0.0");
 *
 * void setup() {
 *     WiFi.begin("SSID", "PASSWORD");
 *     while (!WiFi.isConnected()) delay(100);
 *
 *     // Check and update
 *     if (ota.hasUpdate()) {
 *         ota.update();
 *     }
 * }
 * @endcode
 */
class OTAClient {
private:
  String _jsonUrl;
  String _currentVersion;
  unsigned long _lastCheck = 0;
  unsigned long _checkInterval = 0;
  OTAProgressCallback _progressCallback = nullptr;
  bool _eepromInitialized = false;
  String _lastInstalledFilename = "";
  UpdateInfo _updateInfo;

  void log(const char *msg) {
    Serial.print("[OTA] ");
    Serial.println(msg);
  }

  void log(const char *msg, const char *param) {
    Serial.print("[OTA] ");
    Serial.print(msg);
    Serial.println(param);
  }

  /**
   * @brief Extract filename from URL
   * @param url The firmware URL
   * @return Filename extracted from URL
   */
  String extractFilename(const String &url) {
    int lastSlash = url.lastIndexOf('/');
    if (lastSlash >= 0 && lastSlash < (int)url.length() - 1) {
      String filename = url.substring(lastSlash + 1);
      // Remove query parameters if any
      int queryIndex = filename.indexOf('?');
      if (queryIndex > 0) {
        filename = filename.substring(0, queryIndex);
      }
      return filename;
    }
    return "";
  }

  /**
   * @brief Initialize EEPROM and load last installed filename
   */
  void initEEPROM() {
    if (_eepromInitialized)
      return;

    EEPROM.begin(OTA_EEPROM_SIZE);

    // Check magic number
    uint16_t magic;
    EEPROM.get(OTA_EEPROM_START_ADDR, magic);

    if (magic == OTA_EEPROM_MAGIC) {
      // Read stored filename length
      uint8_t len = EEPROM.read(OTA_EEPROM_START_ADDR + 2);
      if (len > 0 && len < OTA_EEPROM_SIZE - 3) {
        char buffer[len + 1];
        for (int i = 0; i < len; i++) {
          buffer[i] = EEPROM.read(OTA_EEPROM_START_ADDR + 3 + i);
        }
        buffer[len] = '\0';
        _lastInstalledFilename = String(buffer);
        log("Last installed firmware: ", _lastInstalledFilename.c_str());
      }
    } else {
      log("EEPROM not initialized, no previous firmware record");
    }

    _eepromInitialized = true;
  }

  /**
   * @brief Save firmware filename to EEPROM
   * @param filename The firmware filename to save
   * @return true on success, false on error
   */
  bool saveFilenameToEEPROM(const String &filename) {
    if (!_eepromInitialized) {
      initEEPROM();
    }

    if (filename.length() >= OTA_EEPROM_SIZE - 3) {
      log("Filename too long to save");
      return false;
    }

    // Write magic number
    EEPROM.put(OTA_EEPROM_START_ADDR, (uint16_t)OTA_EEPROM_MAGIC);

    // Write filename length
    EEPROM.write(OTA_EEPROM_START_ADDR + 2, (uint8_t)filename.length());

    // Write filename
    for (int i = 0; i < (int)filename.length(); i++) {
      EEPROM.write(OTA_EEPROM_START_ADDR + 3 + i, filename.charAt(i));
    }

    if (EEPROM.commit()) {
      _lastInstalledFilename = filename;
      log("Saved firmware filename: ", filename.c_str());
      return true;
    }

    log("Failed to save filename to EEPROM");
    return false;
  }

  /**
   * @brief Follow HTTP redirects and return final response code
   * @param http HTTPClient instance
   * @param url Initial URL to request
   * @param maxRedirects Maximum number of redirects to follow (default 5)
   * @return Final HTTP response code
   */
  int followRedirects(HTTPClient &http, const String &url,
                      int maxRedirects = 5) {
    String currentUrl = url;
    int redirectCount = 0;

    while (redirectCount < maxRedirects) {
      // Determine if URL is HTTPS
      bool isHttps = currentUrl.startsWith("https://");

      if (isHttps) {
        WiFiClientSecure *client = new WiFiClientSecure();
        client->setInsecure(); // Skip certificate validation
        http.begin(*client, currentUrl);
      } else {
        http.begin(currentUrl);
      }

      http.setTimeout(30000);
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

      int httpCode = http.GET();

      // Check if response is a redirect
      if (httpCode == 301 || httpCode == 302 || httpCode == 307 ||
          httpCode == 308) {
        String newUrl = http.getLocation();
        http.end();

        if (newUrl.isEmpty()) {
          log("Redirect without Location header");
          return httpCode;
        }

        log("Following redirect to: ", newUrl.c_str());
        currentUrl = newUrl;
        redirectCount++;
      } else {
        // Not a redirect, return the response code
        return httpCode;
      }
    }

    log("Too many redirects");
    return -1; // Too many redirects
  }

public:
  /**
   * @brief Construct OTA Client
   * @param jsonUrl URL to JSON API endpoint
   * @param version Current firmware version (e.g., "1.0.0")
   */
  OTAClient(const char *jsonUrl, const char *version) {
    _jsonUrl = jsonUrl;
    _currentVersion = version;
    // Note: EEPROM initialization moved to hasUpdate() to ensure Serial is
    // ready
  }

  /**
   * @brief Set progress callback
   * @param callback Function(int percent, int bytesWritten, int totalBytes)
   */
  void onProgress(OTAProgressCallback callback) {
    _progressCallback = callback;
  }

  /**
   * @brief Check if update is available (does NOT download)
   * @return true if update available, false otherwise
   */
  bool hasUpdate() {
    // Initialize EEPROM on first call (after Serial is ready)
    if (!_eepromInitialized) {
      initEEPROM();
    }

    log("Checking for updates...");

    HTTPClient http;
    int httpCode = followRedirects(http, _jsonUrl);

    if (httpCode != 200) {
      log("Server error: ", String(httpCode).c_str());
      http.end();
      return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
      log("Invalid JSON response");
      return false;
    }

    JsonArray configs = doc["updater"].as<JsonArray>();

    for (JsonObject config : configs) {
      String version = config["version"] | "";
      String url = config["url"] | "";
      bool force = config["force"] | false;
      String filename = extractFilename(url);

      // For force update, check if firmware filename is different from last
      // installed
      if (force) {
        if (!filename.isEmpty() && filename == _lastInstalledFilename) {
          log("Force update skipped - same firmware: ", filename.c_str());
          continue;
        }
        log("Force update: ", version.c_str());
        log("New firmware file: ", filename.c_str());
        _updateInfo.available = true;
        _updateInfo.force = true;
        _updateInfo.version = version;
        _updateInfo.url = url;
        _updateInfo.filename = filename;
        return true;
      }

      // Normal version comparison
      if (version > _currentVersion) {
        log("Update available: ", version.c_str());
        _updateInfo.available = true;
        _updateInfo.force = false;
        _updateInfo.version = version;
        _updateInfo.url = url;
        _updateInfo.filename = filename;
        return true;
      }
    }

    log("Already up to date");
    _updateInfo.available = false;
    _updateInfo.force = false;
    return false;
  }

  /**
   * @brief Perform update (call hasUpdate first or use directly)
   * @return 1 on success (will reboot), 0 if no update, negative on error
   */
  int update() {
    if (_updateInfo.available && !_updateInfo.url.isEmpty()) {
      log("Updating to: ", _updateInfo.version.c_str());
      return doUpdate(_updateInfo.url);
    }

    if (hasUpdate()) {
      return doUpdate(_updateInfo.url);
    }

    log("No update available");
    return 0;
  }

  /**
   * @brief Get cached update info (call hasUpdate first)
   * @return UpdateInfo struct with version and URL
   */
  UpdateInfo getUpdateInfo() { return _updateInfo; }

  /**
   * @brief Check for update and install if available
   * @return 1 on success (will reboot), 0 if up to date, negative on error
   */
  int checkUpdate() {
    if (hasUpdate()) {
      return doUpdate(_updateInfo.url);
    }
    return 0;
  }

  /**
   * @brief Force check and update (clears cache first)
   * @return 1 on success (will reboot), 0 if up to date, negative on error
   */
  int forceUpdate() {
    log("Force update check...");
    _updateInfo.available = false;
    return checkUpdate();
  }

  /**
   * @brief Download and install firmware from URL
   * @param url Firmware binary URL
   * @return 1 on success (will reboot), negative on error
   */
  int doUpdate(const String &url) {
    log("Downloading firmware...");

    HTTPClient http;
    int httpCode = followRedirects(http, url);

    if (httpCode != 200) {
      log("Download failed: ", String(httpCode).c_str());
      http.end();
      return -3;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
      log("Invalid content length");
      http.end();
      return -3;
    }

    WiFiClient *stream = http.getStreamPtr();

    if (!Update.begin(contentLength)) {
      log("Not enough space for update");
      http.end();
      return -4;
    }

    log("Installing...");

    int written = 0;
    int lastPercent = -1;
    uint8_t buff[512];

    while (http.connected() && written < contentLength) {
      int available = stream->available();
      if (available > 0) {
        int len = stream->readBytes(buff, min(available, (int)sizeof(buff)));
        Update.write(buff, len);
        written += len;

        int percent = (written * 100) / contentLength;
        if (percent != lastPercent) {
          lastPercent = percent;

          if (_progressCallback) {
            _progressCallback(percent, written, contentLength);
          } else if (percent % 10 == 0) {
            Serial.printf("[OTA] Progress: %d%%\n", percent);
          }
        }
      }
      delay(1);
    }

    http.end();

    if (Update.end(true)) {
      // Save firmware filename to EEPROM before reboot
      if (!_updateInfo.filename.isEmpty()) {
        saveFilenameToEEPROM(_updateInfo.filename);
      } else {
        // Extract filename from URL if not already set
        String filename = extractFilename(url);
        if (!filename.isEmpty()) {
          saveFilenameToEEPROM(filename);
        }
      }

      log("Update complete! Rebooting...");
      delay(500);
      ESP.restart();
      return 1;
    }

    log("Update failed");
    return -5;
  }

  /**
   * @brief Set periodic check interval
   * @param interval Interval in milliseconds (0 to disable)
   */
  void setCheckInterval(unsigned long interval) { _checkInterval = interval; }

  /**
   * @brief Call in loop() for periodic auto-check
   */
  void loop() {
    if (_checkInterval > 0 && millis() - _lastCheck > _checkInterval) {
      _lastCheck = millis();
      checkUpdate();
    }
  }

  /**
   * @brief Check if rollback is possible
   * @return true if can rollback to previous partition, false otherwise
   */
  bool canRollback() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();

    // Get the next update partition (opposite of current)
    const esp_partition_t *next_partition =
        esp_ota_get_next_update_partition(NULL);

    if (next_partition == NULL) {
      return false;
    }

    // Can rollback if the other partition exists and is different from last
    // invalid
    return (next_partition != last_invalid);
  }

  /**
   * @brief Rollback to previous firmware version
   * @return 1 on success (will reboot), 0 if cannot rollback, negative on error
   */
  int rollback() {
    log("Attempting rollback...");

    if (!canRollback()) {
      log("No valid partition to rollback to");
      return 0;
    }

    const esp_partition_t *next_partition =
        esp_ota_get_next_update_partition(NULL);

    if (next_partition == NULL) {
      log("Failed to find rollback partition");
      return -1;
    }

    esp_err_t err = esp_ota_set_boot_partition(next_partition);
    if (err != ESP_OK) {
      log("Failed to set boot partition");
      return -2;
    }

    log("Rollback successful! Rebooting...");
    delay(500);
    ESP.restart();
    return 1;
  }

  /**
   * @brief Mark current firmware as valid (prevent auto-rollback)
   * @return true on success, false on error
   */
  bool markAsValid() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
      if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
          log("Firmware marked as valid");
          return true;
        }
      }
    }
    return false;
  }

  /**
   * @brief Get current boot partition name
   * @return Partition name (e.g., "ota_0", "ota_1")
   */
  String getBootPartition() {
    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (partition != NULL) {
      return String(partition->label);
    }
    return "unknown";
  }

  /**
   * @brief Get partition where next update will be written
   * @return Partition name
   */
  String getNextUpdatePartition() {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition != NULL) {
      return String(partition->label);
    }
    return "unknown";
  }

  /**
   * @brief Get current firmware version
   * @return Version string
   */
  String getVersion() { return _currentVersion; }

  /**
   * @brief Get API URL
   * @return URL string
   */
  String getUrl() { return _jsonUrl; }

  /**
   * @brief Get last installed firmware filename from EEPROM
   * @return Firmware filename string, empty if not set
   */
  String getLastInstalledFilename() { return _lastInstalledFilename; }

  /**
   * @brief Clear the last installed firmware record from EEPROM
   * This allows force update to run again even with the same firmware filename
   * @return true on success, false on error
   */
  bool clearFirmwareRecord() {
    if (!_eepromInitialized) {
      initEEPROM();
    }

    // Clear magic number to invalidate the record
    EEPROM.write(OTA_EEPROM_START_ADDR, 0);
    EEPROM.write(OTA_EEPROM_START_ADDR + 1, 0);

    if (EEPROM.commit()) {
      _lastInstalledFilename = "";
      log("Firmware record cleared from EEPROM");
      return true;
    }

    log("Failed to clear firmware record");
    return false;
  }
};

#endif // ESP32_OTA_CLIENT_H
