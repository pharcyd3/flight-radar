#pragma once
#include <Arduino.h>

struct Aircraft {
    char  icao24[8];    // e.g. "4ca7b5"
    char  callsign[12]; // e.g. "RYR123  " (trailing spaces stripped)
    char  country[32];  // e.g. "Ireland"
    char  squawk[6];    // e.g. "7700" or "" if unknown
    float lat;
    float lon;
    float altM;         // barometric altitude in metres (0 if unknown)
    float speedMs;      // ground speed m/s
    float heading;      // true track, degrees (0 = north)
    float posAgeS;      // how stale this position already was at fetch time (s)
    bool  onGround;

    // 7700 = general emergency, 7600 = radio failure, 7500 = hijacking
    bool isEmergency() const {
        return squawk[0] != '\0' &&
               (strcmp(squawk, "7700") == 0 ||
                strcmp(squawk, "7600") == 0 ||
                strcmp(squawk, "7500") == 0);
    }
};
