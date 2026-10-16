/*
 * ESP32-OTA-Client
 *
 * A lightweight OTA (Over-The-Air) update library for ESP32.
 * Supports JSON-based update server with version checking.
 * Supports BOTH firmware (app) and filesystem (LittleFS/SPIFFS) updates.
 *
 * Author: LEKPCSTEAM
 * Version: 1.2.0
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
 *   - Filesystem (LittleFS/SPIFFS) OTA via U_SPIFFS partition
 *   - Combined firmware + filesystem update in a single call
 *   - NEW: Numeric version compare ("1.10.0" > "1.9.0")
 *   - NEW: Size + SHA-256 verification, stall timeout, resumable downloads
 *   - NEW: Device key header, CA certificate, check-in, OTA result reports
 *   - NEW: Rollback-aware state (a rolled-back image is not re-downloaded)
 *   - NEW: Factory-partition updater support (bootToFactory, setAutoReboot)
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
 *         "filesystem_version": "1.0.1",
 *
 *         // Optional integrity fields (absent = check skipped):
 *         "size": 1048576,
 *         "sha256": "<64 lowercase hex>",
 *         "filesystem_size": 1441792,
 *         "filesystem_sha256": "<64 lowercase hex>"
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
#include <Preferences.h>
#include <StreamString.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <functional>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <memory>

// EEPROM
// Bumped from 128 to 256 to fit both firmware + filesystem filenames.
// Old (v1.0.x) records inside the first 128 bytes are still read correctly.
#define OTA_EEPROM_SIZE 256
#define OTA_EEPROM_START_ADDR 0
#define OTA_EEPROM_MAGIC 0xAA55     // v1: firmware filename only
#define OTA_EEPROM_MAGIC_V2 0xAA56  // v2: firmware + filesystem filenames

#define OTA_PREFS_NAMESPACE "otacli"
#define OTA_DEFAULT_TIMEOUT_MS 30000
#define OTA_DEFAULT_DEADLINE_MS 600000
#define OTA_REPORT_TIMEOUT_MS 10000
#define OTA_MAX_RESUME_ATTEMPTS 3
#define OTA_MAX_REDIRECTS 5
#define OTA_MANIFEST_MAX 8192
#define OTA_REPORT_DETAIL_MAX 200
#define OTA_NVS_KEY_MAX 15
#define OTA_MAX_BACKOFF_SHIFT 3 // backoff skips 1, 2, 4, then 8 intervals
#define OTA_SIGNED_MAX_AGE_MS 600000 // refetch a signed manifest older than 10 min
#define OTA_SIGNED_DEADLINE_MS 1200000 // signed URLs expire 1 h after the manifest

// Update stage identifiers (passed to OTAProgressCallback)
enum OTAStage {
  OTA_STAGE_FIRMWARE = 0,
  OTA_STAGE_FILESYSTEM = 1
};

enum OTAResult {
  OTA_SUCCESS = 1,
  OTA_NO_UPDATE = 0,
  OTA_ERR_NO_ROLLBACK_PARTITION = -1,
  OTA_ERR_SET_BOOT = -2,
  OTA_ERR_DOWNLOAD = -3,
  OTA_ERR_NO_SPACE = -4,
  OTA_ERR_UPDATE = -5,
  OTA_ERR_TIMEOUT = -6,
  OTA_ERR_WRITE = -7,
  OTA_ERR_SIZE = -8,
  OTA_ERR_CHECKSUM = -9,
  OTA_ERR_RESUME = -10,
  OTA_ERR_NO_OTA_PARTITION = -11,
  OTA_ERR_NO_FACTORY = -12,
  OTA_ERR_DEADLINE = -13,
  OTA_ERR_STATE = -14
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

  String device = "";
  uint32_t size = 0;  // bytes; 0 = not provided
  String sha256 = "";
  uint32_t filesystemSize = 0;  // bytes; 0 = not provided
  String filesystemSha256 = "";
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
  struct Connection {
    std::unique_ptr<WiFiClient> client;
    HTTPClient http;
  };

  struct PendingFlash {
    String file;
    String version;
    String sha;
  };

  class CappedString : public StreamString {
  public:
    explicit CappedString(size_t cap) : _cap(cap) {}
    size_t write(const uint8_t *data, size_t len) override {
      return length() + len > _cap ? 0 : StreamString::write(data, len);
    }
    size_t write(uint8_t data) override {
      return length() >= _cap ? 0 : StreamString::write(data);
    }

  private:
    size_t _cap;
  };

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

  String _deviceKey = "";
  const char *_caCert = nullptr;
  String _reportUrl = "";
  uint32_t _timeoutMs = OTA_DEFAULT_TIMEOUT_MS;
  uint32_t _deadlineMs = OTA_DEFAULT_DEADLINE_MS;
  bool _checkinSet = false;
  bool _checkinEnabled = false;
  bool _autoReboot = true;
  bool _begun = false;
  bool _awaitingVerify = false;
  bool _insecureWarned = false;
  String _badId = "";
  String _flashedId = "";
  String _lastError = "";
  String _failedManifest = "";
  uint8_t _failStreak = 0;
  uint8_t _skipChecks = 0;
  unsigned long _checkedAt = 0;

  void log(const char *msg) {
    Serial.print("[OTA] ");
    Serial.println(msg);
  }

  void log(const char *msg, const char *param) {
    Serial.print("[OTA] ");
    Serial.print(msg);
    Serial.println(param);
  }

  static String stripQuery(const String &url) {
    int end = (int)url.length();
    int query = url.indexOf('?');
    int fragment = url.indexOf('#');
    if (query >= 0) {
      end = query;
    }
    if (fragment >= 0 && fragment < end) {
      end = fragment;
    }
    return url.substring(0, end);
  }

  /**
   * @brief Extract filename from URL
   */
  static String extractFilename(const String &url) {
    String path = stripQuery(url);
    int lastSlash = path.lastIndexOf('/');
    return lastSlash < 0 ? String() : path.substring(lastSlash + 1);
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

  static int sign(int value) { return value < 0 ? -1 : (value > 0 ? 1 : 0); }

  static bool isNumeric(const String &s) {
    if (s.isEmpty()) {
      return false;
    }
    for (size_t i = 0; i < s.length(); i++) {
      if (!isdigit((unsigned char)s[i])) {
        return false;
      }
    }
    return true;
  }

  static int segmentCount(const String &s) {
    if (s.isEmpty()) {
      return 0;
    }
    int count = 1;
    for (size_t i = 0; i < s.length(); i++) {
      if (s[i] == '.') {
        count++;
      }
    }
    return count;
  }

  static String segmentAt(const String &s, int index) {
    int start = 0;
    for (int i = 0; i < index; i++) {
      int dot = s.indexOf('.', start);
      if (dot < 0) {
        return "";
      }
      start = dot + 1;
    }
    int dot = s.indexOf('.', start);
    return dot < 0 ? s.substring(start) : s.substring(start, dot);
  }

  static int compareNumbers(const String &a, const String &b) {
    unsigned long x = strtoul(a.c_str(), nullptr, 10);
    unsigned long y = strtoul(b.c_str(), nullptr, 10);
    return x < y ? -1 : (x > y ? 1 : 0);
  }

  static String splitVersion(const String &version, String &preRelease) {
    String s = version;
    s.trim();
    if (s.startsWith("v") || s.startsWith("V")) {
      s = s.substring(1);
    }
    int plus = s.indexOf('+');
    if (plus >= 0) {
      s = s.substring(0, plus);
    }
    int dash = s.indexOf('-');
    if (dash < 0) {
      preRelease = "";
      return s;
    }
    preRelease = s.substring(dash + 1);
    return s.substring(0, dash);
  }

  static bool isHex64(const String &s) {
    if (s.length() != 64) {
      return false;
    }
    for (size_t i = 0; i < s.length(); i++) {
      if (!isxdigit((unsigned char)s[i])) {
        return false;
      }
    }
    return true;
  }

  static String toHex(const uint8_t *data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    String out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
      out += digits[data[i] >> 4];
      out += digits[data[i] & 0x0F];
    }
    return out;
  }

  static String urlEncode(const String &s) {
    static const char digits[] = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
      unsigned char c = (unsigned char)s[i];
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        out += (char)c;
      } else {
        out += '%';
        out += digits[c >> 4];
        out += digits[c & 0x0F];
      }
    }
    return out;
  }

  static String urlDecode(const String &s) {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (c == '+') {
        out += ' ';
      } else if (c == '%' && i + 2 < s.length() &&
                 isxdigit((unsigned char)s[i + 1]) &&
                 isxdigit((unsigned char)s[i + 2])) {
        char hex[3] = {s[i + 1], s[i + 2], '\0'};
        out += (char)strtol(hex, nullptr, 16);
        i += 2;
      } else {
        out += c;
      }
    }
    return out;
  }

  static bool queryParam(const String &url, const char *key, String *value) {
    int query = url.indexOf('?');
    if (query < 0) {
      return false;
    }
    String needle = String(key) + "=";
    int pos = query + 1;
    while (pos < (int)url.length()) {
      int amp = url.indexOf('&', pos);
      int end = amp < 0 ? (int)url.length() : amp;
      if (url.startsWith(needle, pos)) {
        if (value != nullptr) {
          *value = urlDecode(url.substring(pos + needle.length(), end));
        }
        return true;
      }
      if (amp < 0) {
        break;
      }
      pos = amp + 1;
    }
    return false;
  }

  static String hostOf(const String &url) {
    int start = url.indexOf("://");
    start = start < 0 ? 0 : start + 3;
    int end = start;
    while (end < (int)url.length() && url[end] != '/' && url[end] != '?' &&
           url[end] != '#') {
      end++;
    }
    String host = url.substring(start, end);
    host.toLowerCase();
    return host;
  }

  static String originOf(const String &url) {
    int sep = url.indexOf("://");
    if (sep <= 0) {
      return String();
    }
    String scheme = url.substring(0, sep);
    scheme.toLowerCase();
    return scheme + "://" + hostOf(url);
  }

  bool keyAllowed(const String &url) const {
    String origin = originOf(url);
    return !_deviceKey.isEmpty() && !origin.isEmpty() &&
           origin == originOf(_jsonUrl);
  }

  static bool isSignedUrl(const String &url) {
    return queryParam(url, "sig", nullptr) && queryParam(url, "exp", nullptr);
  }

  static String imageId(String sha, const String &filename,
                        const String &version) {
    sha.toLowerCase();
    if (isHex64(sha)) {
      return "sha256:" + sha;
    }
    if (filename.isEmpty() && version.isEmpty()) {
      return String();
    }
    return filename + "@" + version;
  }

  static String resolveLocation(const String &base, const String &location) {
    if (!location.startsWith("/")) {
      return location;
    }
    int scheme = base.indexOf("://");
    int pathStart = base.indexOf('/', scheme < 0 ? 0 : scheme + 3);
    return (pathStart < 0 ? base : base.substring(0, pathStart)) + location;
  }

  static void shaStart(mbedtls_sha256_context *ctx) {
    mbedtls_sha256_init(ctx);
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_starts(ctx, 0);
#else
    mbedtls_sha256_starts_ret(ctx, 0);
#endif
  }

  static void shaUpdate(mbedtls_sha256_context *ctx, const uint8_t *data,
                        size_t len) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_update(ctx, data, len);
