#pragma once

// ── Product branding ─────────────────────────────────────────────────────────
// Display name is "Frank's Flight Radar" (boot splash lives in radar.cpp).
// SETUP_AP_SSID is the WiFi network the setup captive portal broadcasts.
#define SETUP_AP_SSID  "Franks-Flight-Radar-Setup"
#define PRODUCT_UA     "FranksFlightRadar/1.0 (ESP32 hobby project)"

// WiFi and home location are configured on first boot via the captive portal
// (connect to the "Franks-Flight-Radar-Setup" AP, open 192.168.4.1 in a
// browser). Hold the encoder button for 3 seconds to reset and re-run setup.

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
// airplanes.live is keyless with no credit/quota model (fair use ~1 req/s), so
// there's no tiering to adapt to — just a flat list of intervals. Default is a
// brisk 8 s, well inside fair use, for a lively-feeling radar; the slower options
// are there for anyone who'd rather poll less.
static const unsigned long REFRESH_OPTIONS_MS[] = { 5000UL, 8000UL, 15000UL, 30000UL };
static const int           REFRESH_OPTION_COUNT = 4;
static const int           REFRESH_DEFAULT      = 1;   // 8 s

// ── Aircraft cap ──────────────────────────────────────────────────────────────
// Upper bound on aircraft held/drawn per fetch. Bounds heap use for a busy area
// (the 200 km box over UK airspace can return well over 100), and — crucially on
// this no-PSRAM device — the result vector is reserve()d to this size once at
// startup so it never reallocates mid-heap later. A growing/moving vector was
// splitting the single large free region so no contiguous ~40 KB block remained
// for the next TLS handshake, stalling the feed after a big response.
static const int MAX_AIRCRAFT = 120;

// ── Dead-reckoning interpolation ───────────────────────────────────────────────
// Between polls (22 s apart on the authenticated cadence) a mark would otherwise
// teleport to its next reported position. Instead we advance it along its own
// heading at its reported ground speed on every redraw, so it glides. Two guards
// keep it honest: ignore anything slower than a crawl (parked/taxiing GPS jitter
// would just make stationary marks wander), and never extrapolate further than a
// bounded window past the last fetch — a skipped poll must not fling a stale mark
// across the screen.
static const float INTERP_MIN_SPEED_MS = 5.0f;    // ~10 kts — below this, don't move
static const float INTERP_MAX_S        = 120.0f;  // cap extrapolation at 2 min

// ── Position trails ─────────────────────────────────────────────────────────────
// Breadcrumb history for the *selected* aircraft: the last few reported positions,
// drawn as a fading polyline. Only the selected aircraft's trail is ever drawn,
// so we keep history for just a small pool of the most-recently-seen aircraft
// (LRU eviction) rather than all of them — the selected one is seen every poll,
// so it never gets evicted. Kept deliberately small: this is static RAM, and on
// this no-PSRAM board every KB counts against the single contiguous block the TLS
// handshake needs. Sizing it to MAX_AIRCRAFT (~9 KB) starved the fetch's HTTPS
// connection ("SSL - Memory allocation failed") — MAX_TRAILS × TRAIL_LEN is a
// fraction of that.
static const int TRAIL_LEN  = 8;
static const int MAX_TRAILS = 32;

// ── Filters ───────────────────────────────────────────────────────────────────
// Min-altitude filter thresholds, metres (Off, 1,000/5,000/10,000/20,000 ft).
static const float MIN_ALT_OPTIONS_M[]  = { 0.0f, 304.8f, 1524.0f, 3048.0f, 6096.0f };
static const int   MIN_ALT_OPTIONS_COUNT = 5;
