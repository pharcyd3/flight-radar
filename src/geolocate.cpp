#include "geolocate.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Both responses here are small (a few hundred bytes) and only ever fetched
// one-shot at setup time, while the heap is still fresh — so unlike the big
// OpenSky poll (which streams via flash to dodge fragmentation) these can safely
// parse straight from the stream into a modest JsonDocument.

static void copyPlace(char* place, size_t placeLen, const char* src) {
    if (!place || placeLen == 0) return;
    if (!src) { place[0] = '\0'; return; }
    strncpy(place, src, placeLen - 1);
    place[placeLen - 1] = '\0';
}

bool ipGeolocate(float& lat, float& lon, char* place, size_t placeLen) {
    if (WiFi.status() != WL_CONNECTED) return false;

    // ip-api.com's free endpoint is HTTP-only (no key, ~45 req/min) — plain
    // WiFiClient, no TLS handshake to squeeze onto the heap.
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(8000);
    // Ask only for the fields we use to keep the response tiny.
    if (!http.begin(client, "http://ip-api.com/json/?fields=status,lat,lon,city,country"))
        return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Geo] ip-api HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[Geo] ip-api JSON error: %s\n", err.c_str());
        return false;
    }

    if (strcmp(doc["status"] | "", "success") != 0) {
        Serial.println("[Geo] ip-api reported failure");
        return false;
    }

    lat = doc["lat"] | 0.0f;
    lon = doc["lon"] | 0.0f;
    if (lat == 0.0f && lon == 0.0f) return false;

    if (place && placeLen) {
        const char* city    = doc["city"]    | "";
        const char* country = doc["country"] | "";
        snprintf(place, placeLen, "%s%s%s", city, city[0] && country[0] ? ", " : "", country);
    }
    Serial.printf("[Geo] ip-api located %.4f, %.4f\n", lat, lon);
    return true;
}

// Percent-encode a query for a URL. Only alphanumerics survive unescaped; every
// other byte (spaces, commas, accents) becomes %XX — enough for place names.
static String urlEncode(const char* s) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool geocodeCity(const char* query, float& lat, float& lon,
                 char* place, size_t placeLen) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!query || !query[0]) return false;

    String url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q=";
    url += urlEncode(query);

    WiFiClientSecure client;
    client.setInsecure();               // skip cert verification (matches map/opensky)
    client.setTimeout(10);
    HTTPClient http;
    http.setTimeout(9000);
    if (!http.begin(client, url)) return false;
    // Nominatim's usage policy requires an identifying User-Agent.
    http.addHeader("User-Agent", PRODUCT_UA);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Geo] nominatim HTTP %d\n", code);
        http.end();
        return false;
    }

    // Response is a JSON array; keep only the first hit's lat/lon/display_name.
    JsonDocument doc;
    JsonDocument filter;
    filter[0]["lat"] = true;
    filter[0]["lon"] = true;
    filter[0]["display_name"] = true;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[Geo] nominatim JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArrayConst arr = doc.as<JsonArrayConst>();
    if (arr.isNull() || arr.size() == 0) {
        Serial.println("[Geo] nominatim: no match");
        return false;
    }

    // Nominatim returns lat/lon as strings.
    lat = atof(arr[0]["lat"] | "0");
    lon = atof(arr[0]["lon"] | "0");
    if (lat == 0.0f && lon == 0.0f) return false;

    copyPlace(place, placeLen, arr[0]["display_name"] | (const char*)nullptr);
    Serial.printf("[Geo] geocoded \"%s\" -> %.4f, %.4f\n", query, lat, lon);
    return true;
}