#else
    mbedtls_sha256_update_ret(ctx, data, len);
#endif
  }

  static String shaFinish(mbedtls_sha256_context *ctx) {
    uint8_t digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_finish(ctx, digest);
#else
    mbedtls_sha256_finish_ret(ctx, digest);
#endif
    mbedtls_sha256_free(ctx);
    return toHex(digest, sizeof(digest));
  }

  static String readKey(Preferences &prefs, const char *key) {
    return prefs.isKey(key) ? prefs.getString(key) : String();
  }

  static void removeKey(Preferences &prefs, const char *key) {
    if (prefs.isKey(key)) {
      prefs.remove(key);
    }
  }

  static bool writeKey(Preferences &prefs, const char *key,
                       const String &value) {
    if (value.isEmpty()) {
      return !prefs.isKey(key) || prefs.remove(key);
    }
    return prefs.putString(key, value) == value.length();
  }

  static String versionKey(const char *partitionLabel) {
    return (String("v_") + partitionLabel).substring(0, OTA_NVS_KEY_MAX);
  }

  static bool forgetSlotVersion() {
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    Preferences prefs;
    return target != NULL && prefs.begin(OTA_PREFS_NAMESPACE, false) &&
           writeKey(prefs, versionKey(target->label).c_str(), String());
  }

  static bool isOtaSlot(const esp_partition_t *partition) {
    return partition != NULL &&
           partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
  }

  static bool hasOtaPartition() {
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    return next != NULL && (running == NULL || next->address != running->address);
  }

  // HTTPClient::setTimeout() takes uint16_t ms
  uint16_t httpTimeout() const {
    uint32_t ms = _timeoutMs < 1000 ? 1000 : _timeoutMs;
    return (uint16_t)(ms > 60000 ? 60000 : ms);
  }

  int fail(int code, const String &detail) {
    _lastError = detail;
    log("Error: ", detail.c_str());
    return code;
  }

  void attachTransport(Connection &conn, const String &url) {
    if (url.startsWith("https://")) {
      WiFiClientSecure *secure = new WiFiClientSecure();
      if (_caCert != nullptr) {
        secure->setCACert(_caCert);
      } else {
        if (!_insecureWarned) {
          log("WARNING: no CA certificate set (setCACert), HTTPS server is "
              "not verified");
          _insecureWarned = true;
        }
        secure->setInsecure();
      }
      conn.client.reset(secure);
    } else {
      conn.client.reset(new WiFiClient());
    }
  }

  /**
   * @brief Follow HTTP redirects and return final response code
   */
  int followRedirects(Connection &conn, const String &url, int rangeStart = -1,
                      bool http10 = false) {
    static const char *collect[] = {"Content-Range", "Transfer-Encoding"};
    String currentUrl = url;
    int redirectCount = 0;

    while (redirectCount < OTA_MAX_REDIRECTS) {
      attachTransport(conn, currentUrl);
      if (!conn.http.begin(*conn.client, currentUrl)) {
        log("Invalid URL: ", currentUrl.c_str());
        return HTTPC_ERROR_CONNECTION_REFUSED;
      }

      conn.http.collectHeaders(collect, 2);
      conn.http.useHTTP10(http10);
      conn.http.setReuse(false);
      conn.http.setTimeout(httpTimeout());
      conn.http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      if (http10) {
        conn.http.addHeader("Accept-Encoding", "identity");
      }
      if (keyAllowed(currentUrl)) {
        conn.http.addHeader("X-Device-Key", _deviceKey);
      }
      if (rangeStart > 0) {
        conn.http.addHeader("Range", "bytes=" + String(rangeStart) + "-");
      }

      int httpCode = conn.http.GET();

      if (httpCode == 301 || httpCode == 302 || httpCode == 303 ||
          httpCode == 307 || httpCode == 308) {
        String newUrl = conn.http.getLocation();
        conn.http.end();

        if (newUrl.isEmpty()) {
          log("Redirect without Location header");
          return httpCode;
        }

        currentUrl = resolveLocation(currentUrl, newUrl);
        log("Following redirect to: ", currentUrl.c_str());
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

  int streamBody(Connection &conn, int total, int &written, OTAStage stage,
                 mbedtls_sha256_context *sha, int &lastPercent,
                 unsigned long started, uint32_t deadlineMs) {
    WiFiClient *stream = conn.http.getStreamPtr();
    if (stream == nullptr) {
      return OTA_ERR_DOWNLOAD;
    }

    uint8_t buff[512];
    unsigned long lastData = millis();

    while (written < total) {
      if (millis() - started > deadlineMs) {
        _lastError = "download took longer than " +
                     String(deadlineMs / 1000) + " s";
        return OTA_ERR_DEADLINE;
      }
      size_t available = stream->available();
      if (available > 0) {
        size_t want = available < sizeof(buff) ? available : sizeof(buff);
        if (want > (size_t)(total - written)) {
          want = (size_t)(total - written);
        }
        int len = stream->readBytes(buff, want);
        if (len > 0) {
          if (Update.write(buff, len) != (size_t)len) {
            _lastError = String("flash write failed: ") + Update.errorString();
            return OTA_ERR_WRITE;
          }
          shaUpdate(sha, buff, len);
          written += len;
          lastData = millis();

          int percent = (int)((int64_t)written * 100 / total);
          if (percent != lastPercent) {
            lastPercent = percent;
            emitProgress(stage, percent, written, total);
          }
          continue;
        }
      } else if (!conn.http.connected()) {
        return OTA_ERR_DOWNLOAD;
      }

      if (millis() - lastData > _timeoutMs) {
        return OTA_ERR_TIMEOUT;
      }
      delay(1);
    }

    return OTA_SUCCESS;
  }

  static bool resumeAccepted(Connection &conn, int httpCode, int offset,
                             int total) {
    if (httpCode != 206) {
      return false;
    }
    String range = conn.http.header("Content-Range");
    range.trim();
    int dash = range.indexOf('-');
    int slash = range.indexOf('/');
    if (!range.startsWith("bytes ") || dash < 0 || slash < dash) {
      return false;
    }
    String start = range.substring(6, dash);
    start.trim();
    if (!isNumeric(start) || start.toInt() != offset) {
      return false;
    }
    String length = range.substring(slash + 1);
    if (length != "*" && length.toInt() != total) {
      return false;
    }
    return conn.http.getSize() == total - offset;
  }

  static bool isChunked(Connection &conn) {
    return conn.http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
  }

  static int bodyLength(Connection &conn, uint32_t expectedSize) {
    if (isChunked(conn)) {
      return -1;
    }
    int size = conn.http.getSize();
    if (size > 0) {
      return size;
    }
    int fallback = (int)expectedSize;
    return fallback > 0 ? fallback : -1;
  }

  int flashImage(const String &url, int updateType, OTAStage stage,
                 uint32_t expectedSize, String expectedSha,
                 const PendingFlash *pending) {
    const char *kind = (updateType == U_FLASH) ? "firmware" : "filesystem";
    _lastError = "";
    unsigned long started = millis();
    uint32_t deadlineMs = _deadlineMs;
    if (isSignedUrl(url) && deadlineMs > OTA_SIGNED_DEADLINE_MS) {
      deadlineMs = OTA_SIGNED_DEADLINE_MS;
    }

    if (updateType == U_FLASH && !hasOtaPartition()) {
      return fail(OTA_ERR_NO_OTA_PARTITION,
                  "no OTA partition to write firmware (use bootToFactory)");
    }
    expectedSha.toLowerCase();
    if (!expectedSha.isEmpty() && !isHex64(expectedSha)) {
      return fail(OTA_ERR_CHECKSUM, "manifest sha256 is not 64 hex chars");
    }

    log("Downloading ", kind);

    Connection conn;
    bool http10 = false;
    int httpCode = followRedirects(conn, url);
    if (httpCode == 200 && isChunked(conn)) {
      conn.http.end();
      http10 = true;
      httpCode = followRedirects(conn, url, -1, http10);
    }
    if (httpCode != 200) {
      return fail(OTA_ERR_DOWNLOAD, "download HTTP " + String(httpCode));
    }

    int contentLength = bodyLength(conn, expectedSize);
    if (contentLength <= 0) {
      return fail(OTA_ERR_DOWNLOAD,
                  "no Content-Length and no size in the manifest");
    }
    if (expectedSize > 0 && (uint32_t)contentLength != expectedSize) {
      return fail(OTA_ERR_SIZE, "Content-Length " + String(contentLength) +
                                    " != manifest size " +
                                    String(expectedSize));
    }

    if (!Update.begin(contentLength, updateType)) {
      log("Not enough space for update");
      return fail(OTA_ERR_NO_SPACE, Update.errorString());
    }
    if (updateType == U_FLASH && !forgetSlotVersion()) {
      Update.abort();
      return fail(OTA_ERR_STATE, "could not clear the target slot version");
    }

    log("Installing...");

    mbedtls_sha256_context sha;
    shaStart(&sha);
    int written = 0;
    int lastPercent = -1;
    int resumes = 0;
    int result = OTA_SUCCESS;

    while (true) {
      result = streamBody(conn, contentLength, written, stage, &sha,
                          lastPercent, started, deadlineMs);
      if (result != OTA_ERR_DOWNLOAD && result != OTA_ERR_TIMEOUT) {
        break;
      }
      conn.http.end();
      _lastError = String(result == OTA_ERR_TIMEOUT ? "stalled" : "connection lost") +
                   " at " + String(written) + "/" + String(contentLength) +
                   " bytes";
      if (resumes >= OTA_MAX_RESUME_ATTEMPTS) {
        break;
      }
      resumes++;
      log("Resuming download: ", _lastError.c_str());
      httpCode = followRedirects(conn, url, written, http10);
      bool accepted =
          written == 0
              ? httpCode == 200 &&
                    bodyLength(conn, expectedSize) == contentLength
              : resumeAccepted(conn, httpCode, written, contentLength);
      if (!accepted) {
        result = OTA_ERR_RESUME;
        _lastError = "resume at " + String(written) + " rejected (HTTP " +
                     String(httpCode) + ")";
        break;
      }
    }
    conn.http.end();

    String digest = shaFinish(&sha);

    if (result == OTA_SUCCESS && written != contentLength) {
      result = OTA_ERR_SIZE;
      _lastError = "wrote " + String(written) + " of " + String(contentLength) +
                   " bytes";
    }
    if (result == OTA_SUCCESS && !expectedSha.isEmpty() && digest != expectedSha) {
      result = OTA_ERR_CHECKSUM;
      _lastError = "sha256 mismatch, got " + digest;
    }
    if (result != OTA_SUCCESS) {
      Update.abort();
      return fail(result, _lastError);
    }

    if (pending != nullptr && !savePending(*pending)) {
      Update.abort();
      return fail(OTA_ERR_STATE, "could not save the pending OTA record");
    }

    if (!Update.end()) {
      String reason = Update.errorString();
      if (Update.isRunning()) {
        Update.abort();
      }
      if (pending != nullptr) {
        dropPending();
      }
      log("Update failed");
      return fail(OTA_ERR_UPDATE, reason);
    }

    return OTA_SUCCESS;
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
  int downloadAndFlash(const String &url, int updateType, OTAStage stage,
                       uint32_t expectedSize = 0,
                       const String &expectedSha = String(),
                       const String &toVersion = String(),
                       const PendingFlash *pending = nullptr) {
    int result = flashImage(url, updateType, stage, expectedSize, expectedSha,
                            pending);
    if (result < 0) {
      reportFailure(stage, result, toVersion);
    }
    return result;
  }

  static bool reportTarget(const String &manifestUrl, const String &reportUrl,
                           String &url, String &deviceId, String &projectId,
                           String &device) {
    url = reportUrl;
    int api = manifestUrl.indexOf("/api/update-list");
    if (url.isEmpty() && api >= 0) {
      url = manifestUrl.substring(0, api) + "/api/ota-report";
    }
    queryParam(manifestUrl, "deviceId", &deviceId);
    if (deviceId.isEmpty()) {
      queryParam(manifestUrl, "projectId", &projectId);
      queryParam(manifestUrl, "device", &device);
    }
    return !url.isEmpty();
  }

  bool postReport(const char *result, const String &from, const String &to,
                  const String &detail) {
    String url, deviceId, projectId, device;
    if (!reportTarget(_jsonUrl, _reportUrl, url, deviceId, projectId, device)) {
      return true;
    }
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }

    JsonDocument doc;
    if (!deviceId.isEmpty()) {
      doc["deviceId"] = deviceId;
    } else {
      doc["projectId"] = projectId;
      doc["device"] = device.isEmpty() ? _updateInfo.device : device;
    }
    doc["mac"] = WiFi.macAddress();
    doc["from"] = from;
    doc["to"] = to;
    doc["result"] = result;
    if (!detail.isEmpty()) {
      doc["detail"] = detail.substring(0, OTA_REPORT_DETAIL_MAX);
    }
    String body;
    serializeJson(doc, body);

    Connection conn;
    attachTransport(conn, url);
    if (!conn.http.begin(*conn.client, url)) {
      log("Invalid report URL: ", url.c_str());
      return true;
    }
    conn.http.setReuse(false);
    conn.http.setTimeout(min(httpTimeout(), (uint16_t)OTA_REPORT_TIMEOUT_MS));
    conn.http.addHeader("Content-Type", "application/json");
    if (keyAllowed(url)) {
      conn.http.addHeader("X-Device-Key", _deviceKey);
    }
    int httpCode = conn.http.POST(body);
    conn.http.end();

    Serial.printf("[OTA] Report '%s' -> HTTP %d\n", result, httpCode);
    if (httpCode >= 200 && httpCode < 300) {
      return true;
    }
    return httpCode >= 400 && httpCode < 500 && httpCode != 408 && httpCode != 429;
  }

  void reportFailure(OTAStage stage, int code, const String &toVersion) {
    String detail = String(stage == OTA_STAGE_FIRMWARE ? "firmware: " : "filesystem: ") +
                    errorToString(code);
    if (!_lastError.isEmpty()) {
      detail += " - " + _lastError;
    }
    postReport("failed", _currentVersion, toVersion, detail);
  }

  void queueReport(Preferences &prefs, const char *result, const String &from,
                   const String &to, const String &detail) {
    prefs.putString("q_res", result);
    prefs.putString("q_from", from);
    prefs.putString("q_to", to);
    prefs.putString("q_det", detail);
    log("Queued OTA report: ", result);
  }

  static void clearPending(Preferences &prefs) {
    removeKey(prefs, "p_part");
    removeKey(prefs, "p_file");
    removeKey(prefs, "p_ver");
    removeKey(prefs, "p_from");
    removeKey(prefs, "p_sha");
  }

  static void dropPending() {
    Preferences prefs;
    if (prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      clearPending(prefs);
    }
  }

  bool savePending(const PendingFlash &pending) {
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    Preferences prefs;
    if (target == NULL || !prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      return false;
    }
    bool saved = writeKey(prefs, "p_part", String()) &&
                 writeKey(prefs, "p_file", pending.file) &&
                 writeKey(prefs, "p_ver", pending.version) &&
                 writeKey(prefs, "p_from", _currentVersion) &&
                 writeKey(prefs, "p_sha", pending.sha) &&
                 writeKey(prefs, "p_part", target->label);
    if (!saved) {
      clearPending(prefs);
      return false;
    }
    _awaitingVerify = false;
    return true;
  }

  void confirmVerified() {
    if (!_awaitingVerify) {
      return;
    }
    Preferences prefs;
    if (!prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      return;
    }
    queueReport(prefs, "ok", readKey(prefs, "p_from"), readKey(prefs, "p_ver"), "");
    clearPending(prefs);
    _awaitingVerify = false;
  }

  void dropStaleSignedInfo() {
    bool isSigned =
        (_updateInfo.available && isSignedUrl(_updateInfo.url)) ||
        (_updateInfo.filesystemAvailable &&
         isSignedUrl(_updateInfo.filesystemUrl));
    if (isSigned && millis() - _checkedAt > OTA_SIGNED_MAX_AGE_MS) {
      log("Signed download URL is old, checking again");
      _updateInfo.available = false;
      _updateInfo.filesystemAvailable = false;
    }
  }

  String manifestUrl() {
    bool enabled = _checkinSet ? _checkinEnabled
                               : _jsonUrl.indexOf("/api/update-list") >= 0;
    if (!enabled || queryParam(_jsonUrl, "mac", nullptr)) {
      return _jsonUrl;
    }
    String url = _jsonUrl;
    char last = url.isEmpty() ? '\0' : url[url.length() - 1];
    if (last != '?' && last != '&') {
      url += url.indexOf('?') >= 0 ? '&' : '?';
    }
    url += "mac=" + urlEncode(WiFi.macAddress()) +
           "&ver=" + urlEncode(_currentVersion);
    return url;
  }

public:
  /**
   * @brief Construct OTA Client
   */
  OTAClient(const char *jsonUrl, const char *version) {
    _jsonUrl = jsonUrl;
    _currentVersion = version;
  }

  void begin() {
    if (_begun) {
      return;
    }
    _begun = true;
    initEEPROM();

    Preferences prefs;
    if (!prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      log("Preferences unavailable, rollback tracking disabled");
      return;
    }
    _badId = readKey(prefs, "bad_id");
    if (!_badId.isEmpty()) {
      log("Rolled-back firmware on record: ", _badId.c_str());
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    String runningLabel = running != NULL ? String(running->label) : String();

    if (isOtaSlot(running)) {
      String key = versionKey(running->label);
      if (readKey(prefs, key.c_str()) != _currentVersion) {
        prefs.putString(key.c_str(), _currentVersion);
      }
    }

    if (!prefs.isKey("p_part")) {
      return;
    }
    String target = readKey(prefs, "p_part");
    String from = readKey(prefs, "p_from");
    String to = readKey(prefs, "p_ver");
    const esp_partition_t *targetPartition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, target.c_str());
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (targetPartition == NULL ||
        esp_ota_get_state_partition(targetPartition, &state) != ESP_OK) {
      state = ESP_OTA_IMG_UNDEFINED;
    }

    if (targetPartition == NULL) {
      log("Pending OTA target partition not found: ", target.c_str());
    } else if (runningLabel == target) {
      if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        _awaitingVerify = true;
        log("New firmware is pending verification; call markAsValid()");
        return;
      }
      queueReport(prefs, "ok", from, to, "");
    } else if (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED) {
      String id = imageId(readKey(prefs, "p_sha"), readKey(prefs, "p_file"), to);
      log("Firmware rolled back: ", id.c_str());
      if (!id.isEmpty()) {
        _badId = id;
      }
      bool saved = id.isEmpty() || writeKey(prefs, "bad_id", id);
      queueReport(prefs, "rollback", from, to,
                  "running " + runningLabel + ", expected " + target);
      if (!saved) {
        log("Could not save the rolled-back firmware id; keeping the record");
        return;
      }
    } else if (state == ESP_OTA_IMG_VALID) {
      queueReport(prefs, "ok", from, to, "");
    } else {
      log("Result of the last firmware flash not known yet: ", target.c_str());
      return;
    }
    clearPending(prefs);
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

  void setDeviceKey(const char *key) { _deviceKey = key != nullptr ? key : ""; }

  void setCACert(const char *pem) { _caCert = pem; }

  void setTimeout(uint32_t ms) { _timeoutMs = ms > 0 ? ms : OTA_DEFAULT_TIMEOUT_MS; }

  void setDownloadDeadline(uint32_t ms) {
    _deadlineMs = ms > 0 ? ms : OTA_DEFAULT_DEADLINE_MS;
  }

  void setCheckin(bool enabled) {
    _checkinSet = true;
    _checkinEnabled = enabled;
  }

  void setReportUrl(const char *url) { _reportUrl = url != nullptr ? url : ""; }

  void setAutoReboot(bool enabled) { _autoReboot = enabled; }

  static int compareVersions(const String &a, const String &b) {
    String preA, preB;
    String coreA = splitVersion(a, preA);
    String coreB = splitVersion(b, preB);

    int count = max(segmentCount(coreA), segmentCount(coreB));
    for (int i = 0; i < count; i++) {
      String segA = segmentAt(coreA, i);
      String segB = segmentAt(coreB, i);
      if (segA.isEmpty()) {
        segA = "0";
      }
      if (segB.isEmpty()) {
        segB = "0";
      }
      if (!isNumeric(segA) || !isNumeric(segB)) {
        return sign(strcmp(a.c_str(), b.c_str()));
      }
      int cmp = compareNumbers(segA, segB);
      if (cmp != 0) {
        return cmp;
      }
    }

    if (preA.isEmpty() || preB.isEmpty()) {
      if (preA.isEmpty() == preB.isEmpty()) {
        return 0;
      }
      return preA.isEmpty() ? 1 : -1;
    }

    int countA = segmentCount(preA);
    int countB = segmentCount(preB);
    for (int i = 0; i < max(countA, countB); i++) {
      if (i >= countA || i >= countB) {
        return i >= countA ? -1 : 1;
      }
      String segA = segmentAt(preA, i);
      String segB = segmentAt(preB, i);
      int cmp = (isNumeric(segA) && isNumeric(segB))
                    ? compareNumbers(segA, segB)
                    : sign(strcmp(segA.c_str(), segB.c_str()));
      if (cmp != 0) {
        return cmp;
      }
    }
    return 0;
  }

  static const char *errorToString(int code) {
    switch (code) {
    case OTA_SUCCESS:
      return "success";
    case OTA_NO_UPDATE:
      return "no update";
    case OTA_ERR_NO_ROLLBACK_PARTITION:
      return "rollback partition not found";
    case OTA_ERR_SET_BOOT:
      return "failed to set boot partition";
    case OTA_ERR_DOWNLOAD:
      return "download failed";
    case OTA_ERR_NO_SPACE:
      return "not enough space";
    case OTA_ERR_UPDATE:
      return "update failed";
    case OTA_ERR_TIMEOUT:
      return "download stalled";
    case OTA_ERR_WRITE:
      return "flash write failed";
    case OTA_ERR_SIZE:
      return "size mismatch";
    case OTA_ERR_CHECKSUM:
      return "sha256 mismatch";
    case OTA_ERR_RESUME:
      return "resume rejected";
    case OTA_ERR_NO_OTA_PARTITION:
      return "no OTA partition to write";
    case OTA_ERR_NO_FACTORY:
      return "no factory partition";
    case OTA_ERR_DEADLINE:
      return "download deadline exceeded";
    case OTA_ERR_STATE:
      return "could not save OTA state";
    default:
      return "unknown error";
    }
  }

  String getLastError() { return _lastError; }

  static String getPartitionVersion(const char *partitionLabel) {
    Preferences prefs;
    if (partitionLabel == nullptr || !prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      return "";
    }
    return readKey(prefs, versionKey(partitionLabel).c_str());
  }

  bool sendPendingReport() {
    begin();
    Preferences prefs;
    if (!prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      return false;
    }
    if (!prefs.isKey("q_res")) {
      return true;
    }
    String result = readKey(prefs, "q_res");
    if (!postReport(result.c_str(), readKey(prefs, "q_from"),
                    readKey(prefs, "q_to"), readKey(prefs, "q_det"))) {
      return false;
    }
    removeKey(prefs, "q_res");
    removeKey(prefs, "q_from");
    removeKey(prefs, "q_to");
    removeKey(prefs, "q_det");
    return true;
  }

  /**
   * @brief Check if update is available (does NOT download)
   *
   * Populates _updateInfo with both firmware and (optional) filesystem fields.
   * Returns true if EITHER firmware or filesystem update is available.
   */
  bool hasUpdate() {
    begin();
    sendPendingReport();

    log("Checking for updates...");

    Connection conn;
    int httpCode = followRedirects(conn, manifestUrl());

    if (httpCode != 200) {
      log("Server error: ", String(httpCode).c_str());
      conn.http.end();
      return false;
    }

    CappedString payload(OTA_MANIFEST_MAX);
    int read = conn.http.getSize() > OTA_MANIFEST_MAX
                   ? -1
                   : conn.http.writeToStream(&payload);
    conn.http.end();
    if (read < 0) {
      Serial.printf("[OTA] Manifest unreadable or over %d bytes\n",
                    OTA_MANIFEST_MAX);
      return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload.c_str(), payload.length())) {
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
      String sha = config["sha256"] | "";
      String id = imageId(sha, filename, version);

      if (!id.isEmpty() && id == _badId) {
        log("Skipping rolled-back firmware: ", id.c_str());
        continue;
      }
      if (!id.isEmpty() && id == _flashedId) {
        log("Firmware already flashed, restart pending: ", id.c_str());
        continue;
      }

      bool fwAvailable = false;
      bool fsAvailable = false;

      // --- Firmware decision ---
      if (!version.isEmpty() && compareVersions(version, _currentVersion) == 0) {
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
      } else if (!url.isEmpty() && compareVersions(version, _currentVersion) > 0) {
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
        _updateInfo.device = config["device"] | "";
        _updateInfo.size = config["size"].as<uint32_t>();
        _updateInfo.sha256 = sha;
        _updateInfo.filesystemSize = config["filesystem_size"].as<uint32_t>();
        _updateInfo.filesystemSha256 = config["filesystem_sha256"] | "";
        _checkedAt = millis();
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
   * @return 1 on success (reboots unless setAutoReboot(false)), 0 if no
   *         update, negative on error
   */
  int update() {
    begin();
    dropStaleSignedInfo();
    if (!_updateInfo.available && !_updateInfo.filesystemAvailable) {
      if (!hasUpdate()) {
        log("No update available");
        return 0;
      }
    }

    if (_updateInfo.available && !_updateInfo.url.isEmpty() &&
        !hasOtaPartition()) {
      _lastError = "no OTA partition to write firmware (use bootToFactory)";
      log("Error: ", _lastError.c_str());
      reportFailure(OTA_STAGE_FIRMWARE, OTA_ERR_NO_OTA_PARTITION,
                    _updateInfo.version);
      return OTA_ERR_NO_OTA_PARTITION;
    }

    bool fsFlashed = false;
    if (_updateInfo.filesystemAvailable && !_updateInfo.filesystemUrl.isEmpty()) {
      log("Updating filesystem to: ",
          _updateInfo.filesystemFilename.c_str());
      int fsResult = downloadAndFlash(
          _updateInfo.filesystemUrl, U_SPIFFS, OTA_STAGE_FILESYSTEM,
          _updateInfo.filesystemSize, _updateInfo.filesystemSha256,
          _updateInfo.version.isEmpty() ? _updateInfo.filesystemVersion
                                        : _updateInfo.version);
      if (fsResult < 0) {
        log("Filesystem update failed; aborting before firmware flash");
        return fsResult;
      }
      // Persist immediately so a power loss between fs and firmware flash
      // doesn't cause us to re-flash the same fs image next boot.
      saveFilenamesToEEPROM(_lastInstalledFilename,
                            _updateInfo.filesystemFilename);
      _updateInfo.filesystemAvailable = false;
      fsFlashed = true;
    }

    if (_updateInfo.available && !_updateInfo.url.isEmpty()) {
      log("Updating firmware to: ", _updateInfo.version.c_str());
      return doUpdate(_updateInfo.url);
    }

    // Filesystem-only path — reboot so the freshly flashed image is mounted.
    if (fsFlashed) {
      if (!_autoReboot) {
        log("Filesystem-only update complete. Restart to mount it");
        return 1;
      }
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
   * NOTE: This is the legacy single-stage entry point. It reboots on success
   * (unless setAutoReboot(false)), matching v1.0.x behavior — callers that
   * want the multi-stage flow should use update() instead.
   */
  int doUpdate(const String &url) {
    begin();
    bool fromManifest = (url == _updateInfo.url);
    PendingFlash pending;
    pending.file = fromManifest ? _updateInfo.filename : extractFilename(url);
    pending.version = fromManifest ? _updateInfo.version : String();
    pending.sha = fromManifest ? _updateInfo.sha256 : String();
    int result = downloadAndFlash(url, U_FLASH, OTA_STAGE_FIRMWARE,
                                  fromManifest ? _updateInfo.size : 0,
                                  pending.sha, pending.version, &pending);
    if (result < 0) {
      return result;
    }

    String filename = !_updateInfo.filename.isEmpty()
                          ? _updateInfo.filename
                          : extractFilename(url);
    if (!filename.isEmpty()) {
      saveFilenamesToEEPROM(filename, _lastInstalledFsFilename);
    }
    _flashedId = imageId(pending.sha, pending.file, pending.version);
    _updateInfo.available = false;

    if (!_autoReboot) {
      log("Update complete! Restart to boot the new firmware");
      return 1;
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
    dropStaleSignedInfo();
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
    begin();
    bool fromManifest = (url == _updateInfo.filesystemUrl);
    int result = downloadAndFlash(
        url, U_SPIFFS, OTA_STAGE_FILESYSTEM,
        fromManifest ? _updateInfo.filesystemSize : 0,
        fromManifest ? _updateInfo.filesystemSha256 : String(),
        fromManifest ? _updateInfo.filesystemVersion : String());
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
    if (_checkInterval == 0 || millis() - _lastCheck <= _checkInterval) {
      return;
    }
    _lastCheck = millis();
    if (!hasUpdate()) {
      return;
    }

    String manifest = stripQuery(_updateInfo.url) + "|" + _updateInfo.version +
                      "|" + _updateInfo.sha256 + "|" +
                      stripQuery(_updateInfo.filesystemUrl) + "|" +
                      _updateInfo.filesystemVersion + "|" +
                      _updateInfo.filesystemSha256;
    if (manifest != _failedManifest) {
      _failStreak = 0;
      _skipChecks = 0;
    } else if (_skipChecks > 0) {
      _skipChecks--;
      log("Skipping update after repeated failures");
      return;
    }

    if (update() >= 0) {
      _failStreak = 0;
      _failedManifest = "";
      return;
    }
    _failedManifest = manifest;
    if (_failStreak <= OTA_MAX_BACKOFF_SHIFT) {
      _failStreak++;
    }
    _skipChecks = 1 << (_failStreak - 1);
  }

  /**
   * @brief Check if rollback is possible
   */
  bool canRollback() {
    if (!hasOtaPartition()) {
      return false;
    }
    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
    const esp_partition_t *next_partition =
        esp_ota_get_next_update_partition(NULL);

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

  int bootToFactory() {
    begin();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory == NULL) {
      return fail(OTA_ERR_NO_FACTORY, "no factory app partition");
    }
    if (esp_ota_set_boot_partition(factory) != ESP_OK) {
      return fail(OTA_ERR_SET_BOOT, "failed to set factory boot partition");
    }
    log("Rebooting into factory app...");
    delay(500);
    ESP.restart();
    return 1;
  }

  /**
   * @brief Mark current firmware as valid (prevent auto-rollback)
   */
  bool markAsValid() {
    begin();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
      if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
          log("Firmware marked as valid");
          confirmVerified();
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

    _badId = "";
    Preferences prefs;
    if (prefs.begin(OTA_PREFS_NAMESPACE, false)) {
      removeKey(prefs, "bad_id");
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
