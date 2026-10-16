# Changelog

All notable changes to **ESP32-OTA-Client** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-10-16

### Added

- Optional manifest fields `size`, `sha256`, `filesystem_size`,
  `filesystem_sha256`. Size is checked against `Content-Length` and the bytes
  written; SHA-256 is computed while streaming. Both run before
  `Update.end()`; a mismatch aborts the update. A response without
  `Content-Length` uses the manifest `size`; a chunked response is requested
  again over HTTP/1.0 with `Accept-Encoding: identity` and rejected if it is
  still chunked.
- Resumable downloads: a dropped or stalled download is retried up to 3 times
  with `Range: bytes=<written>-`, keeping the same `Update` session and hash.
  Only a `206` whose `Content-Range` starts at the written offset is accepted;
  with nothing written yet, a `200` of the same length restarts the download.
- `setTimeout(ms)` — abort a download after `ms` without data (default 30 s).
- `setDownloadDeadline(ms)` — abort a download that takes longer than `ms` in
  total, resumes included (default 10 min); returns `OTA_ERR_DEADLINE` (-13).
- `setDeviceKey(key)` — `X-Device-Key` header on manifest, download, redirect
  and report requests whose scheme and host match the manifest URL; never sent
  to another host or over a downgraded (`https` → `http`) scheme.
- `setCACert(pem)` — verify HTTPS servers. Without it `setInsecure()` is kept
  and a one-time warning is logged.
- Check-in: manifest URLs containing `/api/update-list` without `mac=` get
  `mac=<WiFi MAC>&ver=<version>` appended. `setCheckin(bool)` overrides.
- Device URL `https://<host>/api/update-list?deviceId=<device uuid>`; the
  project key goes in `X-Device-Key` via `setDeviceKey()`, never in the URL.
  The legacy `?projectId=<id>&device=<name>` URL works unchanged.
- OTA reports: `POST <base>/api/ota-report` with
  `{deviceId, mac, from, to, result, detail?}` when the manifest URL has
  `deviceId`, otherwise `{projectId, device, mac, from, to, result, detail?}`;
  `result` is `ok`, `failed` or `rollback`. `setReportUrl(url)` overrides the
  endpoint. `sendPendingReport()` sends a queued report on demand. Report
  requests time out after at most 10 s.
- Signed download URLs (`?exp=<unix>&sig=<b64url>`, valid 1 h): a resume
  requests the same URL, a signed download is aborted after at most 20 min
  whatever `setDownloadDeadline()` says, and `update()` /
  `updateFilesystem()` fetch the manifest again when the cached signed URL is
  more than 10 min old.
- README section "Migrating from the legacy URL": a bridge firmware on the
  device URL, served to legacy units by a forced `legacy` update rule.
- Rollback-aware state in NVS namespace `otacli`: the pending record (file,
  version, previous version, manifest `sha256`, target partition last) is
  saved before `Update.end()` activates the image and removed if activation
  fails; an NVS write failure aborts the update with `OTA_ERR_STATE` (-14).
  `begin()` (also called lazily) decides `ok` / `rollback` after reboot,
  waiting for `markAsValid()` when the image is `PENDING_VERIFY`. `rollback`
  is decided only when the target image is `INVALID` or `ABORTED`; other
  states keep the record until a later boot decides or a later update
  replaces it. A rolled-back image is skipped by `hasUpdate()` (normal and
  force), keyed on `sha256:<hash>` or else `<filename>@<version>`.
- `loop()` backs off after a failed update of the same manifest entry: it
  skips 1, 2, 4, then 8 check intervals; a success or a changed entry resets
  it.
- The manifest body is limited to 8 KB.
- The recorded version of an OTA slot is cleared before the slot is written.
- `setAutoReboot(bool)` — `false` makes `update()` / `doUpdate()` return 1
  after a verified flash without restarting.
- `bootToFactory()` — boot the `factory` app partition.
- `OTAClient::compareVersions()`, `OTAClient::errorToString()`,
  `OTAClient::getPartitionVersion()`, `getLastError()`.
- `OTAResult` enum with named return codes; new codes `-6` … `-14`.
- `UpdateInfo` fields `device`, `size`, `sha256`, `filesystemSize`,
  `filesystemSha256`.
- New example `examples/FactoryUpdater` with
  `partitions_factory_updater.csv` (factory + single `ota_0`, 4 MB). It
  checks with version `0.0.0` when `ota_0` holds no bootable image.

### Changed

- Examples `Basic`, `PeriodicCheck` and `FactoryUpdater` use the device URL
  with `setDeviceKey()` and `setCACert()`; `ButtonTrigger` and
  `FilesystemUpdate` keep the legacy URL.

### Fixed

- Version compare was lexical (`"1.10.0"` < `"1.9.0"`). It now compares dot
  segments numerically, ignores a leading `v`, treats missing segments as 0,
  sorts a `-pre` release before the plain release, and falls back to lexical
  order for non-numeric segments.
- `WiFiClientSecure` leaked on every HTTPS request and redirect. The client is
  now owned per request and freed after `http.end()`.
- The download loop ignored `Update.write()` failures, could spin forever on a
  silent connection, and finalized images shorter than `Content-Length`.
- A firmware that rolled back was downloaded again on every check.
- `update()` on an app with no other OTA slot (factory + `ota_0` layout) no
  longer writes over the running partition; it returns
  `OTA_ERR_NO_OTA_PARTITION`.
- `canRollback()` returned `true` with a single OTA slot, where `rollback()`
  only rebooted into the same image; it now returns `false`.
- A download could run forever on a slow trickle and a manifest of any size
  was read into RAM.
- The file name of a download URL (force-update record, rolled-back image
  key, `loop()` backoff) now comes from the URL path only; a query or
  fragment, such as a new signature on every check, no longer changes it.

### Compatibility

- All 1.1.0 signatures and defaults are unchanged; existing sketches compile
  and behave the same apart from the fixes above. Visible differences:
  `loop()` backs off after failed updates, downloads stop after 10 min, a
  manifest over 8 KB is ignored, and `canRollback()` is `false` with a single
  OTA slot.
- EEPROM v1/v2 filename records are untouched. `clearFirmwareRecord()` also
  clears the rolled-back image key.
- Servers that do not send the new fields or do not implement
  `/api/ota-report` keep working; reports are dropped on a 4xx.
- Legacy manifest URLs keep their report body; only a URL with `deviceId`
  switches to the device report body.

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
