/*
 * ESP32-OTA-Client
 *
 * A lightweight OTA (Over-The-Air) update library for ESP32.
 * Supports JSON-based update server with version checking.
 * Supports BOTH firmware (app) and filesystem (LittleFS/SPIFFS) updates.
 *
 * Author: LEKPCSTEAM
 * Version: 1.1.0
 * License: MIT
 *
 * GitHub: https://github.com/LEKPCSTEAM/ESP32-OTA-Client
 *
 * Features:
 *   - Check for updates without updating (hasUpdate)
 *   - Update on demand (update)
 *   - Auto-update on check (checkUpdate)
 *   - Rollback to previous firmware version
 *   - Progress callback support (per-stage)
 *   - Periodic auto-check with setCheckInterval
 *   - EEPROM-based duplicate prevention for force updates
 *   - NEW: Filesystem (LittleFS/SPIFFS) OTA via U_SPIFFS partition
 *   - NEW: Combined firmware + filesystem update in a single call
 *
 * Server Response Format:
 *   {
 *     "updater": [
 *       {
 *         "device": "ESP32-S3",
 *         "version": "1.0.1",
 *         "force": false,
 *         "url": "http://example.com/firmware-v1.0.1.bin",
 *
 *         // Optional filesystem image (LittleFS / SPIFFS):
 *         "filesystem_url": "http://example.com/littlefs-v1.0.1.bin",
 *         "filesystem_version": "1.0.1"
 *       }
 *     ]
 *   }
 *
 * Backwards compatibility:
 *   - JSON without filesystem_url behaves identically to v1.0.x
 *   - Existing API signatures unchanged; firmware-only deployments keep working
 *   - EEPROM v1 records are still read; new records use an extended v2 layout
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
// Bumped from 128 to 256 to fit both firmware + filesystem filenames.
// Old (v1.0.x) records inside the first 128 bytes are still read correctly.
#define OTA_EEPROM_SIZE 256
#define OTA_EEPROM_START_ADDR 0
#define OTA_EEPROM_MAGIC 0xAA55     // v1: firmware filename only
#define OTA_EEPROM_MAGIC_V2 0xAA56  // v2: firmware + filesystem filenames

// Update stage identifiers (passed to OTAProgressCallback)
enum OTAStage {
  OTA_STAGE_FIRMWARE = 0,
  OTA_STAGE_FILESYSTEM = 1
};

// Progress callback signatures.
//   Legacy:  (percent, bytesWritten, totalBytes)
//   Stage:   (stage, percent, bytesWritten, totalBytes)
// Both can be registered; the stage-aware one takes precedence when present.
typedef std::function<void(int, int, int)> OTAProgressCallback;
typedef std::function<void(OTAStage, int, int, int)> OTAStageProgressCallback;

/**
 * @brief Update information structure
 */
struct UpdateInfo {
  // Firmware
  bool available = false;
  bool force = false;
  String version = "";
  String url = "";
  String filename = "";

  // Filesystem (LittleFS / SPIFFS) — optional
  bool filesystemAvailable = false;
  String filesystemUrl = "";
  String filesystemFilename = "";
  String filesystemVersion = "";
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
 *     // Single call updates filesystem (if any) then firmware (then reboots)
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
  OTAStageProgressCallback _stageProgressCallback = nullptr;
  bool _eepromInitialized = false;
  String _lastInstalledFilename = "";
  String _lastInstalledFsFilename = "";
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
   */
  String extractFilename(const String &url) {
    int lastSlash = url.lastIndexOf('/');
    if (lastSlash >= 0 && lastSlash < (int)url.length() - 1) {
      String filename = url.substring(lastSlash + 1);
      int queryIndex = filename.indexOf('?');
      if (queryIndex > 0) {
        filename = filename.substring(0, queryIndex);
      }
      return filename;
    }
    return "";
  }

