#pragma once

// ── Product branding ─────────────────────────────────────────────────────────
// Display name is "Frank's Flight Radar" (boot splash lives in radar.cpp).
// SETUP_AP_SSID is the WiFi network the setup captive portal broadcasts.
#define SETUP_AP_SSID  "Franks-Flight-Radar-Setup"
#define PRODUCT_UA     "FranksFlightRadar/1.0 (ESP32 hobby project)"
// Shown as the title of the web config page.
#define PRODUCT_NAME   "Frank's Flight Radar"

// ── Firmware version ─────────────────────────────────────────────────────────
// Bump on every release worth flashing. Purely a human-readable label now
// (shown in the web config page) — docs/changelog.md must carry a matching
// "## X.Y" entry or the CI build fails (see .github/workflows/docs.yml), so
// there's always release notes to go with it.
#define FIRMWARE_VERSION  "1.1"

// Firmware is flashed over USB via a browser (Web Serial + esp-web-tools),
// not downloaded and self-applied over WiFi — this no-PSRAM board doesn't
// have enough contiguous heap to run a TLS download and Update.begin()'s
// flash-write buffer at the same time (see docs/development.md). Hosted on
// the same GitHub Pages deploy as the docs site.
#define FLASH_PAGE_URL  "https://pharcyd3.github.io/flight-radar/flash/"

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
// The two widest steps show traffic out to FETCH_MAX_RADIUS_NM rather than to
// the edge of the ring — see the note there. This used to say 400 km was the
// widest step that still fetched everything it displayed (the API's own radius
// argument caps at 250 nm, and 400 km is 216 nm), which was true until response
// size at that radius turned out to be the thing making the widest views crawl.
static const float ZOOM_STEPS[]  = { 10.0f, 25.0f, 50.0f, 100.0f, 200.0f, 400.0f };
static const int   ZOOM_COUNT    = 6;
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

// This hardware's raw encoder count has been observed to spontaneously bounce
// by a tick with no physical input — including reverting several hundred ms
// after a real click (e.g. raw 0 -> -1 on a click, then -1 -> 0 again on its
// own). EncoderDebouncer (encoder_debounce.h) rejects that with hysteresis:
// a step is only emitted once the raw count moves a full detent from the last
// committed one, so a ±1-tick blip can never accumulate into a step. That
// replaced a "hold steady for N ms before committing" scheme, which rejected
// the same noise but charged its entire settling window (1 s for zoom) as
// latency on every real click.
//
// This is only a floor on how fast steps may be emitted — a burst quicker than
// a finger can physically click is noise, not a spin. Well below the ~80 ms of
// a fast human detent, so it never throttles genuine input.
static const unsigned long ENC_MIN_STEP_MS = 25UL;

// How long after the last zoom step to wait before re-fetching at the new
// radius. The fetch itself is off-thread (see aircraftfeed.h) so this costs no
// interactivity — it purely avoids firing one API request per intermediate
// level while the user spins through several in a row.
static const unsigned long ZOOM_FETCH_DEBOUNCE_MS = 350UL;

// How often the radar repaints while traffic is moving, i.e. the dead-reckoning
// animation's frame interval. 200 ms = 5 fps, which is what makes marks glide
// rather than step. Measure a real frame with the `PERF` serial command before
// lowering this: a full composite runs ~23 ms close in and ~33 ms at the 400 km
// step, so 5 fps is comfortably under a fifth of the loop's time. Everything
// else the UI does (touch, encoder) is sampled between frames, so pushing this
// too low would start eating the input-sampling gaps.
static const unsigned long ANIM_FRAME_MS = 200UL;

// The poll icon's sweep is ticked independently of, and much faster than, the
// radar frame above. It's a few pixels painted straight to the display rather
// than a full 115 KB composite, so it costs almost nothing — and at the frame
// rate alone the sweep would visibly step round in chunks instead of turning.
static const unsigned long POLL_ICON_TICK_MS = 50UL;

// Drop the aircraft set entirely if no fetch has succeeded for this long. A
// failed fetch deliberately keeps the last good data on screen (a transient
// blip shouldn't blank the radar), but past the dead-reckoning cap those marks
// are frozen and increasingly wrong, so showing nothing becomes the honest
// answer. Matches INTERP_MAX_S below.
static const unsigned long FEED_STALE_CLEAR_MS = 120000UL;

