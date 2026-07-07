#pragma once

// WiFi and OpenSky credentials are configured on first boot via the captive
// portal (connect to "FlightDial-Setup" AP, open 192.168.4.1 in a browser).
// Hold the encoder button for 3 seconds to reset and re-run setup.

// ── Home location (radar centre) ─────────────────────────────────────────────
// First-boot default only — central London. The actual home location is
// entered on the captive portal setup page (see provisioning.h: homeLat()/
// homeLon()) and persisted to flash, so the device can be relocated without
// reflashing.
#define DEFAULT_HOME_LAT   51.5f
#define DEFAULT_HOME_LON   -0.1f

// ── Zoom levels (km radius) ──────────────────────────────────────────────────
static const float ZOOM_STEPS[]  = { 10.0f, 25.0f, 50.0f, 100.0f, 200.0f };
static const int   ZOOM_COUNT    = 5;
static const int   ZOOM_DEFAULT  = 1;   // start at 25 km

// ── Polling ───────────────────────────────────────────────────────────────────
// Registered OpenSky accounts can poll every 5 s; stay conservative at 10 s.
#define REFRESH_INTERVAL_MS 5000UL