  /**
   * @brief Initialize EEPROM and load last-installed filenames
   *
   * Reads either v1 (firmware only) or v2 (firmware + filesystem) records.
   * v1 records remain valid forever; v2 records are written by this version.
   */
  void initEEPROM() {
    if (_eepromInitialized)
      return;

    EEPROM.begin(OTA_EEPROM_SIZE);

    uint16_t magic;
    EEPROM.get(OTA_EEPROM_START_ADDR, magic);

    if (magic == OTA_EEPROM_MAGIC || magic == OTA_EEPROM_MAGIC_V2) {
      int addr = OTA_EEPROM_START_ADDR + 2;

      // Firmware filename (always present in both v1 and v2)
      uint8_t fwLen = EEPROM.read(addr++);
      if (fwLen > 0 && fwLen < OTA_EEPROM_SIZE - 4) {
        char buffer[fwLen + 1];
        for (int i = 0; i < fwLen; i++) {
          buffer[i] = EEPROM.read(addr++);
        }
        buffer[fwLen] = '\0';
        _lastInstalledFilename = String(buffer);
        log("Last installed firmware: ", _lastInstalledFilename.c_str());
      } else {
        addr += fwLen; // skip past whatever's there
      }

      // Filesystem filename (v2 only)
      if (magic == OTA_EEPROM_MAGIC_V2) {
        uint8_t fsLen = EEPROM.read(addr++);
        if (fsLen > 0 && fsLen < OTA_EEPROM_SIZE - (addr - OTA_EEPROM_START_ADDR)) {
          char buffer[fsLen + 1];
          for (int i = 0; i < fsLen; i++) {
            buffer[i] = EEPROM.read(addr++);
          }
          buffer[fsLen] = '\0';
          _lastInstalledFsFilename = String(buffer);
          log("Last installed filesystem: ", _lastInstalledFsFilename.c_str());
        }
      }
    } else {
      log("EEPROM not initialized, no previous firmware record");
    }

    _eepromInitialized = true;
  }

  /**
   * @brief Persist firmware + filesystem filenames to EEPROM (v2 layout)
   */
  bool saveFilenamesToEEPROM(const String &fwFilename, const String &fsFilename) {
    if (!_eepromInitialized) {
      initEEPROM();
    }

    // Need: 2 (magic) + 1 + fwLen + 1 + fsLen
    if ((int)fwFilename.length() + (int)fsFilename.length() + 4 >= OTA_EEPROM_SIZE) {
      log("Filenames too long to save");
      return false;
    }

    EEPROM.put(OTA_EEPROM_START_ADDR, (uint16_t)OTA_EEPROM_MAGIC_V2);
    int addr = OTA_EEPROM_START_ADDR + 2;

    EEPROM.write(addr++, (uint8_t)fwFilename.length());
    for (int i = 0; i < (int)fwFilename.length(); i++) {
      EEPROM.write(addr++, fwFilename.charAt(i));
    }

    EEPROM.write(addr++, (uint8_t)fsFilename.length());
    for (int i = 0; i < (int)fsFilename.length(); i++) {
      EEPROM.write(addr++, fsFilename.charAt(i));
    }

    if (EEPROM.commit()) {
      _lastInstalledFilename = fwFilename;
      _lastInstalledFsFilename = fsFilename;
      log("Saved firmware filename: ", fwFilename.c_str());
      if (fsFilename.length() > 0) {
        log("Saved filesystem filename: ", fsFilename.c_str());
      }
      return true;
    }

    log("Failed to save filenames to EEPROM");
    return false;
  }

  /**
   * @brief Save only the firmware filename (preserves existing fs filename)
   *        Used by the legacy single-stage code path.
   */
  bool saveFilenameToEEPROM(const String &filename) {
    return saveFilenamesToEEPROM(filename, _lastInstalledFsFilename);
  }

