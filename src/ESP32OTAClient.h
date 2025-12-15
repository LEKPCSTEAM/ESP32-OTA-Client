/*
 * ESP32-OTA-Client
 * 
 * A lightweight OTA (Over-The-Air) update library for ESP32.
 * Supports JSON-based update server with version checking.
 * 
 * Author: LEKPCSTEAM
 * Version: 1.0.0
 * License: MIT
 * 
 * GitHub: https://github.com/LEKPCSTEAM/ESP32-OTA-Client
 * 
 * Features:
 *   - Check for updates without updating (hasUpdate)
 *   - Update on demand (update)
 *   - Auto-update on check (checkUpdate)
 *   - Progress callback support
 *   - Periodic auto-check with setCheckInterval
 * 
 * Server Response Format:
 *   {
 *     "updater": [
 *       {
 *         "device": "ESP32-S3",
 *         "version": "1.0.1",
 *         "url": "http://example.com/firmware.bin"
 *       }
 *     ]
 *   }
 */

#ifndef ESP32_OTA_CLIENT_H
#define ESP32_OTA_CLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <functional>

// Progress callback: (percent, bytesWritten, totalBytes)
typedef std::function<void(int, int, int)> OTAProgressCallback;

/**
 * @brief Update information structure
 */
struct UpdateInfo {
    bool available = false;   ///< True if update is available
    String version = "";      ///< New version string
    String url = "";          ///< Firmware download URL
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
class OTAClient
{
private:
    String _jsonUrl;
    String _currentVersion;
    unsigned long _lastCheck = 0;
    unsigned long _checkInterval = 0;
    OTAProgressCallback _progressCallback = nullptr;
    UpdateInfo _updateInfo;

    void log(const char *msg)
    {
        Serial.print("[OTA] ");
        Serial.println(msg);
    }

    void log(const char *msg, const char *param)
    {
        Serial.print("[OTA] ");
        Serial.print(msg);
        Serial.println(param);
    }

public:
    /**
     * @brief Construct OTA Client
     * @param jsonUrl URL to JSON API endpoint
     * @param version Current firmware version (e.g., "1.0.0")
     */
    OTAClient(const char *jsonUrl, const char *version)
    {
        _jsonUrl = jsonUrl;
        _currentVersion = version;
    }

    /**
     * @brief Set progress callback
     * @param callback Function(int percent, int bytesWritten, int totalBytes)
     */
    void onProgress(OTAProgressCallback callback)
    {
        _progressCallback = callback;
    }

    /**
     * @brief Check if update is available (does NOT download)
     * @return true if update available, false otherwise
     */
    bool hasUpdate()
    {
        if (!WiFi.isConnected())
        {
            log("WiFi not connected");
            return false;
        }

        log("Checking for updates...");

        HTTPClient http;
        http.begin(_jsonUrl);
        http.setTimeout(10000);
        int httpCode = http.GET();

        if (httpCode != 200)
        {
            log("Server error: ", String(httpCode).c_str());
            http.end();
            return false;
        }

        String payload = http.getString();
        http.end();

        JsonDocument doc;
        if (deserializeJson(doc, payload))
        {
            log("Invalid JSON response");
            return false;
        }

        JsonArray configs = doc["updater"].as<JsonArray>();

        for (JsonObject config : configs)
        {
            String version = config["version"] | "";
            String url = config["url"] | "";

            if (version > _currentVersion)
            {
                log("Update available: ", version.c_str());
                _updateInfo.available = true;
                _updateInfo.version = version;
                _updateInfo.url = url;
                return true;
            }
        }

        log("Already up to date");
        _updateInfo.available = false;
        return false;
    }

    /**
     * @brief Perform update (call hasUpdate first or use directly)
     * @return 1 on success (will reboot), 0 if no update, negative on error
     */
    int update()
    {
        if (_updateInfo.available && !_updateInfo.url.isEmpty())
        {
            log("Updating to: ", _updateInfo.version.c_str());
            return doUpdate(_updateInfo.url);
        }
        
        if (hasUpdate())
        {
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
    int checkUpdate()
    {
        if (hasUpdate())
        {
            return doUpdate(_updateInfo.url);
        }
        return 0;
    }

    /**
     * @brief Force check and update (clears cache first)
     * @return 1 on success (will reboot), 0 if up to date, negative on error
     */
    int forceUpdate()
    {
        log("Force update check...");
        _updateInfo.available = false;
        return checkUpdate();
    }

    /**
     * @brief Download and install firmware from URL
     * @param url Firmware binary URL
     * @return 1 on success (will reboot), negative on error
     */
    int doUpdate(const String &url)
    {
        log("Downloading firmware...");

        HTTPClient http;
        http.begin(url);
        http.setTimeout(30000);
        int httpCode = http.GET();

        if (httpCode != 200)
        {
            log("Download failed: ", String(httpCode).c_str());
            http.end();
            return -3;
        }

        int contentLength = http.getSize();
        if (contentLength <= 0)
        {
            log("Invalid content length");
            http.end();
            return -3;
        }

        WiFiClient *stream = http.getStreamPtr();

        if (!Update.begin(contentLength))
        {
            log("Not enough space for update");
            http.end();
            return -4;
        }

        log("Installing...");

        int written = 0;
        int lastPercent = -1;
        uint8_t buff[512];

        while (http.connected() && written < contentLength)
        {
            int available = stream->available();
            if (available > 0)
            {
                int len = stream->readBytes(buff, min(available, (int)sizeof(buff)));
                Update.write(buff, len);
                written += len;

                int percent = (written * 100) / contentLength;
                if (percent != lastPercent)
                {
                    lastPercent = percent;

                    if (_progressCallback)
                    {
                        _progressCallback(percent, written, contentLength);
                    }
                    else if (percent % 10 == 0)
                    {
                        Serial.printf("[OTA] Progress: %d%%\n", percent);
                    }
                }
            }
            delay(1);
        }

        http.end();

        if (Update.end(true))
        {
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
    void loop()
    {
        if (_checkInterval > 0 && millis() - _lastCheck > _checkInterval)
        {
            _lastCheck = millis();
            checkUpdate();
        }
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
};

#endif // ESP32_OTA_CLIENT_H
