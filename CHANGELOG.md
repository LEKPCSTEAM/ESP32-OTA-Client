# Changelog

All notable changes to **ESP32-OTA-Client** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-05-14

### Added

- **Filesystem (LittleFS / SPIFFS) OTA support** — flash the data partition
  alongside the firmware via `U_SPIFFS`.
- New JSON fields on the `updater[]` entries (both optional):
  - `filesystem_url` — URL of the filesystem image (`littlefs.bin` / `spiffs.bin`)
  - `filesystem_version` — informational version string for the fs image
- New methods:
  - `updateFilesystem()` — download + flash filesystem image, then reboot
  - `doFilesystemUpdate(const String& url)` — low-level fs flash from a URL
  - `hasFilesystemUpdate()` — convenience accessor on cached `UpdateInfo`
  - `getLastInstalledFsFilename()` — last successfully-installed fs filename
- New stage-aware progress callback overload:
  `onProgress(std::function<void(OTAStage, int, int, int)>)` — distinguishes
  firmware vs filesystem progress. Existing 3-arg overload still works.
- New `OTAStage` enum: `OTA_STAGE_FIRMWARE`, `OTA_STAGE_FILESYSTEM`.
- New `UpdateInfo` fields: `filesystemAvailable`, `filesystemUrl`,
  `filesystemFilename`, `filesystemVersion`.
- New example: `examples/FilesystemUpdate/FilesystemUpdate.ino`.

### Changed

- `update()` now performs a two-stage flash when both firmware and
  filesystem images are available:
  1. Flash filesystem (no reboot)
  2. Flash firmware (auto-reboots into the new image with the new fs in place)
  Filesystem-only updates flash the data partition then reboot.
- `hasUpdate()` now returns `true` when **either** a firmware **or** a
  filesystem update is available, and populates both sets of fields on
  `UpdateInfo`.
- Internal download/flash logic refactored into a single shared helper
  (`downloadAndFlash`) used by both `U_FLASH` and `U_SPIFFS` paths.
- `OTA_EEPROM_SIZE` bumped from 128 → 256 bytes to fit firmware + fs
  filename records. The first 128 bytes still hold the legacy v1 layout,
  so existing devices keep their last-installed firmware record on upgrade.

### Compatibility

- **100% backwards compatible with 1.0.x.**
- Existing JSON without `filesystem_url` behaves exactly as before
  (firmware-only flow, single reboot).
- All existing public API signatures are unchanged. Code written against
  1.0.x compiles and runs without modification.
- EEPROM:
  - v1 records (magic `0xAA55`) are still read on startup — devices
    upgrading from 1.0.x retain their last-installed firmware filename.
  - v2 records (magic `0xAA56`) are written by 1.1.0 and store both
    firmware + filesystem filenames.
  - A 1.0.x downgrade after a 1.1.0 install would not recognize the v2
    magic and would treat the record as missing (worst case: one extra
    force-update). No data is lost.

### Notes for Integrators

- When a filesystem update is about to run, the application should stop
  using LittleFS (close open files / call `LittleFS.end()`). The flash
  region is rewritten directly under the mount, and any cached state
  becomes invalid until reboot.
- Build the filesystem image with PlatformIO:
  `pio run --target buildfs` → `.pio/build/<env>/littlefs.bin`
- Filesystem rollback is not supported — there is only one data partition
  on standard ESP32 partition tables. Firmware rollback (via `rollback()`)
  is unchanged.

## [1.0.3] - 2025-12-25

- EEPROM-based duplicate prevention for force updates
- HTTP redirect following (up to 5 hops, HTTP + HTTPS)
- Progress callback support
- Rollback / `markAsValid` / partition introspection
- Periodic auto-check via `setCheckInterval()` + `loop()`
