#pragma once
#include <Arduino.h>

// Looks up a flight's origin/destination airport via adsbdb.com's free, keyless
// callsign-route API: https://api.adsbdb.com/v0/callsign/<callsign>. Used by
// follow mode to draw the route line and auto-fit the zoom to it.
//
// One-shot and small (response is a few hundred bytes), so — unlike the
// recurring OpenSky/airplanes.live polls — this parses directly in RAM rather
// than streaming to flash; same pattern as geolocate.cpp's lookups.
//
// Returns false if the callsign isn't recognised or has no route on file (common
// for GA/military/some regional traffic, and for airplanes.live's non-ICAO
// "~hex" placeholder callsigns) — callers should just skip the route display,
// not treat it as an error.
bool fetchFlightRoute(const char* callsign,
                      float& originLat, float& originLon,
                      char* originCode, size_t originCodeLen,
                      float& destLat, float& destLon,
                      char* destCode, size_t destCodeLen);
