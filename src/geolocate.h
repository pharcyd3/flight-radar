#pragma once
#include <Arduino.h>

// Network location helpers, used to spare the user from looking up coordinates.
// Both require the device to already be connected to WiFi (STA mode) — they
// can't run while the captive portal's AP is up, so callers invoke them AFTER
// the portal closes and the device reconnects.

// Approximate home from the device's public IP (keyless HTTP call to ip-api.com).
// City-level accuracy — good enough to centre a 10–200 km radar, and thrown off
// by VPNs/CGNAT. Fills lat/lon and, if `place`/`placeLen` are given, a short
// "City, Country" label. Returns false on any network/parse failure.
bool ipGeolocate(float& lat, float& lon, char* place = nullptr, size_t placeLen = 0);

// Resolve a free-text place name ("Berlin", "Paris, France") to coordinates via
// OpenStreetMap Nominatim (keyless HTTPS, identifying User-Agent required by
// their usage policy). Fills lat/lon and an optional resolved-name label.
// Returns false if nothing matched or the request failed.
bool geocodeCity(const char* query, float& lat, float& lon,
                 char* place = nullptr, size_t placeLen = 0);
