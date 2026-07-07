#include "opensky.h"
#include "config.h"
#include "provisioning.h"

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

bool fetchAircraft(float centerLat, float centerLon, float radiusKm,
                   std::vector<Aircraft>& out) {
    out.clear();

    // Bounding box around home point
    float dLat = radiusKm / KM_PER_DEG_LAT;
    float dLon = radiusKm / (KM_PER_DEG_LAT * cosf(centerLat * M_PI / 180.0f));

    char url[256];
    snprintf(url, sizeof(url),
        "https://opensky-network.org/api/states/all"
        "?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
        centerLat - dLat, centerLon - dLon,
        centerLat + dLat, centerLon + dLon);

    Serial.printf("[OpenSky] GET %s\n", url);

    WiFiClientSecure client;
    client.setInsecure();   // Skip TLS cert verification for v1
    client.setTimeout(10);

    HTTPClient http;
    http.begin(client, url);
    if (openskyUser()[0])
        http.setAuthorization(openskyUser(), openskyPass());
    http.setTimeout(9000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OpenSky] HTTP error: %d\n", code);
        http.end();
        return false;
    }

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

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        Serial.printf("[OpenSky] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArray states = doc["states"].as<JsonArray>();
    if (states.isNull()) {
        Serial.println("[OpenSky] No states in response (sky may be empty)");
        return true;  // not an error — just no traffic
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

    Serial.printf("[OpenSky] Parsed %d aircraft\n", (int)out.size());
    return true;
}
