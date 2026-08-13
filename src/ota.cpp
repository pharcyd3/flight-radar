#include "ota.h"
#include "config.h"
#include "watchdog.h"
#include "aircraftfeed.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

static char _lastError[96] = "";

const char* otaLastError() { return _lastError; }

const char* otaCurrentVersion() { return FIRMWARE_VERSION; }

// Shared shape for the two plain-text GETs below: connect, read the whole
// body into outBuf, trim it. Follows the same WiFiClientSecure convention as
// adsblive.cpp/map.cpp/geolocate.cpp — .setInsecure(), no cert pinning
// ("matches map/adsblive/geolocate").
//
// rejectAboveLen > 0 makes a too-long body a hard failure (used for the
// version string, where "long" means "this isn't a version string, something
// is misconfigured"). rejectAboveLen == 0 instead truncates the body to fit
// outBuf, with a trailing "..." marker (used for the changelog, where a long
// body is just... a long changelog).
static bool httpGetToBuffer(const char* url, char* outBuf, size_t outLen, size_t rejectAboveLen) {
    outBuf[0] = '\0';

    if (WiFi.status() != WL_CONNECTED) {
        snprintf(_lastError, sizeof(_lastError), "WiFi not connected");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(5000);
    client.setConnectionTimeout(10000);
    client.setHandshakeTimeout(10);

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        snprintf(_lastError, sizeof(_lastError), "GET %s failed: HTTP %d", url, code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    body.trim();

    if (body.isEmpty()) {
        snprintf(_lastError, sizeof(_lastError), "GET %s returned an empty body", url);
        return false;
    }
    if (rejectAboveLen > 0 && body.length() > rejectAboveLen) {
        snprintf(_lastError, sizeof(_lastError),
                 "GET %s returned an implausible body (%u bytes)", url, (unsigned)body.length());
        return false;
    }

    size_t n = body.length();
    if (n >= outLen) {
        // Truncate to fit, carving out room for a "..." marker so a long
        // changelog reads as cut off rather than abruptly cut mid-word.
        n = (outLen > 4) ? outLen - 4 : outLen - 1;
        memcpy(outBuf, body.c_str(), n);
        outBuf[n] = '\0';
        if (outLen > 4) strcat(outBuf, "...");
    } else {
        memcpy(outBuf, body.c_str(), n);
        outBuf[n] = '\0';
    }
    return true;
}

bool otaCheckLatestVersion(char* outVersion, size_t outLen) {
    bool ok = httpGetToBuffer(OTA_VERSION_URL, outVersion, outLen, 15);
    Serial.printf("[OTA] check ok=%d version=%s\n", ok, ok ? outVersion : otaLastError());
    return ok;
}

bool otaFetchChangelog(char* outBuf, size_t outLen) {
    // Best-effort, and deliberately truncates rather than rejects (rejectAboveLen=0)
    // — callers must not treat a false return as blocking anything.
    return httpGetToBuffer(OTA_CHANGELOG_URL, outBuf, outLen, 0);
}

bool otaPerformUpdate() {
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(_lastError, sizeof(_lastError), "WiFi not connected");
        return false;
    }

    // Let a fetch in flight finish before starting a second heap-hungry TLS
    // handshake — concurrent TLS on this no-PSRAM heap is the documented
    // cause of "SSL - Memory allocation failed" elsewhere in this codebase
    // (see main.cpp's map-precache gate). Bounded: this is a manual, user-
    // initiated action, so don't block on it indefinitely.
    unsigned long waitStart = millis();
    while (feed::busy() && millis() - waitStart < 5000) {
        watchdogFeed();
        delay(50);
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(5000);
    client.setConnectionTimeout(10000);
    client.setHandshakeTimeout(10);

    // Required: HTTPUpdate defaults to rebooting internally on success, before
    // the /update/apply route handler can send any response — the browser
    // would just see a reset connection. Disabling it lets the handler send a
    // "rebooting..." page first, then reboot itself.
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.onProgress([](int done, int total) { watchdogFeed(); });

    Serial.printf("[OTA] update GET %s (heap=%u)\n", OTA_FIRMWARE_URL, ESP.getFreeHeap());
    t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);

    if (ret == HTTP_UPDATE_OK) {
        Serial.println("[OTA] update ok, awaiting caller-triggered reboot");
        return true;
    }

    snprintf(_lastError, sizeof(_lastError), "%s (code %d)",
             httpUpdate.getLastErrorString().c_str(), httpUpdate.getLastError());
    Serial.printf("[OTA] update failed: %s\n", _lastError);
    return false;
}
