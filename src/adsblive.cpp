#include <LittleFS.h>   // before M5Dial.h — mirrors opensky.cpp's include-order note
#include "adsblive.h"
#include "opensky.h"    // shared ApiStatus + setApiStatus()
#include "config.h"

#include <M5Dial.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "watchdog.h"

static const float KM_PER_NM = 1.852f;

// ── Body-to-flash streaming (same rationale as opensky.cpp: keep the big
// response out of the fragmented, no-PSRAM heap) — local copies of the helpers.

static size_t streamBodyToFile(HTTPClient& http, bool chunked, int contentLen, File& f) {
    Stream* stream = http.getStreamPtr();
    uint8_t buf[512];
    unsigned long deadline = millis() + 9000;
    size_t total = 0;

    if (chunked) {
        while (millis() < deadline) {
            String hdr = stream->readStringUntil('\n');   // "<hexsize>\r"
            hdr.trim();
            if (hdr.isEmpty()) { if (!http.connected()) break; else continue; }
            long n = strtol(hdr.c_str(), nullptr, 16);
            if (n <= 0) break;
            while (n > 0 && millis() < deadline) {
                int want = n < (long)sizeof(buf) ? (int)n : (int)sizeof(buf);
                int got  = stream->readBytes(buf, want);
                if (got <= 0) { if (!http.connected()) return total; else continue; }
                f.write(buf, got);
                total += got;
                n -= got;
            }
            stream->readBytes(buf, 2);                     // trailing CRLF
        }
    } else {
        long n = contentLen;
        while (n != 0 && millis() < deadline &&
               (http.connected() || stream->available())) {
            if (!stream->available()) { delay(1); continue; }
            int want = (n > 0 && n < (long)sizeof(buf)) ? (int)n : (int)sizeof(buf);
            int got  = stream->readBytes(buf, want);
            if (got <= 0) break;
            f.write(buf, got);
            total += got;
            if (n > 0) n -= got;
        }
    }
    return total;
}

static bool skipPastToken(File& f, const char* token) {
    size_t i = 0, n = strlen(token);
    int c;
    while ((c = f.read()) >= 0) {
        if (c == (uint8_t)token[i]) { if (++i == n) return true; }
        else                        { i = (c == (uint8_t)token[0]) ? 1 : 0; }
    }
    return false;
}

