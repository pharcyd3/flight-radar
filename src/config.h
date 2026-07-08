#pragma once

// WiFi and OpenSky credentials are configured on first boot via the captive
// portal (connect to "FlightDial-Setup" AP, open 192.168.4.1 in a browser).
// Hold the encoder button for 3 seconds to reset and re-run setup.

// ── Home location (radar centre) ─────────────────────────────────────────────
// First-boot default only (central London, deliberately low-precision — this
// is never meant to be anyone's real address). The actual home location is
// entered on the captive portal setup page (see provisioning.h: homeLat()/
// homeLon()) and persisted to flash, so the device can be relocated without
// reflashing.
#define DEFAULT_HOME_LAT   51.5f
#define DEFAULT_HOME_LON   -0.1f

// ── Zoom levels (km radius) ──────────────────────────────────────────────────
static const float ZOOM_STEPS[]  = { 10.0f, 25.0f, 50.0f, 100.0f, 200.0f };
static const int   ZOOM_COUNT    = 5;
static const int   ZOOM_DEFAULT  = 1;   // start at 25 km

// ── Rotary encoder ────────────────────────────────────────────────────────────
// M5Dial's rotary reports 4 raw quadrature ticks per physical detent/click.
// Every encoder-driven UI must convert to detents via encoderDetent() before
// comparing readings, or one click registers as up to 4 steps.
static const long ENC_TICKS_PER_DETENT = 4;

// Converts a raw quadrature tick count to a detent count using FLOOR
// division — plain C++ `/` truncates toward zero, which is asymmetric for
// negative values (e.g. -3/4 truncates to 0, not -1). That silently eats one
// rotation direction's first click at each detent boundary and then jumps
// two steps on the next click, while the other direction behaves correctly —
// exactly a "one way is fine, the other is erratic" symptom.
static inline long encoderDetent(long rawTicks) {
    long q = rawTicks / ENC_TICKS_PER_DETENT;
    long r = rawTicks % ENC_TICKS_PER_DETENT;
    if (r != 0 && r < 0) q--;
    return q;
}

// This hardware's raw encoder count has been observed to spontaneously
// bounce by a tick with no further physical input — including reverting
// back several hundred ms after a real click, on its own (e.g. raw 0 -> -1
// on a real click, then -1 -> 0 again ~500ms later with nobody touching it).
// Real clicks have also been observed to produce as few as 1 raw tick, the
// same magnitude as the noise, so the two can't be told apart by size —
// only by timing: EncoderDebouncer (encoder_debounce.h) requires a reading
// to hold steady for this long before committing to it at all. The zoom
// window is long enough to comfortably outlast the observed ~500ms bounce
// (verified: no visible flip-back). The menu window is shorter — a wrong
// menu step is low-stakes and trivially corrected by continuing to rotate,
// and 1s per step felt sluggish for something that low-stakes.
static const unsigned long ENC_STABLE_MS_ZOOM     = 1000UL;
static const unsigned long ENC_STABLE_MS_MENU     = 350UL;

// ── Polling ───────────────────────────────────────────────────────────────────
// OpenSky credit budget: 4000/day for a standard authenticated (OAuth2)
// client, 8000/day only if the account is also an active ADS-B feeder,
// 400/day anonymous. User-selectable via Settings > Refresh rate, but only
// the 30 s option comfortably fits a standard authenticated budget for a
// full day: 10 s = 8640 req/day (exceeds even the feeder tier — throttles
// after ~11h), 20 s = 4320 req/day (still slightly over standard), 30 s =
// 2880 req/day (safely under, all day). Anonymous access (no client
// credentials) exhausts its 400/day in under 2h at any of these intervals —
// set up an API client at opensky-network.org for a reliable feed.
static const unsigned long REFRESH_OPTIONS_MS[]  = { 10000UL, 20000UL, 30000UL };
static const int           REFRESH_OPTIONS_COUNT = 3;
static const int           REFRESH_DEFAULT       = 2;   // 30 s — the only option safe all day

// ── Filters ───────────────────────────────────────────────────────────────────
// Min-altitude filter thresholds, metres (Off, 1,000/5,000/10,000/20,000 ft).
static const float MIN_ALT_OPTIONS_M[]  = { 0.0f, 304.8f, 1524.0f, 3048.0f, 6096.0f };
static const int   MIN_ALT_OPTIONS_COUNT = 5;
