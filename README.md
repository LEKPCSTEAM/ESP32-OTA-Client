# ESP32-OTA-Client

A lightweight OTA (Over-The-Air) update library for ESP32 with JSON API support.

## Features

- ✅ **Check for updates** without downloading (`hasUpdate()`)
- ✅ **Update on demand** (`update()`)
- ✅ **Auto-update** on check (`checkUpdate()`)
- ✅ **Rollback** to previous firmware version
- ✅ **Firmware validation** with `markAsValid()`
- ✅ **Partition status** checking (`getBootPartition()`, `getNextUpdatePartition()`)
- ✅ **Progress callback** for download progress (legacy + stage-aware)
- ✅ **Periodic auto-check** with `setCheckInterval()`
- ✅ **Version comparison** (numeric dot segments: `1.10.0` > `1.9.0`, v1.2.0+)
- ✅ **Filesystem (LittleFS / SPIFFS) OTA** — flash data partition alongside firmware (v1.1.0+)
- ✅ **Verified downloads** — size + SHA-256 check, stall timeout, resume on drop (v1.2.0+)
- ✅ **Rollback-aware** — a firmware that rolled back is not downloaded again (v1.2.0+)
- ✅ **Device key, CA certificate, check-in and OTA result reports** (v1.2.0+)
- ✅ **Device URL** (`?deviceId=`) with signed firmware downloads; the legacy URL keeps working (v1.2.0+)
- ✅ **Factory-partition updater** support for 4 MB boards (v1.2.0+)

## What's New in 1.2.0

