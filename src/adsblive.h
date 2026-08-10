#pragma once
#include "aircraft.h"
#include <Arduino.h>
#include <vector>

// Flight-data source: airplanes.live's community ADS-B REST API.
//
// A keyless, radius-based endpoint
//   https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}
// which maps directly to the radar's circular view and carries no credit/quota
// model — no account, no OAuth, no setup step — so it can be polled briskly
// (fair use ~1 req/s). Named-field JSON (hex/flight/lat/lon/alt_baro/gs/track/
// squawk/seen_pos/t).
//
// Streams the (potentially large) response to a flash scratch file and parses
// one aircraft at a time rather than buffering it whole — this no-PSRAM board's
// heap can't reliably hold a big response, the TLS context, and the JSON parser
// all at once. Reports through the shared apiStatus() snapshot (see apistatus.h).
// `keepIcao` (may be null/empty) is an aircraft that must survive the
// MAX_AIRCRAFT cap. The whole response is always parsed regardless of the cap
// — past it, adsblive.cpp reservoir-samples so the kept set stays spread
// across whatever was returned rather than converging on a subset. Without
// keepIcao a followed aircraft is just another candidate in that sample and
// could be dropped by chance, ending the follow for a reason unrelated to
// where it actually is; when set, it's excluded from the sample and always
// kept instead.
bool fetchAircraftAdsbLive(float centerLat, float centerLon, float radiusKm,
                           std::vector<Aircraft>& out,
                           const char* keepIcao = nullptr);
