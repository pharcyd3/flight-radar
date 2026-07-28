#include <LittleFS.h>   // before M5Dial.h — mirrors map.cpp's include-order note
#include "opensky.h"
#include "config.h"
#include "provisioning.h"

#include <M5Dial.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "watchdog.h"

// OpenSky state-vector field indices
// [0]  icao24          string
// [1]  callsign        string or null
// [2]  origin_country  string
// [3]  time_position   int or null
// [4]  last_contact    int
// [5]  longitude       float or null
// [6]  latitude        float or null
// [7]  baro_altitude   float or null
// [8]  on_ground       bool
// [9]  velocity        float or null
// [10] true_track      float or null
// [11] vertical_rate   float or null  (skip)
// [12] sensors         array or null  (skip)
// [13] geo_altitude    float or null  (skip)
// [14] squawk          string or null

static const float KM_PER_DEG_LAT = 111.0f;

// Most recent fetch outcome — read by the UI (poll icon colour + status panel).
static ApiStatus _status;
const ApiStatus& apiStatus() { return _status; }

static void setStatus(ApiState st, int http, int bytes, const char* detail) {
    _status.state    = st;
    _status.httpCode = http;
    _status.bytes    = bytes;
    _status.lastMs   = millis();
    strncpy(_status.detail, detail, sizeof(_status.detail) - 1);
    _status.detail[sizeof(_status.detail) - 1] = '\0';
}

// Public setter used by alternate data sources (adsblive.cpp) to report through
// the same status snapshot the UI reads.
void setApiStatus(ApiState st, ApiAuth auth, int http, int bytes, const char* detail) {
    setStatus(st, http, bytes, detail);
    _status.auth = auth;
}

// ── OAuth2 client-credentials ─────────────────────────────────────────────────
// OpenSky retired HTTP Basic Auth; the REST API now requires a Bearer token
// obtained from the client_id/client_secret via the OAuth2 token endpoint.
// Tokens live ~30 min, so we cache and refresh a minute before expiry.

static const char* TOKEN_URL =
    "https://auth.opensky-network.org/auth/realms/opensky-network/"
    "protocol/openid-connect/token";

static String        _token;                 // current Bearer access token (JWT)
static unsigned long _tokenExpiryMs = 0;     // millis() when it should be refreshed
static unsigned long _blockUntilMs  = 0;     // honour a 429 Retry-After backoff

static bool refreshToken() {
    const char* cid = openskyClientId();
    const char* sec = openskyClientSecret();
    if (!cid[0] || !sec[0]) return false;    // no credentials → stay anonymous

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);

    HTTPClient http;
    http.begin(client, TOKEN_URL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(9000);

    String body = "grant_type=client_credentials&client_id=";
    body += cid;
    body += "&client_secret=";
    body += sec;

    int code = http.POST(body);
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OpenSky] Token request failed: HTTP %d\n", code);
        http.end();
        _token = "";
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[OpenSky] Token JSON error: %s\n", err.c_str());
        return false;
    }

    const char* tok = doc["access_token"];
    long expires    = doc["expires_in"] | 1800;
    if (!tok || !tok[0]) {
        Serial.println("[OpenSky] Token missing from response");
        return false;
    }

    _token = tok;
    long margin    = expires > 60 ? expires - 60 : expires;   // refresh 60 s early
    _tokenExpiryMs = millis() + (unsigned long)margin * 1000UL;
    _blockUntilMs  = 0;   // fresh token → new quota bucket, drop anonymous backoff
    Serial.printf("[OpenSky] Got OAuth2 token (expires in %lds)\n", expires);
    return true;
}

// Ensures a valid token if credentials exist. Returns true when the caller
// should send a Bearer header, false for anonymous access.
static bool ensureToken() {
    if (!openskyClientId()[0]) return false;                   // anonymous
    if (_token.length() && (long)(millis() - _tokenExpiryMs) < 0) return true;
    return refreshToken();
}

