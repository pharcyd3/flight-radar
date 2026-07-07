#pragma once

// Runs WiFiManager captive portal on first boot (no stored credentials).
// Blocks until connected.  On subsequent boots, connects instantly with
// saved credentials.  Call once from setup() after M5Dial.begin().
void runProvisioning();

// Opens the same captive portal just to update the home lat/lon, without
// touching stored WiFi credentials. Blocks until the user saves or the
// portal times out. Call from the settings menu.
void runLocationPortal();

// Call every loop() iteration — detects a 3-second encoder-button hold
// (factory reset) or a short press (returns true once to open settings).
void checkResetCombo();
bool settingsRequested();

// Wipes WiFi credentials, OpenSky login, home location, and saved favourites,
// then reboots into first-boot provisioning. Does not return. Used by both
// the encoder-button hold combo and the "Factory Reset" settings menu item.
void factoryReset();

// OpenSky credentials loaded from NVS (may be empty strings if not set).
const char* openskyUser();
const char* openskyPass();

// Home location (radar centre), entered on the captive portal setup page and
// persisted to NVS. Falls back to DEFAULT_HOME_LAT/LON until first set.
float homeLat();
float homeLon();

// Makes (lat, lon) the active home location and persists it to flash.
// Used when the user picks a saved favourite from the settings menu.
void setHomeLocation(float lat, float lon);

// Up to 3 saved favourite locations (name + lat/lon), edited via the
// "Change Location" portal and selectable from the on-device Settings menu.
// An empty name means the slot hasn't been set yet.
static const int FAV_COUNT = 3;
const char* favName(int i);
float       favLat(int i);
float       favLon(int i);