// If a fetch has succeeded at least once since boot but then nothing lands
// for this long despite repeated retries, treat the feed as stuck rather
// than just quiet — most often heap fragmentation after long uptime (see
// docs/troubleshooting.md's "SSL - Memory allocation failed" entry) doesn't
// clear itself, so the device restarts for a clean heap rather than sitting
// silently broken for hours on an unattended shelf. Gated on "succeeded at
// least once" so this never masks a device that's never had working
// WiFi/internet at all — that's a different problem restarting won't fix.
static const unsigned long FEED_STUCK_RESTART_MS = 300000UL;   // 5 min

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
// — measured directly against the API, a 400 km query over populated airspace
// returned 280 — and, crucially on this no-PSRAM device, the result vector is
// reserve()d to this size once at startup so it never reallocates mid-heap
// later. A growing/moving vector was splitting the single large free region so
// no contiguous ~40 KB block remained for the next TLS handshake, stalling the
// feed after a big response.
// 80 rather than higher: two buffers of these are reserved up front (the UI's
// set and the feed's hand-off buffer), so the cap is paid twice in permanently
// committed heap, directly out of the contiguous block each TLS handshake
// needs — a smaller zoom (200 km over UK airspace) returns ~30, so 80 already
// keeps a wide margin there, but the widest 400 km step routinely exceeds it
// in busy airspace.
//
// When a fetch returns more than this, adsblive.cpp reservoir-samples rather
// than keeping strictly the nearest — an earlier "always keep nearest" policy
// meant a busy area's outer reaches were silently never shown at wide zoom,
// which read as the radar not actually scanning the whole circle. A followed
// aircraft is still protected explicitly regardless of the sample — see
// adsblive.h's keepIcao.
static const int MAX_AIRCRAFT = 80;

// Largest radius (nautical miles) actually requested from the API, regardless of
// how far out the display is zoomed. See the note in adsblive.cpp: response size
// scales with the square of this, and past ~120 nm the device spends longer
// fetching and parsing than the refresh interval itself. 120 nm ≈ 222 km.
static const int FETCH_MAX_RADIUS_NM = 100;

// How long the response body may take to arrive before the fetch is abandoned
// and left to the next scheduled poll. Must stay comfortably under the shortest
// refresh interval (5 s) or a stalled transfer keeps the feed permanently busy.
static const unsigned long BODY_READ_TIMEOUT_MS = 3500UL;

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

// Follow mode extrapolates for a different purpose, and needs a far longer cap.
//
// INTERP_MAX_S above bounds what's *drawn*: a stale mark must not be flung
// across the screen. But follow mode also uses dead reckoning to steer the
// fetch box itself, and there the failure mode is the opposite — if the box
// stops advancing during a data gap, the aircraft reappears hundreds of km
// beyond it and is never seen again, ending the follow. Long-haul routes cross
// oceans and remote airspace where community ADS-B coverage routinely drops out
// for 10-30 minutes, so at 2 min the box gave up almost immediately. A cruising
// airliner holds heading and speed very predictably, so extrapolating the
// search centre far longer is safe — worst case the box is a little off and the
// aircraft is picked up again on a later poll.
static const float FOLLOW_DR_MAX_S = 1800.0f;     // 30 min of fetch-box coasting

// How long the tracked aircraft must go unseen before the chase view says so.
// Longer than a couple of poll intervals, so an ordinary single missed poll
// doesn't flash a warning up and straight back down again.
static const unsigned long FOLLOW_LOST_NOTICE_MS = 20000UL;

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
static const int TRAIL_LEN  = 14;
static const int MAX_TRAILS = 28;

// Minimum spacing between recorded breadcrumbs, per aircraft.
//
// Points used to be recorded once per successful poll, which quietly tied the
// trail's time span to the refresh rate: at the 5-8 s cadence, TRAIL_LEN points
// covered barely a minute, which for a 200 km/h aircraft is ~2 km — about ten
// pixels at the 25 km zoom, i.e. entirely hidden under the aircraft mark's own
// halo. (It looked fine only while most fetches were failing, since that
// sampled positions minutes apart by accident.) Spacing the points by time
// instead makes the trail cover a consistent ~TRAIL_LEN x this window of
// flight — a bit over 2 minutes — whatever the refresh rate is set to.
static const unsigned long TRAIL_MIN_INTERVAL_MS = 10000UL;

// ── Battery ───────────────────────────────────────────────────────────────────
// Measured on hardware: the M5Dial exposes no battery voltage to the ESP32.
//   - M5Unified configures no battery sensing for board_M5Dial, so
//     getBatteryLevel() falls through to `return -2` and getBatteryVoltage()
//     reports 0. (Its pin-identical sibling the M5DinMeter *is* configured,
//     reading a 1:2 divider on GPIO10.)
//   - GPIO10 is the only ADC1 pin M5Dial leaves free — GPIO4-9 drive the
//     GC9A01 display and GPIO1-3 are taken — but it reads 4095 counts, i.e.
//     saturated at the 3.3 V rail. A 4.1 V cell behind a 1:2 divider would
//     read ~2050 mV / ~2540 counts, so it is not a battery sense line.
//   - ADC2 is unusable here because WiFi owns it.
// The gauge therefore stays hidden on this board by design. It will light up
// unchanged if a battery source ever reports through M5.Power (a future
// library revision, or an I2C fuel-gauge unit on Port A).

// ── Filters ───────────────────────────────────────────────────────────────────
// Min-altitude filter thresholds, metres (Off, 1,000/5,000/10,000/20,000 ft).
static const float MIN_ALT_OPTIONS_M[]  = { 0.0f, 304.8f, 1524.0f, 3048.0f, 6096.0f };
static const int   MIN_ALT_OPTIONS_COUNT = 5;