// Reads the HTTP response body straight to a flash file using only a small
// fixed stack buffer — no large heap allocation. This is why we can't just use
// getString() or writeToStream(): both need a big contiguous buffer (getString
// holds the whole body; writeToStream mallocs a 4 KB block) that the fragmented,
// no-PSRAM heap can't provide for the big 200 km response — they truncated or
// even crashed the device. De-chunks a Transfer-Encoding: chunked response on
// the fly (getStreamPtr() delivers the raw body with chunk framing intact). A
// 9 s deadline bounds every read so a stalled socket can't hang the loop.
static size_t streamBodyToFile(HTTPClient& http, bool chunked, int contentLen, File& f) {
    Stream* stream = http.getStreamPtr();
    uint8_t buf[512];
    unsigned long deadline = millis() + 9000;
    size_t total = 0;

    if (chunked) {
        while (millis() < deadline) {
            String hdr = stream->readStringUntil('\n');   // "<hexsize>\r"
            hdr.trim();
            if (hdr.isEmpty()) {                            // blank line / timeout
                if (!http.connected()) break;
                continue;
            }
            long n = strtol(hdr.c_str(), nullptr, 16);
            if (n <= 0) break;                             // 0-size chunk = end of body
            while (n > 0 && millis() < deadline) {
                int want = n < (long)sizeof(buf) ? (int)n : (int)sizeof(buf);
                int got  = stream->readBytes(buf, want);
                if (got <= 0) { if (!http.connected()) return total; else continue; }
                f.write(buf, got);
                total += got;
                n -= got;
            }
            stream->readBytes(buf, 2);                     // consume chunk's trailing CRLF
        }
    } else {
        long n = contentLen;                               // -1 → read until closed
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

// Advances `f` to just past the first occurrence of `token`. Returns false if
// EOF is hit first. Used to seek to the "states": value without loading the
// document — the token is distinctive enough that it can't appear before the
// real one in OpenSky's {"time":N,"states":[...]} envelope.
static bool skipPastToken(File& f, const char* token) {
    size_t i = 0, n = strlen(token);
    int c;
    while ((c = f.read()) >= 0) {
        if (c == (uint8_t)token[i]) { if (++i == n) return true; }
        else                        { i = (c == (uint8_t)token[0]) ? 1 : 0; }
    }
    return false;
}

bool fetchAircraftOpenSky(float centerLat, float centerLon, float radiusKm,
                          std::vector<Aircraft>& out) {
    out.clear();

    // Obtain/refresh an OAuth2 token first. A newly acquired token moves us onto
    // the account's own quota bucket, so refreshToken() clears any backoff left
    // over from an anonymous 429 — otherwise entering credentials wouldn't take
    // effect until the (up to ~24 h) anonymous Retry-After elapsed.
    bool authed = ensureToken();

    // Record which tier this request is on for the status panel. Credentials
    // present + a token obtained = Authenticated (key valid); credentials
    // present but no token = Failed (bad key); no credentials = Anonymous. A
    // token accepted here can still be rejected mid-request (401) — that path
    // downgrades this to Failed below.
    bool credsProvided = openskyClientId()[0] != '\0';
    _status.auth = !credsProvided ? ApiAuth::Anonymous
                 : authed         ? ApiAuth::Authenticated
                                  : ApiAuth::Failed;

    // Respect a prior 429 Retry-After — don't hammer while throttled.
    if (_blockUntilMs && (long)(millis() - _blockUntilMs) < 0) {
        unsigned long wait = (_blockUntilMs - millis()) / 1000UL;
        char msg[48];
        snprintf(msg, sizeof(msg), "Rate limited, retry %lus", wait);
        setStatus(ApiState::HttpError, 429, 0, msg);
        Serial.printf("[OpenSky] %s (skipping request)\n", msg);
        return false;
    }

    // Bounding box around home point
    float dLat = radiusKm / KM_PER_DEG_LAT;
    float dLon = radiusKm / (KM_PER_DEG_LAT * cosf(centerLat * M_PI / 180.0f));

    char url[256];
    snprintf(url, sizeof(url),
        "https://opensky-network.org/api/states/all"
        "?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
        centerLat - dLat, centerLon - dLon,
        centerLat + dLat, centerLon + dLon);

    Serial.printf("[OpenSky] GET %s  (%s, heap=%u)\n", url, authed ? "authed" : "anonymous",
                  ESP.getFreeHeap());

    // NOTE: we used to call M5Dial.Display.releasePngMemory() here to free the
    // map's ~45 KB PNG-decoder buffer before the TLS handshake. That was the
    // root cause of "maps only load at one zoom level": once freed, the decoder
    // could never be re-allocated for a later map compose, because the largest
    // contiguous free block on this (no-PSRAM) heap is only ~31 KB — smaller
    // than the single ~45 KB allocation pngle needs. Map compose and OpenSky
    // polls never run at the same time (both block this loop), so the decoder
    // is allocated once at boot (map.cpp begin() priming, while a 45 KB block
    // still exists) and kept resident for the life of the program. See
    // map.cpp's compose() note. TLS still gets enough contiguous room here.

    // The response body is streamed to this flash scratch file and parsed only
    // AFTER the TLS connection is torn down (see the scope note below).
    static const char* JSON_TMP = "/osky.json";
    int  code = -1, size = -1;
    bool chunked = false;

    // ── HTTP request + body-to-flash, in its own scope ───────────────────────
    // The WiFiClientSecure and HTTPClient live only inside this block, so both
    // are destroyed at the closing brace — freeing the ~40 KB mbedTLS context
    // BEFORE we parse. On this fragmented, no-PSRAM heap the TLS context and a
    // large filtered JSON document can't both fit at once: with TLS still up the
    // parser had only ~24 KB and the big 200 km response failed with NoMemory
    // (or a hard OOM crash). Freeing it explicitly mid-function via client.stop()
    // instead corrupted the *next* handshake (every subsequent fetch then failed
    // with a TLS start error), so we rely on clean RAII destruction at scope
    // exit. The body is safely on flash by then, so nothing is lost.
    {
        const char* hdrKeys[] = { "X-Rate-Limit-Retry-After-Seconds", "Transfer-Encoding" };
        HTTPClient       http;
        WiFiClientSecure client;   // must outlive http within this block
        client.setInsecure();      // Skip TLS cert verification for v1
        client.setTimeout(10);

        // A negative HTTPClient code means the connection itself failed before
        // any server response — on this device that's almost always the TLS
        // handshake unable to allocate its buffers on a fragmented heap, not a
        // real network problem, and it's transient. One retry after a short
        // pause resolves most of these.
        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt > 0) client.stop();   // close the failed attempt's socket first
            watchdogFeed();   // the TLS handshake + GET below blocks — keep WDT happy

            http.begin(client, url);
            if (authed)
                http.addHeader("Authorization", "Bearer " + _token);
            http.setTimeout(9000);
            http.collectHeaders(hdrKeys, 2);   // 429 retry hint + Transfer-Encoding

            code = http.GET();
            size = http.getSize();   // Content-Length, or -1 if not provided
            if (code >= 0) break;

            Serial.printf("[OpenSky] HTTP %d (heap=%u) — retrying once\n", code, ESP.getFreeHeap());
            http.end();
            delay(250);
        }

        Serial.printf("[OpenSky] HTTP %d  (Content-Length %d)\n", code, size);

        if (code != HTTP_CODE_OK) {
            char msg[48];
            if (code < 0) {              // HTTPClient negative == connection/TLS failure
                snprintf(msg, sizeof(msg), "Net error %d", code);
                setStatus(ApiState::NetError, code, size, msg);
            } else if (code == HTTP_CODE_UNAUTHORIZED) {
                // Token expired/invalid — drop it so the next cycle re-authenticates.
                _token = "";
                _tokenExpiryMs = 0;
                _status.auth = ApiAuth::Failed;   // key present but rejected by the server
                snprintf(msg, sizeof(msg), "Auth failed (401)");
                setStatus(ApiState::HttpError, code, size, msg);
            } else if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
                long retry = http.header("X-Rate-Limit-Retry-After-Seconds").toInt();
                if (retry <= 0) retry = 60;
                _blockUntilMs = millis() + (unsigned long)retry * 1000UL;
                snprintf(msg, sizeof(msg), "Rate limited (%lds)", retry);
                setStatus(ApiState::HttpError, code, size, msg);
            } else {
                snprintf(msg, sizeof(msg), "HTTP %d", code);
                setStatus(ApiState::HttpError, code, size, msg);
            }
            Serial.printf("[OpenSky] %s\n", msg);
            http.end();
            return false;   // client + http destroyed by scope exit
        }

        _blockUntilMs = 0;   // successful call — clear any prior backoff
        chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");

        // Stream the body to flash with a small fixed buffer (streamBodyToFile
        // de-chunks on the fly). getString()/writeToStream() both need a big
        // contiguous heap block this device can't provide for the 200 km
        // response and truncated ("IncompleteInput" — the "incomplete data" seen
        // only at the most-zoomed-out level) or crashed. Going via flash keeps
        // the raw body out of RAM entirely.
        File wf = LittleFS.open(JSON_TMP, "w");
        if (!wf) {
            http.end();
            setStatus(ApiState::ParseError, code, size, "FS open failed");
            Serial.println("[OpenSky] JSON scratch open failed");
            return false;
        }
        size = (int)streamBodyToFile(http, chunked, size, wf);   // de-chunked body size
        wf.close();
        http.end();
    }   // ← WiFiClientSecure + HTTPClient destroyed here; ~40 KB TLS context freed

    Serial.printf("[OpenSky] body %d bytes (chunked=%d) heap=%u\n",
                  size, chunked, ESP.getFreeHeap());

    // Parse the "states" array one aircraft at a time, straight from the flash
    // file. Building the whole (even filtered) document in RAM allocated a large
    // block for a big 200 km response that fragmented the heap so badly the NEXT
    // TLS handshake failed with a start error. Streaming element-by-element keeps
    // peak heap to a single ~200-byte state vector plus the growing result
    // vector, so memory stays tiny and stable regardless of response size — and
    // no per-field filter is needed since each element is parsed on its own.
    File rf = LittleFS.open(JSON_TMP, "r");
    if (!rf) {
        setStatus(ApiState::ParseError, code, size, "FS read failed");
        Serial.println("[OpenSky] JSON scratch read failed");
        return false;
    }

    // Read the envelope's server "time" (unix seconds) first — it precedes
    // "states" in {"time":<n>,"states":[...]} — so we can work out how stale each
    // aircraft's position already was when the snapshot was taken, and pre-advance
    // it by that age (otherwise interpolation extrapolates from a stale point and
    // the mark visibly jumps backward when the next, differently-aged, poll lands).
    long serverTime = 0;
    if (skipPastToken(rf, "\"time\":")) {
        int ch;
        while ((ch = rf.read()) >= 0 && ch >= '0' && ch <= '9')
            serverTime = serverTime * 10 + (ch - '0');
    }

    // Seek to the value of "states" and check it's an array (not null). The
    // envelope is {"time":<n>,"states":[[...],...]} or "states":null.
    int c = -1;
    if (skipPastToken(rf, "\"states\":")) {
        do { c = rf.read(); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    }
    if (c != '[') {
        rf.close();
        LittleFS.remove(JSON_TMP);
        // states:null (or missing) — genuinely empty sky OR over anonymous quota.
        setStatus(ApiState::NoData, code, size, "No data (states:null)");
        Serial.println("[OpenSky] states:null — empty sky or over quota");
        return false;
    }

    // Now positioned just after the array '['. Deserialize one state vector at a
    // time; between elements skip whitespace/commas, and stop at the closing ']'.
    JsonDocument elem;                        // reused each iteration (one vector)
    DeserializationError err{};
    while ((int)out.size() < MAX_AIRCRAFT) {
        do {
            c = rf.peek();
            if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') rf.read();
            else break;
        } while (true);
        if (c == ']' || c < 0) break;         // end of array / EOF

        err = deserializeJson(elem, rf);      // parses exactly one [ ... ] element
        if (err) break;

        JsonArrayConst s = elem.as<JsonArrayConst>();
        if (s.isNull() || s[5].isNull() || s[6].isNull()) continue;   // need position

        Aircraft ac{};
        const char* icao  = s[0];
        const char* cs    = s[1].isNull() ? nullptr : s[1].as<const char*>();
        const char* cntry = s[2];
        strncpy(ac.icao24,  icao  ? icao  : "??????", sizeof(ac.icao24) - 1);
        strncpy(ac.country, cntry ? cntry : "???",    sizeof(ac.country) - 1);
        if (cs) {
            strncpy(ac.callsign, cs, sizeof(ac.callsign) - 1);
            // Trim trailing spaces OpenSky pads into callsigns
            for (int i = strlen(ac.callsign) - 1; i >= 0 && ac.callsign[i] == ' '; --i)
                ac.callsign[i] = '\0';
        } else {
            strncpy(ac.callsign, ac.icao24, sizeof(ac.callsign) - 1);
        }
        ac.lon      = s[5].as<float>();
        ac.lat      = s[6].as<float>();
        ac.altM     = s[7].isNull() ? 0.0f : s[7].as<float>();
        ac.onGround = s[8].as<bool>();
        ac.speedMs  = s[9].isNull() ? 0.0f : s[9].as<float>();
        ac.heading  = s[10].isNull() ? 0.0f : s[10].as<float>();
        // Field [3] time_position: when this position was actually measured.
        // Its age (serverTime - time_position) is how far behind "now" it is; the
        // renderer pre-advances the mark by this so interpolation starts current.
        long tp = s[3].isNull() ? 0 : s[3].as<long>();
        float age = (serverTime > 0 && tp > 0) ? (float)(serverTime - tp) : 0.0f;
        ac.posAgeS = age < 0.0f ? 0.0f : (age > 60.0f ? 60.0f : age);   // clamp 0..60s
        const char* sq = s[14].isNull() ? nullptr : s[14].as<const char*>();
        if (sq) strncpy(ac.squawk, sq, sizeof(ac.squawk) - 1);
        out.push_back(ac);
    }
    rf.close();
    LittleFS.remove(JSON_TMP);

    // A parse error with nothing collected is a real failure; a hiccup after we
    // already have aircraft (e.g. a truncated tail) still yields a usable frame.
    if (err && out.empty()) {
        char msg[48];
        snprintf(msg, sizeof(msg), "JSON: %s", err.c_str());
        setStatus(ApiState::ParseError, code, size, msg);
        Serial.printf("[OpenSky] JSON error: %s (%d bytes)\n", err.c_str(), size);
        return false;
    }

    char msg[48];
    snprintf(msg, sizeof(msg), "%d aircraft", (int)out.size());
    setStatus(ApiState::Ok, code, size, msg);
    Serial.printf("[OpenSky] Parsed %d aircraft%s\n", (int)out.size(),
                  err ? " (partial)" : "");
    return true;
}
