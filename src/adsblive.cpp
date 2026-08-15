// LittleFS.h must be included before M5Dial.h (which pulls in M5GFX): M5GFX
// only compiles in its LittleFS-aware drawPngFile() overload if LittleFS's
// include guard is already defined by the time it's processed (see map.cpp).
#include <LittleFS.h>
#include "adsblive.h"
#include "apistatus.h"  // shared ApiStatus + setApiStatus()
#include "config.h"

#include <M5Dial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "watchdog.h"
#include <esp_random.h>

static const float KM_PER_NM = 1.852f;

// ── Body-to-flash streaming: keep the (potentially large) response out of the
// fragmented, no-PSRAM heap by streaming it straight to flash instead of
// buffering it in RAM. ──

static size_t streamBodyToFile(HTTPClient& http, bool chunked, int contentLen, File& f) {
    Stream* stream = http.getStreamPtr();
    uint8_t buf[512];
    // Hard ceiling on how long the body may take to arrive. This was 9 s, which
    // is longer than the whole refresh interval: a response that stalled
    // mid-transfer held the feed "in flight" for nine seconds, so the poll icon
    // sat solid (no countdown) for most of a cycle and the aircraft set that
    // finally landed was a truncated, different-every-time subset. Failing fast
    // and letting the next scheduled poll retry is both quicker and steadier —
    // a healthy response of this size lands in 1-2 s.
    unsigned long deadline = millis() + BODY_READ_TIMEOUT_MS;
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
                           std::vector<Aircraft>& out, const char* keepIcao) {
    out.clear();

    // Fail fast if WiFi isn't actually associated yet — e.g. mid-reconnect
    // after a drop. Without this, HTTPClient/WiFiClientSecure still attempt
    // the connection and can block for the full timeout (up to ~9 s, twice
    // with the retry below) before giving up, freezing the whole UI for that
    // whole stretch since every touch/encoder input is handled synchronously
    // in the same loop() this call blocks. A dead radio fails in microseconds.
    if (WiFi.status() != WL_CONNECTED) {
        setApiStatus(ApiState::NetError, -1, 0, "WiFi not connected");
        return false;
    }

    // Radius param is nautical miles, capped at the API's 250 nm maximum.
    int radiusNm = (int)lroundf(radiusKm / KM_PER_NM);
    if (radiusNm < 1)   radiusNm = 1;
    if (radiusNm > 250) radiusNm = 250;

    // Payload grows with the square of the radius, and the API returns every
    // field for every aircraft. Measured over UK airspace: 54 nm returns ~13 KB,
    // but 216 nm (the 400 km zoom step) returns ~150 KB — which costs ~3 s to
    // stream to flash plus ~1.7 s to parse, so the poll icon sat showing "in
    // flight" for five seconds at a time and the data was stale before it landed.
    //
    // Almost all of that is thrown away: the display holds MAX_AIRCRAFT and now
    // keeps the nearest of them, so several hundred distant aircraft are parsed
    // only to be evicted. Capping the *query* radius gets the same picture for a
    // quarter of the bytes. It does mean the widest zoom shows traffic only
    // within this cap rather than out to the ring — a deliberate trade of
    // completeness at the outermost step for a responsive one.
    if (radiusNm > FETCH_MAX_RADIUS_NM) radiusNm = FETCH_MAX_RADIUS_NM;

    char url[128];
    snprintf(url, sizeof(url), "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
             centerLat, centerLon, radiusNm);
    Serial.printf("[AdsbLive] GET %s  (heap=%u)\n", url, ESP.getFreeHeap());

    static const char* JSON_TMP = "/adsb.json";
    int  code = -1, size = -1;
    bool chunked = false;

    // ── HTTP + body-to-flash in its own scope, so the ~40 KB TLS context is
    // freed (via RAII at scope exit) before we parse — this fragmented,
    // no-PSRAM heap can't hold the TLS context and the JSON parser at once. ──
    {
        const char* hdrKeys[] = { "Transfer-Encoding" };
        HTTPClient       http;
        WiFiClientSecure client;
        client.setInsecure();      // no cert verification for v1
        // Three DIFFERENT timeouts, easily confused — all three matter:
        //  - setTimeout() is Stream::setTimeout (NetworkClient does NOT override
        //    it), i.e. how long a single readBytes()/readStringUntil() waits for
        //    the next chunk of BODY, in milliseconds. This read 10 here, which
        //    is 10 ms, not the 10 s clearly intended: any link that paused even
        //    briefly mid-body had the read abandoned, truncating the response.
        //  - setConnectionTimeout() bounds establishing the TCP connection.
        //  - setHandshakeTimeout() bounds the TLS handshake and is in SECONDS.
        //    It defaults to 120 s, so without it a stalled handshake could hang
        //    for two minutes.
        // These are generous rather than tight on purpose: the fetch runs on a
        // background task now (aircraftfeed.h), so a slow request costs nothing
        // in UI responsiveness, and clipping a merely-slow connection just to
        // return early throws away a poll for no benefit.
        client.setTimeout(5000);             // ms, per-read of the body
        client.setConnectionTimeout(10000);  // ms, TCP connect
        client.setHandshakeTimeout(10);      // SECONDS, TLS handshake

        // One retry on a transient failure — a dropped SYN or a reset
        // connection is common on real WiFi and usually succeeds immediately
        // on a second attempt.
        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt > 0) client.stop();
            watchdogFeed();
            http.begin(client, url);
            http.addHeader("User-Agent", PRODUCT_UA);   // community API — identify politely
            http.setTimeout(10000);
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
            setApiStatus(code < 0 ? ApiState::NetError : ApiState::HttpError, code, size, msg);
            Serial.printf("[AdsbLive] %s\n", msg);
            http.end();
            return false;
        }

        chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
        File wf = LittleFS.open(JSON_TMP, "w");
        if (!wf) {
            http.end();
            setApiStatus(ApiState::ParseError, code, size, "FS open failed");
            return false;
        }
        size = (int)streamBodyToFile(http, chunked, size, wf);
        wf.close();
        http.end();
    }   // TLS context freed here

    Serial.printf("[AdsbLive] body %d bytes heap=%u\n", size, ESP.getFreeHeap());

    File rf = LittleFS.open(JSON_TMP, "r");
    if (!rf) {
        setApiStatus(ApiState::ParseError, code, size, "FS read failed");
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
        setApiStatus(ApiState::NoData, code, size, "No aircraft");
        return false;
    }

    // Only pull the fields we use out of each (~50-field) object — far less work
    // and heap per element than parsing the whole thing.
    JsonDocument filter;
    for (const char* k : { "hex", "flight", "lat", "lon", "alt_baro", "gs",
                           "track", "squawk", "seen_pos", "t", "r", "desc", "ownOp" })
        filter[k] = true;

    JsonDocument elem;
    DeserializationError err{};
    const bool wantKeep = (keepIcao && keepIcao[0]);
    bool       gotKeep  = false;

    // Once at the cap, keep the MAX_AIRCRAFT *nearest* the query centre.
    //
    // This replaced reservoir sampling (Algorithm R), which spread the kept set
    // evenly across everything returned. That looked better in principle, but
    // it re-rolls independently on every poll: over busy airspace a 400 km query
    // returns several hundred candidates for 80 slots, so each poll surfaced a
    // different random subset and traffic visibly popped in and out between
    // refreshes — the picture never settled. Nearest-N is deterministic, so an
    // aircraft that qualifies stays qualified from one poll to the next and only
    // leaves when it genuinely flies out of range. A stable, coherent picture of
    // the nearer traffic beats a uniformly-sampled one that flickers, and near
    // traffic is what this radar is for.
    //
    // Cost is O(candidates x MAX_AIRCRAFT) in the worst case, which sounds worse
    // than Algorithm R's O(1) but is a few tens of thousands of float compares —
    // utterly lost next to the ~1.7 s the JSON parse itself takes at this size.
    float d2Kept[MAX_AIRCRAFT];   // distance² (deg², planar) of each held slot
    float cosCLat = cosf(centerLat * 0.0174532925f);
    if (cosCLat < 0.05f) cosCLat = 0.05f;
    int keepSlot = -1;   // slot holding the followed aircraft, once placed
    int seen      = 0;   // candidates considered so far, including the initial fill

    while (true) {
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

        // Aircraft type (e.g. "A320") — airplanes.live doesn't carry a country
        // field, and the type is more useful for planespotting anyway.
        const char* type = elem["t"] | "";
        strncpy(ac.country, type, sizeof(ac.country) - 1);

        const char* reg = elem["r"] | "";
        strncpy(ac.reg, reg, sizeof(ac.reg) - 1);

        const char* desc = elem["desc"] | "";
        strncpy(ac.desc, desc, sizeof(ac.desc) - 1);

        const char* ownOp = elem["ownOp"] | "";
        strncpy(ac.ownOp, ownOp, sizeof(ac.ownOp) - 1);

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

        const bool isKeep = wantKeep && !gotKeep &&
                            strncmp(ac.icao24, keepIcao, sizeof(ac.icao24)) == 0;

        float dLat = ac.lat - centerLat;
        float dLon = (ac.lon - centerLon) * cosCLat;
        float d2   = dLat * dLat + dLon * dLon;

        if ((int)out.size() < MAX_AIRCRAFT) {
            if (isKeep) keepSlot = (int)out.size();
            d2Kept[out.size()] = d2;
            out.push_back(ac);
            if (isKeep) gotKeep = true;
        } else if (isKeep) {
            // The followed aircraft always gets a seat, displacing whichever
            // slot last happened to hold it (or the farthest one the first time
            // this fires past the fill phase) — it must survive regardless of
            // how far out it drifts.
            int slot = keepSlot;
            if (slot < 0) {
                slot = 0;
                for (int j = 1; j < MAX_AIRCRAFT; ++j)
                    if (d2Kept[j] > d2Kept[slot]) slot = j;
            }
            out[slot]    = ac;
            d2Kept[slot] = d2;
            keepSlot     = slot;
            gotKeep      = true;
        } else {
            // Evict the farthest held aircraft if this one is nearer. The
            // protected follow slot is skipped so it can never be evicted.
            int worst = -1;
            for (int j = 0; j < MAX_AIRCRAFT; ++j) {
                if (j == keepSlot) continue;
                if (worst < 0 || d2Kept[j] > d2Kept[worst]) worst = j;
            }
            if (worst >= 0 && d2 < d2Kept[worst]) {
                out[worst]    = ac;
                d2Kept[worst] = d2;
            }
        }
        seen++;
    }
    rf.close();
    LittleFS.remove(JSON_TMP);

    // A mid-stream parse failure means the body was truncated: the array's
    // closing ']' is detected before deserialising, so `err` can only mean the
    // response was cut short. Publishing what was salvaged replaces a complete
    // picture with a fragment of it — observed live as 55 aircraft dropping to
    // 11 and back, which reads as traffic vanishing. Fail the poll instead and
    // the feed keeps the last good set (see aircraftfeed.cpp) until one
    // completes.
    if (err) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Truncated: %d ac", (int)out.size());
        setApiStatus(ApiState::ParseError, code, size, msg);
        Serial.printf("[AdsbLive] truncated body, discarding %d aircraft\n", (int)out.size());
        out.clear();
        return false;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "%d aircraft", (int)out.size());
    setApiStatus(ApiState::Ok, code, size, msg);
    Serial.printf("[AdsbLive] Parsed %d aircraft\n", (int)out.size());
    return true;
}
