#pragma once
#include <Arduino.h>

// Pull-based OTA: checks a version file hosted on GitHub Pages and, if asked,
// downloads and flashes the matching firmware.bin via the ESP32 core's
// HTTPUpdate helper. Manual only — never called from setup()/loop(), only
// from the web-config /update routes (see provisioning.cpp), because this
// device is unattended at a non-technical user's house and must never
// silently reflash itself.

// Fetches OTA_VERSION_URL and copies the trimmed remote version string into
// outVersion (outLen bytes). Returns true on a successful fetch with a
// plausible (short, non-empty) body; false otherwise — see otaLastError().
// Does not compare against the running version; the caller does that with a
// plain string comparison; there's no semver ordering here, just equality.
bool otaCheckLatestVersion(char* outVersion, size_t outLen);

// Best-effort fetch of the changenotes for the version OTA_VERSION_URL is
// currently offering. Copies (and truncates, with a trailing "...") the body
// into outBuf (outLen bytes). Returns false on any failure — callers must
// treat that as "no changelog to show", never as a reason to block a check
// or update.
bool otaFetchChangelog(char* outBuf, size_t outLen);

// Downloads OTA_FIRMWARE_URL and flashes it to the inactive OTA partition.
// Blocking — runs to completion (or failure) before returning, typically
// several to tens of seconds on home WiFi. On success the device reboots
// itself and this call does not return in that path; it only returns to the
// caller on failure, with false and otaLastError() populated.
bool otaPerformUpdate();

// Human-readable detail for the most recent failure from any function above
// (HTTP code / HTTPUpdate's own error string), for showing on the /update
// pages and logging over Serial.
const char* otaLastError();

// The compiled-in firmware version (FIRMWARE_VERSION from config.h).
const char* otaCurrentVersion();