  /**
   * @brief Follow HTTP redirects and return final response code
   */
  int followRedirects(HTTPClient &http, const String &url,
                      int maxRedirects = 5) {
    String currentUrl = url;
    int redirectCount = 0;

    while (redirectCount < maxRedirects) {
      bool isHttps = currentUrl.startsWith("https://");

      if (isHttps) {
        WiFiClientSecure *client = new WiFiClientSecure();
        client->setInsecure();
        http.begin(*client, currentUrl);
      } else {
        http.begin(currentUrl);
      }

      http.setTimeout(30000);
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

      int httpCode = http.GET();

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
        return httpCode;
      }
    }

    log("Too many redirects");
    return -1;
  }

  /**
   * @brief Emit progress to whichever callback the user registered.
   */
  void emitProgress(OTAStage stage, int percent, int written, int total) {
    if (_stageProgressCallback) {
      _stageProgressCallback(stage, percent, written, total);
    } else if (_progressCallback) {
      _progressCallback(percent, written, total);
    } else if (percent % 10 == 0) {
      Serial.printf("[OTA] %s progress: %d%%\n",
                    stage == OTA_STAGE_FIRMWARE ? "Firmware" : "Filesystem",
                    percent);
    }
  }

  /**
   * @brief Shared download + flash routine for either U_FLASH or U_SPIFFS.
   *
   * @param url        URL to download from
   * @param updateType U_FLASH (firmware) or U_SPIFFS (filesystem)
   * @param stage      Stage identifier passed to the progress callback
   * @return 1 on success, negative error code otherwise.
   *         Note: U_FLASH does NOT reboot here — caller decides when to reboot.
   *               (Old behavior was to reboot inside doUpdate — see doUpdate
   *               wrapper which preserves that for the legacy code path.)
   */
  int downloadAndFlash(const String &url, int updateType, OTAStage stage) {
    const char *kind = (updateType == U_FLASH) ? "firmware" : "filesystem";
    log("Downloading ", kind);

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

    if (!Update.begin(contentLength, updateType)) {
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
          emitProgress(stage, percent, written, contentLength);
        }
      }
      delay(1);
    }

    http.end();

    if (!Update.end(true)) {
      log("Update failed");
      return -5;
    }

    return 1;
  }

