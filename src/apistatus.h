#pragma once
#include <Arduino.h>

// Outcome of the most recent flight-data fetch. Surfaced on-screen via the poll
// icon (red on failure) and the tap-to-view API status panel.
enum class ApiState : uint8_t {
    Never,       // no request attempted yet
    Ok,          // HTTP 200 with an aircraft list (count may still be 0)
    NoData,      // HTTP 200 but no aircraft in the response
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
// red poll icon. HTTP 200 with no aircraft is NOT a failure: it just means
// nothing matched the query area right now.
inline bool apiFailed(ApiState s) {
    return s == ApiState::HttpError ||
           s == ApiState::ParseError || s == ApiState::NetError;
}

// Snapshot of the most recent fetch outcome.
const ApiStatus& apiStatus();

// Sets the shared fetch-status snapshot — called by the fetch layer (adsblive.cpp)
// after every attempt, and read by the UI (poll icon colour, status panel).
void setApiStatus(ApiState st, int httpCode, int bytes, const char* detail);
