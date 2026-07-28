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

// Which OpenSky tier the last request used — surfaced in the API status panel so
// the user can see at a glance whether their API key is configured and working.
enum class ApiAuth : uint8_t {
    Anonymous,      // no credentials configured — OpenSky free/anonymous tier (400/day)
    Authenticated,  // credentials configured and a valid OAuth2 token obtained (4000/day)
    Failed          // credentials configured but authentication failed (bad key / token rejected)
};

struct ApiStatus {
    ApiState      state    = ApiState::Never;
    ApiAuth       auth     = ApiAuth::Anonymous;
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

// Sets the shared fetch-status snapshot. Public so alternate data sources
// (see adsblive.h) can report through the same poll icon / status panel.
void setApiStatus(ApiState st, ApiAuth auth, int httpCode, int bytes, const char* detail);

// Fetches all aircraft within a bounding box derived from (centerLat, centerLon)
// expanded by radiusKm, from the OpenSky Network. Returns true on success and
// populates `out`. (See adsblive.h for the alternate airplanes.live source; the
// active source is chosen by the "Data source" setting and dispatched in main.)
bool fetchAircraftOpenSky(float centerLat, float centerLon, float radiusKm,
                          std::vector<Aircraft>& out);