public:
  /**
   * @brief Construct OTA Client
   */
  OTAClient(const char *jsonUrl, const char *version) {
    _jsonUrl = jsonUrl;
    _currentVersion = version;
  }

  /**
   * @brief Set legacy progress callback (firmware-only signature)
   */
  void onProgress(OTAProgressCallback callback) {
    _progressCallback = callback;
  }

  /**
   * @brief Set stage-aware progress callback.
   *        Receives OTA_STAGE_FIRMWARE or OTA_STAGE_FILESYSTEM as 1st arg.
   *        Takes precedence over onProgress() when both are set.
   */
  void onProgress(OTAStageProgressCallback callback) {
    _stageProgressCallback = callback;
  }

  /**
   * @brief Check if update is available (does NOT download)
   *
   * Populates _updateInfo with both firmware and (optional) filesystem fields.
   * Returns true if EITHER firmware or filesystem update is available.
   */
  bool hasUpdate() {
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

      // Optional filesystem fields (absent in legacy server responses)
      String fsUrl = config["filesystem_url"] | "";
      String fsVersion = config["filesystem_version"] | "";
      String fsFilename = extractFilename(fsUrl);

      bool fwAvailable = false;
      bool fsAvailable = false;

      // --- Firmware decision ---
      if (!version.isEmpty() && version == _currentVersion) {
        // If version matches exactly, we never update (prevents loops)
        log("Firmware version is current: ", version.c_str());
        fwAvailable = false;
      } else if (force) {
        if (!filename.isEmpty() && filename == _lastInstalledFilename) {
          log("Force firmware update skipped - same file: ", filename.c_str());
        } else if (!url.isEmpty()) {
          log("Force firmware update: ", version.c_str());
          fwAvailable = true;
        }
      } else if (!url.isEmpty() && version > _currentVersion) {
        log("Firmware update available: ", version.c_str());
        fwAvailable = true;
      }

      // --- Filesystem decision ---
      // Filesystem is updated whenever the filename differs from the last one
      // we installed. Force flag also forces a filesystem reinstall.
      if (!fsUrl.isEmpty()) {
        if (force) {
          if (fsFilename != _lastInstalledFsFilename) {
            log("Force filesystem update: ", fsFilename.c_str());
            fsAvailable = true;
          } else {
            log("Force filesystem update skipped - same file: ",
                fsFilename.c_str());
          }
        } else if (fsFilename != _lastInstalledFsFilename) {
          log("Filesystem update available: ", fsFilename.c_str());
          fsAvailable = true;
        }
      }

      if (fwAvailable || fsAvailable) {
        _updateInfo.available = fwAvailable;
        _updateInfo.force = force;
        _updateInfo.version = version;
        _updateInfo.url = url;
        _updateInfo.filename = filename;
        _updateInfo.filesystemAvailable = fsAvailable;
        _updateInfo.filesystemUrl = fsUrl;
        _updateInfo.filesystemFilename = fsFilename;
        _updateInfo.filesystemVersion = fsVersion;
        return true;
      }
    }

    log("Already up to date");
    _updateInfo.available = false;
    _updateInfo.force = false;
    _updateInfo.filesystemAvailable = false;
    return false;
  }

  /**
   * @brief Convenience: check if a filesystem update is pending
   *        (call hasUpdate() first).
   */
  bool hasFilesystemUpdate() { return _updateInfo.filesystemAvailable; }

  /**
   * @brief Perform update (call hasUpdate first or use directly).
   *
   * Order of operations when both updates are present:
   *   1. Flash filesystem (no reboot — running firmware keeps executing,
   *      LittleFS should NOT be accessed between flash and reboot).
   *   2. Flash firmware (auto-reboots into new image, which then mounts the
   *      newly-flashed filesystem).
   *
   * Filesystem-only updates trigger a reboot at the end so the new image is
   * mounted cleanly.
   *
   * @return 1 on success (will reboot), 0 if no update, negative on error
   */
  int update() {
    if (!_updateInfo.available && !_updateInfo.filesystemAvailable) {
      if (!hasUpdate()) {
        log("No update available");
        return 0;
      }
    }

    int fsResult = 1;
    if (_updateInfo.filesystemAvailable && !_updateInfo.filesystemUrl.isEmpty()) {
      log("Updating filesystem to: ",
          _updateInfo.filesystemFilename.c_str());
      fsResult = downloadAndFlash(_updateInfo.filesystemUrl, U_SPIFFS,
                                  OTA_STAGE_FILESYSTEM);
      if (fsResult < 0) {
        log("Filesystem update failed; aborting before firmware flash");
        return fsResult;
      }
      // Persist immediately so a power loss between fs and firmware flash
      // doesn't cause us to re-flash the same fs image next boot.
      saveFilenamesToEEPROM(_lastInstalledFilename,
                            _updateInfo.filesystemFilename);
    }

    if (_updateInfo.available && !_updateInfo.url.isEmpty()) {
      log("Updating firmware to: ", _updateInfo.version.c_str());
      return doUpdate(_updateInfo.url);
    }

    // Filesystem-only path — reboot so the freshly flashed image is mounted.
    if (_updateInfo.filesystemAvailable) {
      log("Filesystem-only update complete. Rebooting...");
      delay(500);
      ESP.restart();
      return 1;
    }

    return 0;
  }

  /**
   * @brief Get cached update info (call hasUpdate first)
   */
  UpdateInfo getUpdateInfo() { return _updateInfo; }

  /**
   * @brief Check for update and install if available
   */
  int checkUpdate() {
    if (hasUpdate()) {
      return update();
    }
    return 0;
  }

  /**
   * @brief Force check and update (clears cache first)
   */
  int forceUpdate() {
    log("Force update check...");
    _updateInfo.available = false;
    _updateInfo.filesystemAvailable = false;
    return checkUpdate();
  }

  /**
   * @brief Download and install firmware from URL.
   *
   * NOTE: This is the legacy single-stage entry point. It reboots on success,
   * matching v1.0.x behavior — callers that want the multi-stage flow should
   * use update() instead.
   */
  int doUpdate(const String &url) {
    int result = downloadAndFlash(url, U_FLASH, OTA_STAGE_FIRMWARE);
    if (result < 0) {
      return result;
    }

    String filename = !_updateInfo.filename.isEmpty()
                          ? _updateInfo.filename
                          : extractFilename(url);
    if (!filename.isEmpty()) {
      saveFilenamesToEEPROM(filename, _lastInstalledFsFilename);
    }

    log("Update complete! Rebooting...");
    delay(500);
    ESP.restart();
    return 1;
  }

  /**
   * @brief Download and install a filesystem image (LittleFS / SPIFFS).
   *
   * Does NOT reboot — caller is expected to reboot afterwards if no firmware
   * flash will follow. While this runs, the running firmware should avoid
   * touching the filesystem (open files become invalid mid-flash).
   *
   * @return 1 on success, negative error code on failure.
   */
  int updateFilesystem() {
    if (!_updateInfo.filesystemAvailable ||
        _updateInfo.filesystemUrl.isEmpty()) {
      if (!hasUpdate() || !_updateInfo.filesystemAvailable) {
        log("No filesystem update available");
        return 0;
      }
    }
    return doFilesystemUpdate(_updateInfo.filesystemUrl);
  }

  /**
   * @brief Download and install filesystem image from a specific URL.
   *        Lower-level counterpart to doUpdate(url).
   */
  int doFilesystemUpdate(const String &url) {
    int result = downloadAndFlash(url, U_SPIFFS, OTA_STAGE_FILESYSTEM);
    if (result < 0) {
      return result;
    }

    String filename = !_updateInfo.filesystemFilename.isEmpty()
                          ? _updateInfo.filesystemFilename
                          : extractFilename(url);
    if (!filename.isEmpty()) {
      saveFilenamesToEEPROM(_lastInstalledFilename, filename);
    }

    log("Filesystem update complete!");
    return 1;
  }

  /**
   * @brief Set periodic check interval
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
   */
  bool canRollback() {
    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
    const esp_partition_t *next_partition =
        esp_ota_get_next_update_partition(NULL);

    if (next_partition == NULL) {
      return false;
    }

    return (next_partition != last_invalid);
  }

  /**
   * @brief Rollback to previous firmware version
   *        (Filesystem is not rolled back — there is only one fs partition.)
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
   */
  String getNextUpdatePartition() {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition != NULL) {
      return String(partition->label);
    }
    return "unknown";
  }

  String getVersion() { return _currentVersion; }
  String getUrl() { return _jsonUrl; }
  String getLastInstalledFilename() { return _lastInstalledFilename; }
  String getLastInstalledFsFilename() { return _lastInstalledFsFilename; }

  /**
   * @brief Clear the last-installed firmware + filesystem records.
   *        Allows a force update to re-flash the same files.
   */
  bool clearFirmwareRecord() {
    if (!_eepromInitialized) {
      initEEPROM();
    }

    EEPROM.write(OTA_EEPROM_START_ADDR, 0);
    EEPROM.write(OTA_EEPROM_START_ADDR + 1, 0);

    if (EEPROM.commit()) {
      _lastInstalledFilename = "";
      _lastInstalledFsFilename = "";
      log("Firmware + filesystem records cleared from EEPROM");
      return true;
    }

    log("Failed to clear firmware record");
    return false;
  }
};

#endif // ESP32_OTA_CLIENT_H
