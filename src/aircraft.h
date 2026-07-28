#pragma once
#include <Arduino.h>

struct Aircraft {
    char  icao24[8];    // e.g. "4ca7b5"
    char  callsign[12]; // e.g. "RYR123  " (trailing spaces stripped)
    // ICAO aircraft *type* designator, e.g. "A320" — 4 chars plus slack.
    // (Named "country" from when the old data source carried one; airplanes.live
    // doesn't, and the type is more useful for planespotting. Sized down from 32
    // because MAX_AIRCRAFT of these are held in two reserved buffers, so every
    // byte here costs ~2 × MAX_AIRCRAFT of the contiguous heap the TLS
    // handshake needs. All writers bound themselves with sizeof().)
    char  country[12];
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
