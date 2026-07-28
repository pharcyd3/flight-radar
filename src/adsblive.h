#pragma once
#include "aircraft.h"
#include <Arduino.h>
#include <vector>

// Alternate data source: airplanes.live's community ADS-B REST API.
//
// Unlike OpenSky, this is a keyless, radius-based endpoint
//   https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}
// which maps directly to the radar's circular view and carries no credit/quota
// model — so it can be polled far more often (fair use ~1 req/s) with no OAuth.
// Named-field JSON (hex/flight/lat/lon/alt_baro/gs/track/squawk/seen_pos/t).
//
// Same contract and memory discipline as fetchAircraftOpenSky(): streams the
// (potentially large) body to a flash scratch file and parses one aircraft at a
// time, and reports through the shared apiStatus() snapshot. The active source is
// chosen by the "Data source" setting and dispatched in main.
bool fetchAircraftAdsbLive(float centerLat, float centerLon, float radiusKm,
                           std::vector<Aircraft>& out);
