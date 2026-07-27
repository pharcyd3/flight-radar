#pragma once
#include "aircraft.h"
#include <vector>

class RadarDisplay;

// Loads all settings from NVS (called at boot and on entering the menu).
void loadSettings();

// Runs the on-device settings menu until the user backs out or it times out.
// Renders as a floating panel over a live backdrop, so the caller passes
// through everything needed to keep drawing the current radar frame
// underneath it (main.cpp's own draw arguments).
void runSettings(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                  float homeLat, float homeLon, float radiusKm, int zoomIdx,
                  unsigned long lastUpdateMs, bool fetching);

bool buzzOnEmergency();

// Flight label visibility: 0 = Off, 1 = Selected only, 2 = All.
int flightLabels();

// Colour theme index — see THEMES[] in radar.cpp (0=Radar, 1=Amber, 2=Ocean, 3=Neon).
int activeTheme();

// Units: 0 = Imperial (ft / kts), 1 = Metric (m / km/h).
int activeUnits();

// Traffic filter: 0 = Airborne only, 1 = All (including on-ground).
int trafficFilter();

// Minimum-altitude filter in metres (0 = off/no filter). Aircraft with a
// known positive altitude below this are hidden; on-ground/unknown-altitude
// aircraft are unaffected (governed by trafficFilter() instead).
float minAltitudeM();

bool showTrails();
bool showRings();

// Map background mode. Full = OSM raster tiles; Lo-fi = embedded vector coastlines/
// borders/rivers + city labels (offline, themed lines); Off = plain background.
enum MapMode { MAP_FULL = 0, MAP_LOFI = 1, MAP_OFF = 2 };
int mapMode();

// Auto-refresh interval, driven by the user-selectable "Refresh rate" setting.
unsigned long refreshIntervalMs();
