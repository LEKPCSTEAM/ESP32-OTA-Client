# ESP32-OTA-Client

A lightweight OTA (Over-The-Air) update library for ESP32 with JSON API support.

## Features

- ✅ **Check for updates** without downloading (`hasUpdate()`)
- ✅ **Update on demand** (`update()`)
- ✅ **Auto-update** on check (`checkUpdate()`)
- ✅ **Rollback** to previous firmware version
- ✅ **Firmware validation** with `markAsValid()`
- ✅ **Partition status** checking (`getBootPartition()`, `getNextUpdatePartition()`)
- ✅ **Progress callback** for download progress
- ✅ **Periodic auto-check** with `setCheckInterval()`
- ✅ **Version comparison** (string-based)

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

OTAClient ota("http://your-server/api/update?device=esp32", "1.0.0");

void setup() {
    Serial.begin(115200);

    WiFi.begin("YOUR_SSID", "YOUR_PASSWORD");
    while (!WiFi.isConnected()) delay(100);

    // Check and update if available
    ota.checkUpdate();
}

void loop() {
    // Your code here
}
```

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
| `hasUpdate()`              | Check if update available (no download)        | `bool`                                     |
| `update()`                 | Download and install update                    | `int` (1=success, 0=no update, <0=error)   |
| `checkUpdate()`            | Check and auto-update if available             | `int`                                      |
| `forceUpdate()`            | Clear cache and check/update                   | `int`                                      |
| `getUpdateInfo()`          | Get cached update info                         | `UpdateInfo`                               |
| `canRollback()`            | Check if rollback is possible                  | `bool`                                     |
| `rollback()`               | Rollback to previous firmware                  | `int` (1=success, 0=no rollback, <0=error) |
| `markAsValid()`            | Mark firmware as valid (prevent auto-rollback) | `bool`                                     |
| `getBootPartition()`       | Get current boot partition name                | `String`                                   |
| `getNextUpdatePartition()` | Get next update partition name                 | `String`                                   |
| `getVersion()`             | Get current version                            | `String`                                   |
| `onProgress(callback)`     | Set progress callback                          | `void`                                     |
| `setCheckInterval(ms)`     | Set auto-check interval                        | `void`                                     |
| `loop()`                   | Call in loop() for auto-check                  | `void`                                     |

### Progress Callback

```cpp
ota.onProgress([](int percent, int bytesWritten, int totalBytes) {
    Serial.printf("Progress: %d%% (%d/%d)\n", percent, bytesWritten, totalBytes);
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

| Code | Description                  |
| ---- | ---------------------------- |
| 1    | Success (device will reboot) |
| 0    | No update/rollback available |
| -1   | Rollback partition not found |
| -2   | Failed to set boot partition |
| -3   | Download failed              |
| -4   | Not enough space             |
| -5   | Update failed                |

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
