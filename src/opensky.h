#pragma once
#include "aircraft.h"
#include <Arduino.h>
#include <vector>

// Outcome of the most recent OpenSky request. Surfaced on-screen via the poll
// icon (red on failure) and the tap-to-view API status panel.
enum class ApiState : uint8_t {
    Never,       // no request attempted yet
    Ok,          // HTTP 200 with a states array (count may still be 0)
    NoData,      // HTTP 200 but states:null — empty sky OR over anonymous quota
    HttpError,   // server replied with a non-200 status
    ParseError,  // response received but JSON failed to parse
    NetError     // connection/DNS/TLS failure — never reached the server
};

struct ApiStatus {
    ApiState      state    = ApiState::Never;
    int           httpCode = 0;      // last HTTP status (or negative HTTPClient err)
    int           bytes    = 0;      // Content-Length of last response, -1 if unknown
    unsigned long lastMs   = 0;      // millis() when the last attempt completed
    char          detail[48] = "";   // short human-readable message (e.g. "12 aircraft")
};

// True only for states where the request itself didn't succeed — drives the
// red poll icon. HTTP 200 with states:null is NOT a failure: it just means
// zero aircraft matched the bounding box right now (the emulator treats an
// empty states array the same way — as normal, not an error).
inline bool apiFailed(ApiState s) {
    return s == ApiState::HttpError ||
           s == ApiState::ParseError || s == ApiState::NetError;
}

// Snapshot of the most recent fetch outcome.
const ApiStatus& apiStatus();

// Fetches all aircraft within a bounding box derived from (centerLat, centerLon)
// expanded by radiusKm.  Returns true on success and populates `out`.
bool fetchAircraft(float centerLat, float centerLon, float radiusKm,
                   std::vector<Aircraft>& out);