- Device URL `https://<host>/api/update-list?deviceId=<device uuid>` with the
  project key in `X-Device-Key`; reports carry `deviceId`. The legacy
  `?projectId=&device=` URL works unchanged — see
  [Migrating from the legacy URL](#migrating-from-the-legacy-url)
- Signed download URLs (`?exp=&sig=`, valid 1 h): the file identity ignores
  the query, a resume reuses the same URL, a signed download is capped at
  20 min, and `update()` checks again when a signed URL is over 10 min old

- Numeric version compare: `"1.10.0"` is now newer than `"1.9.0"` (was lexical)
- Optional manifest fields `size`, `sha256`, `filesystem_size`, `filesystem_sha256`
  are verified before the image is activated; a mismatch aborts the update
- Downloads abort after `setTimeout(ms)` without data (default 30 s) or after
  `setDownloadDeadline(ms)` in total (default 10 min), check every
  `Update.write`, and resume a dropped download up to 3 times with HTTP `Range`
- `loop()` backs off after a failed update (skips 1, 2, 4, then 8 intervals)
- Fixed a `WiFiClientSecure` memory leak on every HTTPS request / redirect
- `setDeviceKey()`, `setCACert()`, `setCheckin()`, `setReportUrl()`,
  `setAutoReboot()`, `setDownloadDeadline()`, `begin()`, `sendPendingReport()`,
  `bootToFactory()`
- Results are reported to `POST /api/ota-report` (`ok`, `failed`, `rollback`)
- New example: `examples/FactoryUpdater` (factory + single `ota_0` layout)
- 100% source compatible with 1.1.0 — see [Compatibility](#compatibility-with-11x)

## What's New in 1.1.0

- LittleFS / SPIFFS filesystem image OTA via `U_SPIFFS`
- A single `update()` call now flashes filesystem first, then firmware, then reboots
- New methods: `updateFilesystem()`, `doFilesystemUpdate(url)`, `hasFilesystemUpdate()`, `getLastInstalledFsFilename()`
- New stage-aware progress callback overload — `(OTAStage, percent, written, total)`
- 100% backwards compatible: existing JSON without `filesystem_url` and existing API calls behave exactly as in 1.0.x

## Installation

### PlatformIO

Add to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/LEKPCSTEAM/ESP32-OTA-Client.git
```

Or copy the `ESP32-OTA-Client` folder to your `lib/` directory.

### Dependencies

- ArduinoJson (v7+)

## Quick Start

```cpp
#include "ESP32OTAClient.h"
#include <WiFi.h>

OTAClient ota("https://your-server/api/update-list?deviceId=YOUR_DEVICE_ID", "1.0.0");

void setup() {
    Serial.begin(115200);

    WiFi.begin("YOUR_SSID", "YOUR_PASSWORD");
    while (!WiFi.isConnected()) delay(100);

    ota.setDeviceKey("YOUR_DEVICE_KEY");  // only when the project has a device key
    ota.setCACert(rootCaPem);             // PEM root CA of the server

    // Check and update if available
    ota.checkUpdate();
}

void loop() {
    // Your code here
}
```

## Manifest URL

The constructor takes any URL that returns the [JSON below](#server-api-format).
With ESP32 OTA Web Manager use one of these forms:

| Form                 | URL                                                                   | Device key |
| -------------------- | --------------------------------------------------------------------- | ---------- |
| Device URL (v1.2.0+) | `https://<host>/api/update-list?deviceId=<device uuid>`               | `setDeviceKey()`, required when the project has a key |
| Legacy               | `<base>/api/update-list?projectId=<project id>&device=<device name>` | not used   |

- `deviceId` is the device's UUID from the web manager. Every unit flashed
  with the same build uses the same `deviceId`; the server tells units apart
  by the `mac` [check-in](#check-in).
- The key travels only in the `X-Device-Key` header. The server does not
  accept `?key=` in the URL, so keep the key out of the URL.
- Both forms get `mac` and `ver` appended and send
  [reports](#ota-reports) to `<base>/api/ota-report`.
- The legacy form keeps working unchanged for devices already in the field:

```cpp
OTAClient ota("http://192.168.1.250:3000/api/update-list"
              "?projectId=06afd666-8542-4b61-bcb7-ace3116902e1&device=ESP32-S3",
              "1.0.5");
```

### Signed downloads

For a project with a device key the server returns firmware URLs that are
signed for one hour:

```text
https://<host>/firmware/<file>?exp=<unix time>&sig=<base64url>
```

- The file name used for the force-update record, the rollback blacklist and
  the `loop()` backoff comes from the URL path; the query is ignored, so a new
  signature on every check does not make the same image look new.
- A resumed download (`Range`) requests the same signed URL again.
- A signed download is aborted after 20 min even when
  `setDownloadDeadline()` is longer, so it ends well inside the hour.
- `update()` and `updateFilesystem()` fetch the manifest again when the cached
  signed URL is more than 10 min old (for example `hasUpdate()`, wait for a
  button, then `update()`). `doUpdate(url)` uses the URL as given.
- The download needs no key; `X-Device-Key` is still sent only to the
  manifest's origin.

### Server update rules

The server can offer a specific version to some devices, including an older
or a forced one. The client:

- never installs a version equal to the running one (`compareVersions()`
  returns 0), forced or not;
- installs an older version only when the entry has `"force": true`;
- installs a forced image once: an image with the same file name as the last
  installed one is skipped;
- skips an image that rolled back (see
  [Rollback-Aware Updates](#rollback-aware-updates)).

A rule that pins a version is therefore applied once, never in a loop.

## API Reference

### Constructor

```cpp
OTAClient ota(const char* jsonUrl, const char* version);
```

| Parameter | Description                              |
| --------- | ---------------------------------------- |
| `jsonUrl` | URL to your update API endpoint          |
| `version` | Current firmware version (e.g., "1.0.0") |

### Methods

| Method                     | Description                                    | Returns                                    |
| -------------------------- | ---------------------------------------------- | ------------------------------------------ |
| `hasUpdate()`              | Check if firmware OR filesystem update is available (no download) | `bool`                  |
| `hasFilesystemUpdate()`    | Whether the cached UpdateInfo has a pending fs image | `bool`                               |
| `update()`                 | Install fs (if any) then firmware (then reboot) | `int` (1=success, 0=no update, <0=error)  |
| `updateFilesystem()`       | Install only the filesystem image (then reboot) | `int`                                     |
| `doFilesystemUpdate(url)`  | Low-level: flash a fs image from a specific URL | `int`                                     |
| `checkUpdate()`            | Check and auto-update if available             | `int`                                      |
| `forceUpdate()`            | Clear cache and check/update                   | `int`                                      |
| `getUpdateInfo()`          | Get cached update info                         | `UpdateInfo`                               |
| `getLastInstalledFsFilename()` | Last successfully installed filesystem image filename | `String`                       |
| `canRollback()`            | Check if rollback is possible (`false` with a single OTA slot) | `bool`                     |
| `rollback()`               | Rollback to previous firmware                  | `int` (1=success, 0=no rollback, <0=error) |
| `markAsValid()`            | Mark firmware as valid (prevent auto-rollback) | `bool`                                     |
| `getBootPartition()`       | Get current boot partition name                | `String`                                   |
| `getNextUpdatePartition()` | Get next update partition name                 | `String`                                   |
| `getVersion()`             | Get current version                            | `String`                                   |
| `onProgress(callback)`     | Set progress callback                          | `void`                                     |
| `setCheckInterval(ms)`     | Set auto-check interval                        | `void`                                     |
| `loop()`                   | Call in loop() for auto-check (backs off after failed updates) | `void`                     |

#### Added in 1.2.0

| Method                          | Description                                                                 | Returns  |
| ------------------------------- | --------------------------------------------------------------------------- | -------- |
| `begin()`                       | Load state and decide the result of the last firmware flash (called lazily by `hasUpdate()`, `update()`, `markAsValid()`) | `void` |
| `setDeviceKey(key)`             | Send `X-Device-Key: <key>` to the manifest's origin (see [Security](#security)) | `void` |
| `setCACert(pem)`                | Verify HTTPS servers with this root CA (PEM must stay in memory)            | `void`   |
| `setTimeout(ms)`                | Abort a download after `ms` without data (default 30000)                    | `void`   |
| `setDownloadDeadline(ms)`       | Abort a download that takes longer than `ms` in total, resumes included (default 600000; at most 1200000 for a signed URL) | `void` |
| `setCheckin(enabled)`           | Force check-in params on/off (default: on for `/api/update-list` URLs)      | `void`   |
| `setReportUrl(url)`             | Override the OTA report endpoint                                            | `void`   |
| `setAutoReboot(enabled)`        | `false`: `update()` / `doUpdate()` return 1 after a verified flash, no restart | `void` |
| `sendPendingReport()`           | Send a queued `ok` / `rollback` report now                                  | `bool` (true = nothing left to send) |
| `bootToFactory()`               | Boot the `factory` app partition and restart                                | `int` (`OTA_ERR_NO_FACTORY` if none) |
| `getLastError()`                | Detail of the last failure (e.g. `"sha256 mismatch, got ..."`)              | `String` |
| `OTAClient::compareVersions(a, b)` | Numeric version compare                                                  | `int` (-1, 0, 1) |
| `OTAClient::errorToString(code)` | Text for a return code                                                     | `const char*` |
| `OTAClient::getPartitionVersion(label)` | Version last run from that OTA partition (e.g. `"ota_0"`), or `""`   | `String` |

`UpdateInfo` also gains `device`, `size`, `sha256`, `filesystemSize` and
`filesystemSha256` (0 / empty when the server does not send them).

### Progress Callback

Two overloads are available — pick whichever you prefer:

```cpp
// Legacy — same as v1.0.x
ota.onProgress([](int percent, int bytesWritten, int totalBytes) {
    Serial.printf("Progress: %d%% (%d/%d)\n", percent, bytesWritten, totalBytes);
});

// Stage-aware — distinguishes firmware vs filesystem
ota.onProgress([](OTAStage stage, int percent, int written, int total) {
    const char* kind = (stage == OTA_STAGE_FIRMWARE) ? "FW" : "FS";
    Serial.printf("[%s] %d%%\n", kind, percent);
});
```

## Server API Format

Your server should return JSON in this format:

```json
{
  "updater": [
    {
      "device": "ESP32-S3",
      "version": "1.0.1",
      "url": "http://your-server/firmware/v1.0.1.bin"
    }
  ]
}
```

### With filesystem image (v1.1.0+)

Add the optional `filesystem_url` field — devices on v1.0.x will simply ignore it,
while v1.1.0+ devices will flash the filesystem image alongside the firmware.

```json
{
  "updater": [
    {
      "device": "ESP32-S3",
      "version": "1.1.0",
      "force": false,
      "url":              "http://your-server/firmware-v1.1.0.bin",
      "filesystem_url":   "http://your-server/littlefs-v1.1.0.bin",
      "filesystem_version": "1.1.0"
    }
  ]
}
```

Build the filesystem image with PlatformIO:

```bash
pio run --target buildfs
# output: .pio/build/<env>/littlefs.bin
```

### Update Order

When both URLs are present, `update()` does:

1. Flash the **filesystem** to the data partition (no reboot).
   While this runs, the running firmware should NOT touch LittleFS — close
   any open files / call `LittleFS.end()` first.
2. Flash the **firmware** to the inactive OTA partition.
3. Reboot into the new firmware, which mounts the new filesystem.

Filesystem-only updates (no `url`) flash the data partition then reboot so
the new image is mounted cleanly.

### Integrity fields (v1.2.0+)

All optional. When a field is absent its check is skipped, so older servers
keep working.

```json
{
  "updater": [
    {
      "device": "ESP32-S3",
      "version": "1.2.0",
      "force": false,
      "url": "http://your-server/firmware-v1.2.0.bin",
      "size": 1048576,
      "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
      "filesystem_url": "http://your-server/littlefs-v1.2.0.bin",
      "filesystem_version": "1.2.0",
      "filesystem_size": 1441792,
      "filesystem_sha256": "2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae"
    }
  ]
}
```

| Field               | Check                                                              |
| ------------------- | ------------------------------------------------------------------ |
| `size`              | Must equal the download's `Content-Length` and the bytes written; used as the length when the server sends none |
| `sha256`            | 64 hex chars; SHA-256 computed while streaming must match          |
| `filesystem_size`   | Same as `size`, for the filesystem image                           |
| `filesystem_sha256` | Same as `sha256`, for the filesystem image                         |

Both checks run before `Update.end()`, so a bad image is never activated: the
update is aborted and `OTA_ERR_SIZE` (-8) or `OTA_ERR_CHECKSUM` (-9) is
returned.

A download that stops sending data for `setTimeout()` ms (default 30 s) or
drops the connection is resumed up to 3 times with `Range: bytes=<written>-`.
Only an HTTP `206` whose `Content-Range` starts at the bytes already written is
accepted; anything else aborts with `OTA_ERR_RESUME` (-10). If nothing was
written yet, the retry is a plain request and a `200` of the same length is
accepted. The whole download, resumes included, is aborted with
`OTA_ERR_DEADLINE` (-13) after `setDownloadDeadline()` ms (default 10 min).

A response without `Content-Length` is accepted only when the manifest has
`size` (or `filesystem_size`). A `Transfer-Encoding: chunked` response is
requested again over HTTP/1.0 with `Accept-Encoding: identity`, which servers
answer unchunked; if it is still chunked the download fails.

The manifest body is limited to 8 KB; a larger or unreadable manifest is
treated as "no update".

## Security

```cpp
ota.setDeviceKey("device-secret");   // X-Device-Key header to the manifest's origin
ota.setCACert(rootCaPem);            // verify the HTTPS server
```

- The device key is sent on manifest, download, redirect and report requests
  only when the URL has the same scheme and host (and port) as the manifest
  URL. It is never sent to another host, and never over `http://` unless the
  manifest URL itself is `http://`, so an `https` → `http` redirect drops it.
  Serve firmware from the manifest's origin when the server requires the key;
  [signed download URLs](#signed-downloads) need no key, so they may point to
  another host.
- The key is never put in the URL; the server does not accept `?key=`.
- Without `setCACert()` HTTPS connections use `setInsecure()` (same as 1.1.0)
  and a one-time warning is logged. Set it in production: the key is only as
  safe as the TLS connection that carries it.

## Check-in

When the manifest URL contains `/api/update-list` and has no `mac=` parameter,
the client appends `mac=<WiFi MAC>&ver=<current version>` (URL-encoded) so the
server can track devices. `setCheckin(false)` turns it off, `setCheckin(true)`
turns it on for any URL.

## OTA Reports

Results are posted as JSON with `Content-Type: application/json` and the
device key header:

```http
POST <base>/api/ota-report
```

With a device URL (`?deviceId=`):

```json
{
  "deviceId": "3f2b8c1e-...",
  "mac": "AA:BB:CC:DD:EE:FF",
  "from": "1.1.0",
  "to": "1.2.0",
  "result": "ok",
  "detail": "optional, max 200 chars"
}
```

With a legacy URL (`?projectId=&device=`):

```json
{
  "projectId": "06afd666-...",
  "device": "ESP32-S3",
  "mac": "AA:BB:CC:DD:EE:FF",
  "from": "1.1.0",
  "to": "1.2.0",
  "result": "ok",
  "detail": "optional, max 200 chars"
}
```

- `<base>` and the identity fields come from a manifest URL of the form
  `<base>/api/update-list?deviceId=...` or
  `<base>/api/update-list?projectId=...&device=...`. When the URL has
  `deviceId`, the report uses it and leaves out `projectId` / `device`.
  `setReportUrl()` overrides the endpoint. With neither, no report is sent.
- `failed` is sent right away when a download or verification fails.
- `ok` / `rollback` are decided after the next boot (see below), stored in NVS
  and sent at the start of the next `hasUpdate()` or by `sendPendingReport()`.
  A 5xx or network error keeps the report queued; 2xx or 4xx clears it.
- Report requests time out after at most 10 s.
- A report failure never blocks an update.

## Rollback-Aware Updates

After a firmware image is downloaded and verified, and before it is activated
(`Update.end()`), the client stores a pending record (filename, target
version, previous version, manifest `sha256`, target partition) in the NVS
namespace `otacli`. The target partition is written last. If the record
cannot be saved the update is aborted with `OTA_ERR_STATE` (-14); if
activation fails the record is removed. On the next boot, `begin()` decides:

| Situation on boot                                      | Result                                   |
| ------------------------------------------------------ | ---------------------------------------- |
| Running the target partition, image valid              | `ok` report queued                       |
| Running the target partition, `ESP_OTA_IMG_PENDING_VERIFY` | Waits; `ok` queued when `markAsValid()` succeeds |
| Running another partition, target `INVALID` or `ABORTED` | `rollback` queued; image blacklisted   |
| Running another partition, target `VALID`              | `ok` report queued (left on purpose)     |
| Running another partition, any other state (e.g. after `bootToFactory()` before the new image ran) | Record kept until a later boot decides or a later update replaces it |

The blacklist key is `sha256:<hash>` when the manifest has `sha256`, otherwise
`<filename>@<version>`; an entry with neither is never blacklisted.
`hasUpdate()` skips a manifest entry with a blacklisted key (normal and
force), so the device does not download it again; a new build with a new hash
or version is not affected. `clearFirmwareRecord()` clears the blacklist
together with the EEPROM records.

### Backoff in `loop()`

When an update started by `loop()` fails, the next attempts for the same
manifest entry skip 1, 2, 4, then 8 check intervals (the manifest is still
fetched every interval). A success or any change in the manifest entry (URL,
version or hash) resets the backoff. Direct calls to `checkUpdate()` /
`update()` are not throttled.

## Factory Updater (4 MB boards)

Boards that cannot fit two full app slots can keep a small updater in the
`factory` partition and the main app in a single `ota_0` slot:

```csv
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
factory,  app,  factory, 0x10000,  0x110000,
ota_0,    app,  ota_0,   0x120000, 0x200000,
spiffs,   data, spiffs,  0x320000, 0xE0000,
```

The file is `examples/FactoryUpdater/partitions_factory_updater.csv`.

- **Updater** (`examples/FactoryUpdater`): build with
  `board_build.partitions = partitions_factory_updater.csv` and upload normally
  (it lands in `factory`). It joins Wi-Fi, checks the manifest using the
  version `ota_0` last ran with (`OTAClient::getPartitionVersion("ota_0")`),
  or `0.0.0` when `ota_0` holds no bootable image, flashes `ota_0` with
  `setAutoReboot(false)`, selects `ota_0` and restarts. The recorded version
  of a slot is cleared before the slot is written, so a half-written `ota_0`
  is never mistaken for the version it held before. With no update or on
  error it boots `ota_0` if that image is valid, otherwise it retries every
  minute.
- **Main app** (`ota_0`): same partition table plus
  `board_upload.offset_address = 0x120000` and
  `board_upload.maximum_size = 2097152`. It cannot flash itself:
  `update()` returns `OTA_ERR_NO_OTA_PARTITION` (-11) and `canRollback()`
  returns `false`. Hand over to the updater instead:

```cpp
if (ota.hasUpdate() && ota.update() == OTA_ERR_NO_OTA_PARTITION) {
    ota.bootToFactory();   // restarts into the factory updater
}
```

## Examples

### Basic Update

```cpp
// Check and update immediately
ota.checkUpdate();
```

### Manual Check and Update

```cpp
if (ota.hasUpdate()) {
    UpdateInfo info = ota.getUpdateInfo();
    Serial.printf("New version: %s\n", info.version.c_str());

    // User confirms...
    ota.update();
}
```

### Button Trigger

```cpp
#define BTN_PIN 0

void loop() {
    if (digitalRead(BTN_PIN) == LOW) {
        if (ota.hasUpdate()) {
            Serial.println("Update found! Press again to install");
            // Wait for second press...
            ota.update();
        }
    }
}
```

### Firmware + Filesystem Update

```cpp
#include <LittleFS.h>

void setup() {
    LittleFS.begin(true);
    // ... wifi connect ...

    if (ota.hasUpdate()) {
        UpdateInfo info = ota.getUpdateInfo();

        // Stop using LittleFS before the data partition is flashed
        if (info.filesystemAvailable) {
            LittleFS.end();
        }

        ota.update();  // flashes fs first, then firmware, then reboots
    }
}
```

See `examples/FilesystemUpdate/FilesystemUpdate.ino` for the full flow.

### Periodic Auto-Check

```cpp
void setup() {
    // Check every 5 minutes
    ota.setCheckInterval(5 * 60 * 1000);
}

void loop() {
    ota.loop();  // Will auto-check and update
}
```

### Rollback on Failed Update

```cpp
void setup() {
    // Perform self-test
    if (systemSelfTest()) {
        ota.markAsValid();  // Prevent auto-rollback
    } else {
        Serial.println("Self-test failed, rolling back...");
        ota.rollback();  // Will reboot to previous firmware
    }
}

bool systemSelfTest() {
    // Test WiFi, sensors, critical functions
    if (WiFi.status() != WL_CONNECTED) return false;
    if (ESP.getFreeHeap() < 50000) return false;
    // Add your tests here
    return true;
}
```

### Manual Rollback Trigger

```cpp
void loop() {
    // Triple-press BOOT button to rollback
    static int pressCount = 0;

    if (digitalRead(0) == LOW) {
        pressCount++;
        if (pressCount >= 3 && ota.canRollback()) {
            ota.rollback();  // Revert to previous firmware
        }
        delay(500);
    }
}
```

### Check Partition Status

```cpp
void setup() {
    Serial.print("Boot Partition: ");
    Serial.println(ota.getBootPartition());  // e.g., "ota_0"

    Serial.print("Next Update Partition: ");
    Serial.println(ota.getNextUpdatePartition());  // e.g., "ota_1"

    Serial.print("Can Rollback: ");
    Serial.println(ota.canRollback() ? "YES" : "NO");
}
```

## Partition Requirements

For rollback functionality to work, your ESP32 must use an OTA partition scheme. In PlatformIO, set this in `platformio.ini`:

```ini
board_build.partitions = default_16MB.csv  ; or min_spiffs.csv, etc.
```

The partition table must include:

- Two OTA partitions (ota_0 and ota_1)
- OTA data partition (otadata)

## Error Codes

Return values are also available as the `OTAResult` enum;
`OTAClient::errorToString(code)` gives the text and `getLastError()` the detail.

| Code | Constant                        | Description                                         |
| ---- | ------------------------------- | --------------------------------------------------- |
| 1    | `OTA_SUCCESS`                   | Success (device reboots unless `setAutoReboot(false)`) |
| 0    | `OTA_NO_UPDATE`                 | No update/rollback available                        |
| -1   | `OTA_ERR_NO_ROLLBACK_PARTITION` | Rollback partition not found                        |
| -2   | `OTA_ERR_SET_BOOT`              | Failed to set boot partition                        |
| -3   | `OTA_ERR_DOWNLOAD`              | Download failed (HTTP error, bad length, connection lost) |
| -4   | `OTA_ERR_NO_SPACE`              | Not enough space                                    |
| -5   | `OTA_ERR_UPDATE`                | Update failed (image rejected by `Update.end()`)    |
| -6   | `OTA_ERR_TIMEOUT`               | Download stalled longer than `setTimeout()`         |
| -7   | `OTA_ERR_WRITE`                 | Flash write failed                                  |
| -8   | `OTA_ERR_SIZE`                  | Size mismatch (manifest vs `Content-Length` / bytes written) |
| -9   | `OTA_ERR_CHECKSUM`              | SHA-256 mismatch or malformed `sha256` in manifest  |
| -10  | `OTA_ERR_RESUME`                | Server refused to resume (no valid `206`)           |
| -11  | `OTA_ERR_NO_OTA_PARTITION`      | No OTA partition to write (e.g. app running in a factory + `ota_0` layout) |
| -12  | `OTA_ERR_NO_FACTORY`            | `bootToFactory()`: no factory partition             |
| -13  | `OTA_ERR_DEADLINE`              | Download took longer than `setDownloadDeadline()`   |
| -14  | `OTA_ERR_STATE`                 | Could not save OTA state in NVS; update aborted before activation |

## Compatibility with 1.1.x

- Every 1.1.0 method keeps its signature and default behaviour; sketches
  compile unchanged.
- EEPROM records are untouched; the new state lives in the NVS namespace
  `otacli`.
- Version compare is numeric now. A manifest that relied on lexical order
  (e.g. offering `"1.9.0"` to a device on `"1.10.0"`) no longer updates.
- Downloads without `Content-Length` are still rejected unless the manifest
  has `size`; downloads that end early are now rejected instead of being
  finalized.
- A download now fails after 10 min in total (`setDownloadDeadline()` to
  change it), and a manifest larger than 8 KB is ignored.
- `loop()` no longer retries a failing update every interval; it backs off
  (see [Backoff](#backoff-in-loop)).
- `canRollback()` returns `false` when the next OTA slot is the running one
  (single OTA slot layouts), where 1.1.0 returned `true` and `rollback()`
  rebooted into the same image.
- A firmware update is aborted with `OTA_ERR_STATE` (-14) when the NVS state
  cannot be written.
- The file name taken from a download URL ignores everything after `?` or
  `#`; 1.1.0 cut only at a `?` inside the last path segment.

## Migrating from the legacy URL

Devices already in the field (v1.0.x / v1.1.0) poll the legacy URL without a
key and without `ver`. The server keeps that URL working, so nothing breaks
while you migrate. To move them to the device URL:

1. **Collect the IDs.** In the web manager, copy the device's UUID
   (`deviceId`) and, if the project uses one, the project device key.
2. **Build a bridge firmware.** This is your application built with this
   library (v1.2.0+), the device URL and the key:

   ```cpp
   OTAClient ota("https://<host>/api/update-list?deviceId=<device uuid>", "2.0.0");

   void setup() {
       // ... Wi-Fi ...
       ota.setDeviceKey("<project device key>");
       ota.setCACert(rootCaPem);
       ota.checkUpdate();
   }
   ```

   Give it a version string that no legacy unit runs and a firmware file name
   that no unit installed before: v1.0.x / v1.1.0 skip a forced image whose
   version equals the running one or whose file name matches the last one
   they installed.
3. **Test it on one unit.** Flash the bridge over USB and check that it gets
   HTTP 200 from the device URL. A wrong `deviceId` or key leaves a unit on
   the bridge with no further updates.
4. **Serve it to legacy units.** Upload the bridge to the device and add an
   update rule with scope `legacy`, "no version reported" on and force on.
   Legacy clients send no `ver`, so all of them match. Force is needed because
   v1.1.0 compares versions as plain strings (`"1.10.0"` sorts before
   `"1.9.0"`), which can hide a newer bridge. If the legacy response carries
   signed URLs, v1.0.x / v1.1.0 drop the query when they record the file name.
5. **Units switch over.** Each legacy unit installs the bridge once and
   reboots into it. From then on it polls the device URL with `mac`, `ver`
   and the key, and reports with `deviceId`; the legacy rule no longer
   applies to it.
6. **Finish.** Release later firmware for the device URL as usual. Once every
   unit checks in on the device URL, the legacy URL can be switched off for
   the project on the server.

Units that already run 1.2.0 on the legacy URL send `ver`, so "no version
reported" does not match them; give them a legacy rule with a version range
instead.

## How Rollback Works

ESP32 uses a dual-partition OTA system:

1. When you update, new firmware is written to the inactive partition
2. After successful download, the device reboots to the new partition
3. New firmware should call `markAsValid()` after self-testing
4. If `markAsValid()` is not called and the device reboots, bootloader auto-rolls back
5. Manual rollback via `rollback()` switches back to the previous partition

## License

MIT License - feel free to use in your projects!

## Author

**LEKPCSTEAM** - [GitHub](https://github.com/LEKPCSTEAM)
