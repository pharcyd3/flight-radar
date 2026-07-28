#include "routelookup.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static void copyCode(char* out, size_t len, const char* iata, const char* icao) {
    const char* src = (iata && iata[0]) ? iata : icao;
    if (!out || len == 0) return;
    if (!src) { out[0] = '\0'; return; }
    strncpy(out, src, len - 1);
    out[len - 1] = '\0';
}

bool fetchFlightRoute(const char* callsign,
                      float& originLat, float& originLon,
                      char* originCode, size_t originCodeLen,
                      float& destLat, float& destLon,
                      char* destCode, size_t destCodeLen) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!callsign || !callsign[0]) return false;

    // Trim whitespace and drop a leading '~' (airplanes.live's marker for a
    // non-ICAO/TIS-B synthetic hex-derived callsign — adsbdb won't know it).
    char cs[16];
    strncpy(cs, callsign, sizeof(cs) - 1);
    cs[sizeof(cs) - 1] = '\0';
    char* p = cs;
    while (*p == ' ' || *p == '~') p++;
    int n = strlen(p);
    while (n > 0 && p[n - 1] == ' ') p[--n] = '\0';
    if (!p[0]) return false;

    String url = "https://api.adsbdb.com/v0/callsign/";
    url += p;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    HTTPClient http;
    http.setTimeout(9000);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", PRODUCT_UA);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Route] adsbdb HTTP %d for %s\n", code, p);
        http.end();
        return false;
    }

    // Only pull the few fields we need out of the (small) response.
    JsonDocument filter;
    JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
    fr["origin"]["latitude"] = true;
    fr["origin"]["longitude"] = true;
    fr["origin"]["iata_code"] = true;
    fr["origin"]["icao_code"] = true;
    fr["destination"]["latitude"] = true;
    fr["destination"]["longitude"] = true;
    fr["destination"]["iata_code"] = true;
    fr["destination"]["icao_code"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[Route] adsbdb JSON error: %s\n", err.c_str());
        return false;
    }

    JsonVariantConst route = doc["response"]["flightroute"];
    JsonVariantConst o = route["origin"];
    JsonVariantConst d = route["destination"];
    if (o.isNull() || d.isNull() ||
        o["latitude"].isNull() || d["latitude"].isNull()) {
        Serial.printf("[Route] no route on file for %s\n", p);
        return false;
    }

    originLat = o["latitude"].as<float>();
    originLon = o["longitude"].as<float>();
    destLat   = d["latitude"].as<float>();
    destLon   = d["longitude"].as<float>();
    copyCode(originCode, originCodeLen, o["iata_code"] | (const char*)nullptr,
            o["icao_code"] | (const char*)nullptr);
    copyCode(destCode, destCodeLen, d["iata_code"] | (const char*)nullptr,
            d["icao_code"] | (const char*)nullptr);

    Serial.printf("[Route] %s: %s -> %s\n", p, originCode, destCode);
    return true;
}
