#pragma once

// ── Product branding ─────────────────────────────────────────────────────────
// Display name is "Frank's Flight Radar" (boot splash lives in radar.cpp).
// SETUP_AP_SSID is the WiFi network the setup captive portal broadcasts.
#define SETUP_AP_SSID  "Franks-Flight-Radar-Setup"
#define PRODUCT_UA     "FranksFlightRadar/1.0 (ESP32 hobby project)"

// WiFi and OpenSky credentials are configured on first boot via the captive
// portal (connect to the "Franks-Flight-Radar-Setup" AP, open 192.168.4.1 in a
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

// Extended zoom range available only in follow mode: the first five match
// ZOOM_STEPS (so entering follow keeps the current view), then continue out to a
// whole-earth view. Follow renders on the offline lo-fi vector map, so the extra
// levels cost nothing to draw; the data fetch stays bounded (see FETCH_MAX_KM) —
// we keep pulling the tracked aircraft's local traffic while the map zooms out.
// 20000 km ≈ half the earth's circumference, so the antipode sits at the plot
// edge: the whole globe fills the radar circle.
static const float FOLLOW_ZOOM_STEPS[] = {
    10.0f, 25.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1200.0f, 3000.0f, 8000.0f, 20000.0f
};
static const int   FOLLOW_ZOOM_COUNT   = 10;

// Cap on the OpenSky/airplanes.live fetch radius regardless of display zoom, so
// zooming the map out (in follow mode) never balloons the request. airplanes.live
// allows up to 250 nm (~463 km); OpenSky bounding boxes stay modest too.
static const float FETCH_MAX_KM = 460.0f;

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
// OpenSky daily budget: 4000 requests/day for a standard authenticated (OAuth2)
// client, 400/day anonymous (no client credentials). The default "Auto" refresh
// rate spreads whichever budget applies evenly across a full 24 h, so the feed
// stays live all day without ever hitting the quota:
//   authenticated: 86400 s / 4000 = 21.6 s  → poll every 22 s  (~3927 req/day)
//   anonymous:     86400 s /  400 = 216  s  → poll every 240 s (~360 req/day)
// The poll timer is measured from each fetch's *start* and a fetch itself blocks
// ~1-2 s, so the realised rate is always at or under these figures. The
// anonymous interval carries extra margin because exhausting the anonymous quota
// earns a punishing (~24 h) lockout. Auto adapts live: adding credentials via
// the setup portal switches it from the 240 s to the 22 s cadence immediately.
//
// The fixed 10/20/30 s options remain for manual override, but 10 s (8640/day)
// and 20 s (4320/day) both exceed the authenticated budget and will throttle
// before the day is out; only 30 s (2880/day) is safe all day on a fixed rate.
static const int           REFRESH_AUTO         = 0;         // index 0 — adapts to credentials
static const unsigned long REFRESH_AUTHED_MS    = 22000UL;   // 4000/day  → 21.6 s
static const unsigned long REFRESH_ANON_MS      = 240000UL;  // 400/day   → 216 s (+ margin)
static const unsigned long REFRESH_FIXED_MS[]   = { 10000UL, 20000UL, 30000UL };  // indices 1..3
static const int           REFRESH_OPTION_COUNT = 4;         // Auto + 3 fixed
static const int           REFRESH_DEFAULT       = REFRESH_AUTO;

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
// handshake needs. Sizing it to MAX_AIRCRAFT (~9 KB) starved OpenSky's HTTPS
// connection ("SSL - Memory allocation failed") — MAX_TRAILS × TRAIL_LEN is a
// fraction of that.
static const int TRAIL_LEN  = 8;
static const int MAX_TRAILS = 32;

// ── Filters ───────────────────────────────────────────────────────────────────
// Min-altitude filter thresholds, metres (Off, 1,000/5,000/10,000/20,000 ft).
static const float MIN_ALT_OPTIONS_M[]  = { 0.0f, 304.8f, 1524.0f, 3048.0f, 6096.0f };
static const int   MIN_ALT_OPTIONS_COUNT = 5;
