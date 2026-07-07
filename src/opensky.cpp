#include "opensky.h"
#include "config.h"
#include "provisioning.h"

#include <M5Dial.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

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

bool fetchAircraft(float centerLat, float centerLon, float radiusKm,
                   std::vector<Aircraft>& out) {
    out.clear();

    // Obtain/refresh an OAuth2 token first. A newly acquired token moves us onto
    // the account's own quota bucket, so refreshToken() clears any backoff left
    // over from an anonymous 429 — otherwise entering credentials wouldn't take
    // effect until the (up to ~24 h) anonymous Retry-After elapsed.
    bool authed = ensureToken();

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

    // Give the TLS handshake its best shot at finding a contiguous block —
    // the map's PNG decoder buffer is the largest thing we can free on
    // demand (see compose()'s comment in map.cpp); a cheap no-op if it's
    // already released.
    M5Dial.Display.releasePngMemory();

    const char* hdrKeys[] = { "X-Rate-Limit-Retry-After-Seconds" };
    HTTPClient http;
    WiFiClientSecure client;   // must outlive http — see loop comment below
    client.setInsecure();      // Skip TLS cert verification for v1
    client.setTimeout(10);
    int code = -1, size = -1;

    // A negative HTTPClient code means the connection itself failed before
    // any server response — on this device that's almost always the TLS
    // handshake unable to allocate its buffers on a fragmented heap, not a
    // real network problem, and it's been observed to be transient (works
    // again a moment later). One retry after a short pause resolves most of
    // these. `client` is declared outside this loop deliberately: http holds
    // a pointer to it, and is still used (getString(), end()) after the loop
    // exits, so client must still be alive then — an earlier version scoped
    // client inside the loop, which destroyed it on `break`/loop-end while
    // http still pointed at it, corrupting memory the moment http.getString()
    // touched it (crashed on literally every successful fetch).
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) client.stop();   // close the failed attempt's socket first

        http.begin(client, url);
        if (authed)
            http.addHeader("Authorization", "Bearer " + _token);
        http.setTimeout(9000);
        http.collectHeaders(hdrKeys, 1);   // throttle hint, for a precise 429 backoff

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
        return false;
    }

    _blockUntilMs = 0;   // successful call — clear any prior backoff

    // Use a filter document to only pull the fields we need.
    // This keeps heap usage low — no PSRAM on ESP32-S3FN8.
    JsonDocument filter;
    JsonArray fs = filter["states"].to<JsonArray>();
    JsonArray fe = fs.add<JsonArray>();
    fe.add(true);   // [0]  icao24
    fe.add(true);   // [1]  callsign
    fe.add(true);   // [2]  origin_country
    fe.add(false);  // [3]  time_position   (skip)
    fe.add(false);  // [4]  last_contact    (skip)
    fe.add(true);   // [5]  longitude
    fe.add(true);   // [6]  latitude
    fe.add(true);   // [7]  baro_altitude
    fe.add(true);   // [8]  on_ground
    fe.add(true);   // [9]  velocity
    fe.add(true);   // [10] true_track
    fe.add(false);  // [11] vertical_rate  (skip)
    fe.add(false);  // [12] sensors        (skip)
    fe.add(false);  // [13] geo_altitude   (skip)
    fe.add(true);   // [14] squawk

    // getStream() hands back the raw socket and does NOT de-chunk a
    // "Transfer-Encoding: chunked" response — only getString()/writeToStream()
    // do that internally. OpenSky's authenticated /states/all response has no
    // Content-Length (getSize() == -1 above), so use getString() to be safe
    // against chunked encoding rather than risk feeding raw chunk framing to
    // the JSON parser.
    String body = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body,
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        char msg[48];
        snprintf(msg, sizeof(msg), "JSON: %s", err.c_str());
        setStatus(ApiState::ParseError, code, size, msg);
        Serial.printf("[OpenSky] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArray states = doc["states"].as<JsonArray>();
    if (states.isNull()) {
        // HTTP 200 but states:null. OpenSky returns this both for a genuinely
        // empty sky and for anonymous clients that are over their daily quota,
        // so flag it as a soft failure rather than success.
        setStatus(ApiState::NoData, code, size, "No data (states:null)");
        Serial.println("[OpenSky] states:null — empty sky or over quota");
        return false;
    }

    for (JsonArray s : states) {
        // Must have position data
        if (s[5].isNull() || s[6].isNull()) continue;

        Aircraft ac{};

        const char* icao  = s[0];
        const char* cs    = s[1].isNull() ? nullptr : s[1].as<const char*>();
        const char* cntry = s[2];

        strncpy(ac.icao24,   icao  ? icao  : "??????", sizeof(ac.icao24) - 1);
        strncpy(ac.country,  cntry ? cntry : "???",    sizeof(ac.country) - 1);

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

        const char* sq = s[14].isNull() ? nullptr : s[14].as<const char*>();
        if (sq) strncpy(ac.squawk, sq, sizeof(ac.squawk) - 1);

        out.push_back(ac);
    }

    char msg[48];
    snprintf(msg, sizeof(msg), "%d aircraft", (int)out.size());
    setStatus(ApiState::Ok, code, size, msg);
    Serial.printf("[OpenSky] Parsed %d aircraft\n", (int)out.size());
    return true;
}