bool fetchAircraftAdsbLive(float centerLat, float centerLon, float radiusKm,
                           std::vector<Aircraft>& out) {
    out.clear();

    // Radius param is nautical miles, capped at the API's 250 nm maximum.
    int radiusNm = (int)lroundf(radiusKm / KM_PER_NM);
    if (radiusNm < 1)   radiusNm = 1;
    if (radiusNm > 250) radiusNm = 250;

    char url[128];
    snprintf(url, sizeof(url), "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
             centerLat, centerLon, radiusNm);
    Serial.printf("[AdsbLive] GET %s  (heap=%u)\n", url, ESP.getFreeHeap());

    static const char* JSON_TMP = "/adsb.json";
    int  code = -1, size = -1;
    bool chunked = false;

    // ── HTTP + body-to-flash in its own scope, so the TLS context is freed
    // before we parse (same heap constraint as the OpenSky path). ──
    {
        const char* hdrKeys[] = { "Transfer-Encoding" };
        HTTPClient       http;
        WiFiClientSecure client;
        client.setInsecure();      // no cert verification for v1
        client.setTimeout(10);

        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt > 0) client.stop();
            watchdogFeed();
            http.begin(client, url);
            http.addHeader("User-Agent", PRODUCT_UA);   // community API — identify politely
            http.setTimeout(9000);
            http.collectHeaders(hdrKeys, 1);
            code = http.GET();
            size = http.getSize();
            if (code >= 0) break;
            Serial.printf("[AdsbLive] HTTP %d (heap=%u) — retrying once\n", code, ESP.getFreeHeap());
            http.end();
            delay(250);
        }

        if (code != HTTP_CODE_OK) {
            char msg[48];
            if (code < 0) snprintf(msg, sizeof(msg), "Net error %d", code);
            else          snprintf(msg, sizeof(msg), "HTTP %d", code);
            setApiStatus(code < 0 ? ApiState::NetError : ApiState::HttpError,
                         ApiAuth::Anonymous, code, size, msg);
            Serial.printf("[AdsbLive] %s\n", msg);
            http.end();
            return false;
        }

        chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
        File wf = LittleFS.open(JSON_TMP, "w");
        if (!wf) {
            http.end();
            setApiStatus(ApiState::ParseError, ApiAuth::Anonymous, code, size, "FS open failed");
            return false;
        }
        size = (int)streamBodyToFile(http, chunked, size, wf);
        wf.close();
        http.end();
    }   // TLS context freed here

    Serial.printf("[AdsbLive] body %d bytes heap=%u\n", size, ESP.getFreeHeap());

    File rf = LittleFS.open(JSON_TMP, "r");
    if (!rf) {
        setApiStatus(ApiState::ParseError, ApiAuth::Anonymous, code, size, "FS read failed");
        return false;
    }

    // Envelope is {"ac":[ {...}, ... ], "total":N, "now":..}. Seek to the array.
    int c = -1;
    if (skipPastToken(rf, "\"ac\":")) {
        do { c = rf.read(); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    }
    if (c != '[') {
        rf.close();
        LittleFS.remove(JSON_TMP);
        setApiStatus(ApiState::NoData, ApiAuth::Anonymous, code, size, "No aircraft");
        return false;
    }

    // Only pull the fields we use out of each (~50-field) object — far less work
    // and heap per element than parsing the whole thing.
    JsonDocument filter;
    for (const char* k : { "hex", "flight", "lat", "lon", "alt_baro", "gs",
                           "track", "squawk", "seen_pos", "t" })
        filter[k] = true;

    JsonDocument elem;
    DeserializationError err{};
    while ((int)out.size() < MAX_AIRCRAFT) {
        do {
            c = rf.peek();
            if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') rf.read();
            else break;
        } while (true);
        if (c == ']' || c < 0) break;

        err = deserializeJson(elem, rf, DeserializationOption::Filter(filter));
        if (err) break;

        // Need a position to plot.
        if (elem["lat"].isNull() || elem["lon"].isNull()) continue;

        Aircraft ac{};
        const char* hex = elem["hex"] | "";
        if (hex[0] == '~') hex++;   // '~' marks non-ICAO (TIS-B); drop the prefix
        strncpy(ac.icao24, hex[0] ? hex : "??????", sizeof(ac.icao24) - 1);

        const char* cs = elem["flight"] | "";
        if (cs[0]) {
            strncpy(ac.callsign, cs, sizeof(ac.callsign) - 1);
            for (int i = strlen(ac.callsign) - 1; i >= 0 && ac.callsign[i] == ' '; --i)
                ac.callsign[i] = '\0';
        } else {
            strncpy(ac.callsign, ac.icao24, sizeof(ac.callsign) - 1);
        }

        // Aircraft type (e.g. "A320") shown where OpenSky put country — more use
        // for planespotting, and airplanes.live doesn't carry a country field.
        const char* type = elem["t"] | "";
        strncpy(ac.country, type, sizeof(ac.country) - 1);

        ac.lat = elem["lat"].as<float>();
        ac.lon = elem["lon"].as<float>();

        // alt_baro is feet, or the string "ground".
        JsonVariantConst ab = elem["alt_baro"];
        bool onGround = ab.is<const char*>() && strcmp(ab.as<const char*>(), "ground") == 0;
        ac.onGround = onGround;
        ac.altM     = (onGround || ab.isNull()) ? 0.0f : ab.as<float>() * 0.3048f;   // ft→m

        ac.speedMs = elem["gs"].isNull()    ? 0.0f : elem["gs"].as<float>() * 0.514444f;   // kt→m/s
        ac.heading = elem["track"].isNull() ? 0.0f : elem["track"].as<float>();

        const char* sq = elem["squawk"] | "";
        if (sq[0]) strncpy(ac.squawk, sq, sizeof(ac.squawk) - 1);

        // seen_pos is literally "seconds since this position" — our posAgeS.
        float age  = elem["seen_pos"].isNull() ? 0.0f : elem["seen_pos"].as<float>();
        ac.posAgeS = age < 0.0f ? 0.0f : (age > 60.0f ? 60.0f : age);

        out.push_back(ac);
    }
    rf.close();
    LittleFS.remove(JSON_TMP);

    if (err && out.empty()) {
        char msg[48];
        snprintf(msg, sizeof(msg), "JSON: %s", err.c_str());
        setApiStatus(ApiState::ParseError, ApiAuth::Anonymous, code, size, msg);
        return false;
    }

    char msg[48];
    snprintf(msg, sizeof(msg), "%d aircraft", (int)out.size());
    setApiStatus(ApiState::Ok, ApiAuth::Anonymous, code, size, msg);
    Serial.printf("[AdsbLive] Parsed %d aircraft%s\n", (int)out.size(), err ? " (partial)" : "");
    return true;
}
