#pragma once

// Runs WiFiManager captive portal on first boot (no stored credentials).
// Blocks until connected.  On subsequent boots, connects instantly with
// saved credentials.  Call once from setup() after M5Dial.begin().
void runProvisioning();

// Opens the captive portal to edit home lat/lon, saved favourites, and the
// OpenSky client_id/secret, without wiping stored WiFi credentials. Blocks
// until the user saves or the portal times out. Call from the settings menu.
void runLocationPortal();

// Call every loop() iteration — detects a 3-second encoder-button hold
// (factory reset) or a short press (returns true once to open settings).
void checkResetCombo();
bool settingsRequested();

// Wipes WiFi credentials, OpenSky login, home location, and saved favourites,
// then reboots into first-boot provisioning. Does not return. Used by both
// the encoder-button hold combo and the "Factory Reset" settings menu item.
void factoryReset();

// OpenSky OAuth2 API-client credentials loaded from NVS (may be empty strings
// if not set — the fetch layer then falls back to anonymous access). Created at
// opensky-network.org → Account → API Client.
const char* openskyClientId();
const char* openskyClientSecret();

// Sets and persists the OpenSky OAuth2 credentials without requiring the WiFi
// captive portal — used by the serial "SETCREDS:" debug command in main.cpp.
void setOpenSkyCredentials(const char* clientId, const char* clientSecret);

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
