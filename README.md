# ESP32-OTA-Client

A lightweight OTA (Over-The-Air) update library for ESP32 with JSON API support.

## Features

- ✅ **Check for updates** without downloading (`hasUpdate()`)
- ✅ **Update on demand** (`update()`)
- ✅ **Auto-update** on check (`checkUpdate()`)
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

| Parameter | Description |
|-----------|-------------|
| `jsonUrl` | URL to your update API endpoint |
| `version` | Current firmware version (e.g., "1.0.0") |

### Methods

| Method | Description | Returns |
|--------|-------------|---------|
| `hasUpdate()` | Check if update available (no download) | `bool` |
| `update()` | Download and install update | `int` (1=success, 0=no update, <0=error) |
| `checkUpdate()` | Check and auto-update if available | `int` |
| `forceUpdate()` | Clear cache and check/update | `int` |
| `getUpdateInfo()` | Get cached update info | `UpdateInfo` |
| `getVersion()` | Get current version | `String` |
| `onProgress(callback)` | Set progress callback | `void` |
| `setCheckInterval(ms)` | Set auto-check interval | `void` |
| `loop()` | Call in loop() for auto-check | `void` |

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

## Error Codes

| Code | Description |
|------|-------------|
| 1 | Success (device will reboot) |
| 0 | No update available |
| -1 | WiFi not connected |
| -2 | Invalid JSON response |
| -3 | Download failed |
| -4 | Not enough space |
| -5 | Update failed |

## License

MIT License - feel free to use in your projects!

## Author

**LEKPCSTEAM** - [GitHub](https://github.com/LEKPCSTEAM)
