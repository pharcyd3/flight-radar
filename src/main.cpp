#include <M5Dial.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "aircraft.h"
#include "apistatus.h"
#include "adsblive.h"
#include "aircraftfeed.h"
#include "radar.h"
#include "map.h"
#include "lofimap.h"
#include "provisioning.h"
#include "settings.h"
#include "encoder_debounce.h"
#include "watchdog.h"

// ── State ────────────────────────────────────────────────────────────────────
static RadarDisplay         radar;
static std::vector<Aircraft> aircraft;
static EncoderDebouncer      zoomEncoder;

static int           zoomIdx      = ZOOM_DEFAULT;
static int           selectedAc   = -1;
static unsigned long lastFetchMs  = 0;
static unsigned long lastUpdateMs = 0;
// millis() of the last *successful* fetch. A failure keeps the previous
// aircraft set on screen rather than blanking the radar, so this (not
// lastUpdateMs) is what decides when that set has gone too stale to show.
static unsigned long lastGoodFetchMs = 0;
// A zoom step schedules a fetch rather than firing one per intermediate level
// while the dial is being spun — see ZOOM_FETCH_DEBOUNCE_MS. The fetch itself
// runs off-thread (aircraftfeed.h), so this is API politeness, not latency.
static bool          zoomFetchPending = false;
static unsigned long zoomFetchDueMs   = 0;
// Timestamp of the last user interaction (zoom/touch/menu). Background map
// precaching only runs once the user's been idle a moment, so its blocking
// composes never add latency to active interaction.
static unsigned long lastInteractionMs = 0;

// Measured loop() iterations per second, reported by the INFO/PERF debug
// commands. Input responsiveness is bounded by this: touch is only sampled
// once per iteration, so it's the number to look at whenever the UI feels
// unresponsive (it collapsed to well under 1 Hz when the network fetch still
// ran inline on this thread).
static unsigned long loopHz = 0;

// Emergency alert cooldown — don't re-alert the same aircraft within 60 s
static char          lastAlertIcao[8] = "";
static unsigned long lastAlertMs      = 0;

// ── Follow mode ──────────────────────────────────────────────────────────────
// When following, the view + fetch box centre on a tracked aircraft (by icao24)
// instead of home. followCenter* is the current view/map centre; it only jumps
// to the target's fresh position when the target drifts past ~45% of the plot
// radius (the "lazy re-centre" that keeps map composes and flight-data polls bounded).
static bool  following       = false;
static char  followIcao[8]   = "";
static float followCenterLat = 0.0f;
static float followCenterLon = 0.0f;
// ADS-B coverage has transient gaps — an airborne aircraft can be absent from a
// run of polls and reappear. Keep following through gaps and only give up after
// this long with no sighting at all (time-based so it's independent of the
// refresh rate).
static unsigned long followLastSeenMs = 0;
// 20 minutes. This was 90 s, which is fine over a well-covered land mass but
// ends a long-haul follow at the first oceanic/remote gap. Paired with
// FOLLOW_DR_MAX_S, the fetch box keeps coasting along the aircraft's last known
// heading through the gap, so the follow can survive it and re-acquire.
static const unsigned long FOLLOW_GRACE_MS = 1200000UL;
// The followed aircraft's last confirmed state, kept outside the (possibly
// empty, on a failed poll) `aircraft` vector — fetchAircraftAdsbLive() clears
// that vector unconditionally before every attempt, so a single dropped poll
// would otherwise freeze the fetch box in place (see the continuous re-centre
// block below) right when it most needs to keep moving with the plane.
static Aircraft     followLastAc;
static unsigned long followLastAcMs   = 0;
static bool          followHaveLastAc = false;
// Toggled by the SHOW/HIDE OTHERS button while following — hides all but the
// tracked aircraft for an uncluttered chase.
static bool  followHideOthers = false;

// Follow-mode pan: dragging shifts the DISPLAY view away from being centred
// exactly on the plane (the plane's true tracked position, used for fetching
// and the reticle, is unaffected) — lets you look around, e.g. to see an
// airport that's otherwise hidden behind the HIDE OTHERS / UNFOLLOW buttons.
// Stored as a screen-pixel offset (not lat/lon) so a fixed on-screen drag
// distance feels the same regardless of the current zoom, and re-projected to
// a world offset at render time using the current radius. Clamped so the
// tracked plane can never be dragged fully out of view; a tap elsewhere
// (without dragging) resets it back to centred.
static int   followPanPxX = 0, followPanPxY = 0;
static const float FOLLOW_PAN_MAX_PX = 80.0f;
// Drag-gesture tracking for the follow touch handler (distinguishes a drag from
// a tap on the two buttons vs. empty space — see the touch handling below).
// followTouchDownX/Y is the press-down position, fixed for the whole gesture,
// and is what button hit-testing uses on release; followTouchLastX/Y tracks
// the latest position for computing per-frame drag deltas and drifts during
// an ordinary tap (finger jitter/sensor noise), so hit-testing against it
// instead — as an earlier version of this code did — could fail a genuine tap
// on a short button (e.g. HIDE/SHOW OTHERS, 22 px tall) most of the time.
static int  followTouchDownX = 0, followTouchDownY = 0;
static int  followTouchLastX = 0, followTouchLastY = 0;
static bool followTouchOnButton = false, followTouchDragged = false;

static float viewLat() { return following ? followCenterLat : homeLat(); }
static float viewLon() { return following ? followCenterLon : homeLon(); }

// Display radius drives the on-screen map scale — the normal dial-controlled
// five-step range, whether following or not.
static float displayRadiusKm() {
    return ZOOM_STEPS[constrain(zoomIdx, 0, ZOOM_COUNT - 1)];
}

// Display centre = the view centre (viewLat/Lon) plus the follow-pan offset, if
// any. Fetching, hit-testing, and the reticle all keep using viewLat()/viewLon()
// (the plane's true position) unaffected — only what's drawn shifts.
static void followPanDeltaLatLon(float& dLat, float& dLon) {
    dLat = dLon = 0.0f;
    if (!following || (followPanPxX == 0 && followPanPxY == 0)) return;
    float kmPerPx = displayRadiusKm() / 105.0f;   // PLOT_R
    float cl = cosf(followCenterLat * (float)M_PI / 180.0f);
    if (cl < 0.05f) cl = 0.05f;
    dLat = (followPanPxY * kmPerPx) / 111.0f;
    dLon = -(followPanPxX * kmPerPx) / (111.0f * cl);
}
static float displayCenterLat() { float dLat, dLon; followPanDeltaLatLon(dLat, dLon); return viewLat() + dLat; }
static float displayCenterLon() { float dLat, dLon; followPanDeltaLatLon(dLat, dLon); return viewLon() + dLon; }

static void resetFollowPan() { followPanPxX = 0; followPanPxY = 0; }

// Banner label for the followed aircraft — its callsign while it's in the current
// set, otherwise its icao24. Empty when not following.
static const char* followLabel() {
    if (!following) return "";
    int idx = radar.findByIcao(aircraft, followIcao);
    if (idx >= 0 && aircraft[idx].callsign[0]) return aircraft[idx].callsign;
    return followIcao;
}

// Redraw the live radar with the current follow context applied. Single source
// of truth for "paint the current frame" so every path stays consistent.
static void redraw() {
    radar.setFollow(following, homeLat(), homeLon(), followLabel(), followHideOthers,
                    followCenterLat, followCenterLon);
    radar.draw(aircraft, displayCenterLat(), displayCenterLon(), displayRadiusKm(), zoomIdx,
               selectedAc, lastUpdateMs, feed::busy());
}

static void startFollow(int idx) {
    if (idx < 0 || idx >= (int)aircraft.size()) return;
    following = true;
    followHideOthers = false;   // start each chase showing all traffic
    resetFollowPan();
    followLastSeenMs = millis();
    strncpy(followIcao, aircraft[idx].icao24, sizeof(followIcao) - 1);
    followIcao[sizeof(followIcao) - 1] = '\0';
    followCenterLat = aircraft[idx].lat;
    followCenterLon = aircraft[idx].lon;
    followLastAc     = aircraft[idx];
    followLastAcMs   = millis();
    followHaveLastAc = true;
    lastFetchMs = 0;   // fetch soon, re-centred on the target, to load its area
}

static void stopFollow() {
    following = false;
    followHaveLastAc = false;
    followIcao[0] = '\0';
    resetFollowPan();
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static void startFetch();   // forward declaration (defined below)

// Streams the composited radar frame over serial (RGB565, row order) framed by
// SHOT_BEGIN/SHOT_END markers, so a host script can rebuild a PNG. The whole
// radar scene is composited into the map sprite (the GC9A01 panel can't be read
// back), so we dump that buffer after a fresh redraw.
static bool screenshotPaused = false;   // freeze the loop so a posed frame persists

static void captureScreenshot() {
    auto* spr = mapLayer.sprite();
    const uint8_t* buf = (const uint8_t*)spr->getBuffer();
    Serial.printf("SHOT_BEGIN %d %d\n", 240, 240);
    Serial.flush();
    Serial.write(buf, 240 * 240 * 2);
    Serial.flush();
    Serial.print("\nSHOT_END\n");
    Serial.flush();
}

// Debug/setup convenience over USB serial (local access only — same trust level
// as flashing) — a set of screenshot/pose hooks used to drive the device for
// the manual's screenshots and for on-device diagnosis.
static void checkSerialCommands() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) return;

    // ── Screenshot / pose hooks ──────────────────────────────────────────────
    if (line == "SHOT")      { captureScreenshot(); return; }
    if (line == "PAUSE")     { screenshotPaused = true;  return; }
    if (line == "RESUME")    { screenshotPaused = false; return; }
    if (line == "SPLASH")    { radar.drawBoot(); return; }
    if (line == "INFO") {
        Serial.printf("INFO ac=%d zoom=%d map=%d sel=%d follow=%d hide=%d heap=%u "
                      "home=%.4f,%.4f view=%.4f,%.4f pan=%d,%d "
                      "busy=%d loophz=%lu feedstack=%u maxalloc=%u\n",
                      (int)aircraft.size(), zoomIdx, mapMode(), selectedAc,
                      following ? 1 : 0, followHideOthers ? 1 : 0, ESP.getFreeHeap(),
                      homeLat(), homeLon(), viewLat(), viewLon(), followPanPxX, followPanPxY,
                      feed::busy() ? 1 : 0, loopHz, feed::stackHeadroom(),
                      ESP.getMaxAllocHeap());
        return;
    }
    // Times a full radar composite+push, so the animation cadence is set from
    // a measured frame cost rather than a guess.
    if (line == "PERF") {
        unsigned long t0 = micros();
        redraw();
        unsigned long dt = micros() - t0;
        Serial.printf("PERF redraw=%luus loophz=%lu heap=%u\n", dt, loopHz, ESP.getFreeHeap());
        return;
    }
    // Streams raw encoder counts for ~6 s. Turning the dial one physical click
    // while this runs shows exactly how many raw ticks a detent produces on
    // this unit, which is what EncoderDebouncer's hysteresis threshold has to
    // match (ENC_TICKS_PER_DETENT).
    if (line == "ENC") {
        long start = M5Dial.Encoder.read(), last = start;
        unsigned long until = millis() + 6000;
        Serial.printf("ENC start raw=%ld\n", start);
        while (millis() < until) {
            M5Dial.update();
            long raw = M5Dial.Encoder.read();
            if (raw != last) {
                Serial.printf("ENC raw=%ld delta=%ld t=%lu\n", raw, raw - last, millis());
                last = raw;
            }
            watchdogFeed();
            delay(2);
        }
        Serial.printf("ENC end raw=%ld net=%ld\n", last, last - start);
        return;
    }
    if (line.startsWith("MAP:"))   { setMapMode(line.substring(4).toInt()); redraw(); return; }
    if (line.startsWith("SETHOME:")) {
        int sep = line.indexOf(',', 8);
        if (sep > 0) {
            setHomeLocation(line.substring(8, sep).toFloat(), line.substring(sep + 1).toFloat());
            lastFetchMs = 0; redraw();
        }
        return;
    }
    if (line.startsWith("PAN:")) {
        int sep = line.indexOf(',', 4);
        if (sep > 0) {
            followPanPxX = line.substring(4, sep).toInt();
            followPanPxY = line.substring(sep + 1).toInt();
            redraw();
        }
        return;
    }
    if (line.startsWith("ZOOM:"))  { zoomIdx = constrain(line.substring(5).toInt(), 0, ZOOM_COUNT - 1);
                                     lastFetchMs = 0; redraw(); return; }
    if (line.startsWith("SEL:"))   { selectedAc = line.substring(4).toInt(); redraw(); return; }
    if (line == "FOLLOW")    { if (selectedAc >= 0) startFollow(selectedAc); redraw(); return; }
    if (line == "UNFOLLOW")  { stopFollow(); redraw(); return; }
    if (line == "HIDE")      { followHideOthers = !followHideOthers; redraw(); return; }
    if (line.startsWith("STATUS:")) { radar.setStatusVisible(line.substring(7).toInt() != 0); redraw(); return; }
    if (line == "REFETCH")   { startFetch(); return; }
    if (line == "LIST") {
        for (int i = 0; i < (int)aircraft.size(); ++i)
            Serial.printf("LIST %d %s %.4f,%.4f %.0fkmh %.0fm%s\n", i, aircraft[i].callsign,
                          aircraft[i].lat, aircraft[i].lon,
                          aircraft[i].speedMs * 3.6f, aircraft[i].altM,
                          aircraft[i].onGround ? " GND" : "");
        return;
    }
    if (line == "MENU") {
        renderSettingsPreview(radar, aircraft, homeLat(), homeLon(),
                              ZOOM_STEPS[zoomIdx], zoomIdx, lastUpdateMs, feed::busy());
        return;
    }
    if (line == "SETLOC")    { renderSetLocationPreview(radar); return; }
    if (line == "TRAILS") {
        const char* focus = following ? followIcao
                          : (selectedAc >= 0 && selectedAc < (int)aircraft.size()
                             ? aircraft[selectedAc].icao24 : "");
        radar.debugDumpTrails(focus, displayRadiusKm());
        return;
    }
}

static void checkEmergency() {
    if (!buzzOnEmergency()) return;

    for (const Aircraft& ac : aircraft) {
        if (!ac.isEmergency()) continue;

        unsigned long now = millis();
        bool sameAc   = strcmp(ac.icao24, lastAlertIcao) == 0;
        bool cooldown  = sameAc && (now - lastAlertMs) < 60000UL;
        if (cooldown) continue;

        strncpy(lastAlertIcao, ac.icao24, sizeof(lastAlertIcao) - 1);
        lastAlertMs = now;

        Serial.printf("[Alert] Emergency squawk %s on %s\n",
                      ac.squawk, ac.icao24);

        // Draw current radar so the ring flashes over real content
        redraw();
        radar.flashEmergencyRing();
        break;  // one alert per check cycle even if multiple emergency a/c
    }
}

// Hands a fetch to the background task (aircraftfeed.h) and returns straight
// away. No-op if one is already in flight. The poll icon picks up the "in
// flight" look from feed::busy() on the next scheduled redraw, so there's no
// need to force an extra full-frame composite here.
static void startFetch() {
    feed::request(viewLat(), viewLon(), displayRadiusKm());
}

// Drains a completed fetch, if there is one, and does the bookkeeping that has
// to happen on the UI thread (trail history, selection re-resolution, follow
// tracking, emergency alerting). Cheap and non-blocking when nothing is ready.
static void collectFetch() {
    // Capture the selection by icao24 *before* the vector is replaced: the
    // fresh set is usually reordered and possibly shorter, so the bare index is
    // meaningless afterwards — without this the selection jumps to a different
    // plane or the detail panel vanishes on every poll.
    char selIcao[8] = "";
    if (selectedAc >= 0 && selectedAc < (int)aircraft.size())
        strncpy(selIcao, aircraft[selectedAc].icao24, sizeof(selIcao) - 1);

    bool ok = false;
    if (!feed::takeResult(aircraft, ok)) return;   // nothing finished yet

    lastUpdateMs = millis();

    if (ok) {
        lastGoodFetchMs = lastUpdateMs;
        // Append this poll's reported positions to the breadcrumb trails.
        radar.recordHistory(aircraft, following ? followIcao : selIcao);
    }

    // Re-locate the selection by icao24 in the current set (follow target takes
    // precedence). While following, keep going through ADS-B coverage gaps and
    // only give up after FOLLOW_GRACE_MS with no sighting at all.
    if (following) {
        int idx = radar.findByIcao(aircraft, followIcao);
        if (idx >= 0) {
            selectedAc       = idx;
            followLastSeenMs = millis();
            followLastAc     = aircraft[idx];
            followLastAcMs   = millis();
            followHaveLastAc = true;
        } else if (ok) {
            // Only a *successful* poll that didn't contain the target counts as
            // a miss — a failed one says nothing about where the plane is, and
            // letting it start the grace clock was dropping follows during
            // ordinary network blips.
            selectedAc = -1;
            if (millis() - followLastSeenMs > FOLLOW_GRACE_MS) stopFollow();
        }
    } else if (selIcao[0]) {
        selectedAc = radar.findByIcao(aircraft, selIcao);
    }

    // Outcome (incl. failures) is surfaced by the poll icon colour and the
    // tap-to-view status panel, so just redraw the radar either way.
    redraw();

    checkEmergency();
}

// ── Background map precache ────────────────────────────────────────────────
// Zoom changes are only instant when the target level is already cached to
// flash (a ~115 KB sprite read); an uncached level means a blocking multi-tile
// network compose (a few seconds + "loading map..."). So once the live view is
// up we quietly compose+cache the OTHER zoom levels for the current home,
// nearest-to-current first (most likely next step), one level per idle pass.
// The round restarts whenever home moves (favourites / location portal).
static float   precacheLat  = 999.0f, precacheLon = 999.0f;
static uint8_t precacheDone = 0;    // bit i set once level i handled this round

static void maybePrecacheMaps() {
    // Restart the round when home moves.
    if (fabsf(homeLat() - precacheLat) > 1e-6f ||
        fabsf(homeLon() - precacheLon) > 1e-6f) {
        precacheLat  = homeLat();
        precacheLon  = homeLon();
        precacheDone = 0;
        // Drop the previous location's aircraft (stale for the new home anyway)
        // so the fresh composes below get maximum contiguous heap — decoder +
        // tile-TLS together need most of it, and a big held aircraft set can
        // fragment it enough that tile fetches fail. The next poll repopulates.
        aircraft.clear();
        selectedAc = -1;
    }

    // Pick the not-yet-handled level closest to the current zoom.
    int best = -1, bestDist = 99;
    for (int i = 0; i < ZOOM_COUNT; i++) {
        if (precacheDone & (1 << i)) continue;
        int d = abs(i - zoomIdx);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best < 0) {
        // Whole round done → every level is cached, so the PNG decoder is now
        // dead weight (composes are cache hits). Free its ~45 KB so the flight-data
        // polls have the headroom to handle a big 200 km response. A later
        // compose (new location) re-primes it on demand. Idempotent.
        mapLayer.releaseDecoder();
        return;
    }

    precacheDone |= (1 << best);
    if (best == zoomIdx) return;          // on-screen level is already cached

    // Compose+cache it (no-op if already cached). Blocking, but only reached
    // when idle. If it actually composed it dirtied the shared sprite, so
    // repaint the current level (now a fast cache hit) to keep the screen right.
    if (mapLayer.precache(homeLat(), homeLon(), ZOOM_STEPS[best])) {
        radar.draw(aircraft, homeLat(), homeLon(), ZOOM_STEPS[zoomIdx], zoomIdx,
                   selectedAc, lastUpdateMs, feed::busy());
    }
}

// ── Arduino lifecycle ─────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5Dial.begin(cfg, /*encoder=*/true, /*RFID=*/false);
    M5Dial.Display.setRotation(0);
    M5Dial.Display.setBrightness(160);

    watchdogBegin();     // auto-reset if a blocking call (TLS/compose) ever wedges

    radar.begin();
    mapLayer.begin();
    if (!lofi::begin())
        Serial.println("[LoFi] vector map blob missing/invalid — lo-fi map disabled");
    radar.drawBoot();
    delay(1500);

    runProvisioning();   // blocks until WiFi connected (runs portal if needed)
    loadSettings();

    // Drop cached maps for any location other than the current home. Accumulated
    // stale entries (e.g. from repeated location changes) fill the flash partition,
    // which makes each new save evict another — thrashing endless recomposes that
    // keep the PNG decoder resident and heap too low/fragmented for the TLS
    // handshake (the "SSL - Memory allocation failed" freezes). Pruning lets the
    // precache round finish, free the decoder, and settle at a healthy heap.
    mapLayer.pruneExcept(homeLat(), homeLon());

    // Allocate the aircraft buffer once, up front. This heap has to keep a
    // contiguous ~40 KB block free for each TLS handshake, and a vector that
    // grows/reallocates later is exactly what splits the large free region
    // (the historical cause of "SSL - Memory allocation failed" stalling the
    // feed after a busy response). The feed's own buffers are reserved to match.
    aircraft.reserve(MAX_AIRCRAFT);

    feed::begin();       // background fetch task — the UI never blocks on network
    startFetch();        // first poll; the loop picks up the result when it lands

    lastFetchMs = millis();
    zoomEncoder.begin();
}

void loop() {
    watchdogFeed();      // heartbeat — a stalled loop for 60s triggers auto-reset

    // Loop-rate counter — see loopHz. Cheap, and the single most useful number
    // for telling "the UI is busy" apart from "the UI is blocked".
    {
        static unsigned long tickCount = 0, tickWindowMs = 0;
        tickCount++;
        unsigned long t = millis();
        if (t - tickWindowMs >= 1000UL) {
            loopHz       = tickCount;
            tickCount    = 0;
            tickWindowMs = t;
        }
    }

    M5Dial.update();
    checkResetCombo();
    checkSerialCommands();

    // Screenshot freeze: hold the current posed frame (no fetch/redraw/input) so
    // it can be captured deterministically. Serial commands are still processed
    // above, so PAUSE/SHOT/RESUME keep working.
    if (screenshotPaused) return;

    if (settingsRequested()) {
        float r = displayRadiusKm();
        runSettings(radar, aircraft, viewLat(), viewLon(), r, zoomIdx, lastUpdateMs, feed::busy());
        // The map cache is content-addressed by (lat,lon,radius), so a changed
        // home location just means the next radar.draw()/ensure() loads or
        // composes a different cache entry — no need to wipe anything here.
        redraw();
        lastInteractionMs = millis();
        return;
    }

    // ── Encoder: zoom in / out ───────────────────────────────────────────────
    // See encoder_debounce.h — this hardware needs more than a raw-tick ->
    // detent conversion to behave as a reliable "one click = one step" input.
    int zoomDelta;
    if (zoomEncoder.poll(&zoomDelta)) {
        lastInteractionMs = millis();

        if (selectedAc >= 0 && !following) {
            // An aircraft is selected → the dial scrolls the selection between
            // aircraft instead of changing zoom. Step once per detent, in the
            // turn direction, wrapping around the visible aircraft. (While
            // following, the dial zooms instead — the selection is locked.)
            float r   = displayRadiusKm();
            int   dir = zoomDelta > 0 ? 1 : -1;
            for (int k = 0; k < abs(zoomDelta); ++k) {
                int nx = radar.nextSelectable(selectedAc, dir,
                                              aircraft, viewLat(), viewLon(), r);
                if (nx < 0) break;   // nothing selectable
                selectedAc = nx;
            }
            redraw();
            return;
        }

        zoomIdx = constrain(zoomIdx + zoomDelta, 0, ZOOM_COUNT - 1);
        if (!following) {
            selectedAc = -1;
            // Redraw immediately (rings/zoom-dots/map at the new radius, using
            // the last-fetched aircraft) so the step feels instant; the actual
            // network fetch is debounced below so spinning through several
            // levels quickly doesn't block on one for each intermediate step.
            zoomFetchPending = true;
            zoomFetchDueMs   = millis() + ZOOM_FETCH_DEBOUNCE_MS;
            redraw();
            return;
        }
        // Following: manual zoom, same as the normal view.
        redraw();
        return;
    }

    // ── Touch: select / deselect aircraft, or (while following) drag to look
    // around / tap the two buttons ──────────────────────────────────────────────
    auto touch = M5Dial.Touch.getDetail();
    if (following) {
        // Follow uses a full press/hold/release lifecycle so a drag can be told
        // apart from a tap: dragging pans the view (see followPanPxX/Y above);
        // a plain tap on UNFOLLOW/HIDE OTHERS fires that button; a tap on empty
        // space recentres the pan. This replaces the old "only wasPressed matters"
        // handling, which can't distinguish a drag from a tap.
        if (touch.wasPressed()) {
            lastInteractionMs = millis();
            followTouchDownX = touch.x; followTouchDownY = touch.y;
            followTouchLastX = touch.x; followTouchLastY = touch.y;
            followTouchOnButton = radar.hitUnfollowButton(touch.x, touch.y) ||
                                  radar.hitOthersButton(touch.x, touch.y);
            followTouchDragged = false;
        } else if (touch.isPressed()) {
            int dx = touch.x - followTouchLastX, dy = touch.y - followTouchLastY;
            if (!followTouchOnButton && (dx != 0 || dy != 0)) {
                followPanPxX += dx;
                followPanPxY += dy;
                float mag = sqrtf((float)followPanPxX * followPanPxX +
                                  (float)followPanPxY * followPanPxY);
                if (mag > FOLLOW_PAN_MAX_PX) {
                    float scale = FOLLOW_PAN_MAX_PX / mag;
                    followPanPxX = (int)(followPanPxX * scale);
                    followPanPxY = (int)(followPanPxY * scale);
                }
                followTouchDragged = true;
                lastInteractionMs = millis();
                // Rate-limit the repaint. The pan offset above is applied every
                // sample so the drag stays accurate, but a full composite costs
                // ~70 ms — repainting on every touch report would spend the
                // whole drag inside draw() and leave few gaps to sample the
                // release in, which is how a drag can end up "eating" the tap
                // that follows it.
                static unsigned long lastDragDrawMs = 0;
                unsigned long tnow = millis();
                if (tnow - lastDragDrawMs >= 80UL) {
                    lastDragDrawMs = tnow;
                    redraw();
                }
            }
            followTouchLastX = touch.x; followTouchLastY = touch.y;
        } else if (touch.wasReleased() && !followTouchDragged) {
            // Hit-test against where the finger went DOWN, not its last-tracked
            // position — the latter drifts a few px during an ordinary tap
            // (finger jitter/sensor noise) and would otherwise miss a real tap
            // on a short button most of the time (see followTouchDownX/Y above).
            if (radar.hitUnfollowButton(followTouchDownX, followTouchDownY)) {
                selectedAc = radar.findByIcao(aircraft, followIcao);
                stopFollow();
            } else if (radar.hitOthersButton(followTouchDownX, followTouchDownY)) {
                followHideOthers = !followHideOthers;   // toggle other traffic
            } else if (!followTouchOnButton) {
                resetFollowPan();   // plain tap on empty space recentres
            }
            redraw();
        }
    } else if (touch.wasPressed()) {
        lastInteractionMs = millis();
        float r = displayRadiusKm();
        if (radar.hitPollIcon(touch.x, touch.y)) {
            // Tap the poll icon to toggle the API status panel
            radar.setStatusVisible(!radar.statusVisible());
            selectedAc = -1;
        } else if (selectedAc >= 0 && !radar.statusVisible() &&
                   radar.hitFollowButton(touch.x, touch.y)) {
            // FOLLOW button in the detail panel starts tracking (and hides the panel).
            startFollow(selectedAc);
            // This same physical touch is still down and will report isPressed()/
            // wasReleased() on later loop iterations, now routed to the follow
            // touch handler above — which never saw its wasPressed() edge, so
            // its drag-gesture state (last X/Y, on-button, dragged) would
            // otherwise be stale leftovers from whatever was touched last.
            // Priming it here as "already on a button, not dragging" makes that
            // leftover release a no-op instead of a bogus jump or a phantom hit
            // on UNFOLLOW/HIDE OTHERS.
            followTouchDownX = touch.x; followTouchDownY = touch.y;
            followTouchLastX = touch.x; followTouchLastY = touch.y;
            followTouchOnButton = true;
            followTouchDragged  = false;
        } else {
            radar.setStatusVisible(false);   // any other tap dismisses the panel
            int hit = radar.hitTest(touch.x, touch.y, aircraft, viewLat(), viewLon(), r);
            selectedAc = (hit == selectedAc) ? -1 : hit;  // tap again to deselect
        }
        // Immediate redraw after touch so it feels responsive
        redraw();
    }

    // ── Feed: collect finished fetches, schedule new ones ─────────────────────
    // Both are non-blocking; the request itself runs on the background task
    // (aircraftfeed.h) so a slow or dead network can no longer stall input.
    collectFetch();

    unsigned long now = millis();
    if (zoomFetchPending && now >= zoomFetchDueMs) {
        // The user has settled on a zoom level — poll once for it now, and
        // restart the periodic timer from here so this doesn't immediately
        // double up with the check below on the same tick.
        zoomFetchPending = false;
        lastFetchMs = now;
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        startFetch();
    } else if (now - lastFetchMs >= refreshIntervalMs()) {
        lastFetchMs = now;
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        startFetch();
    }

    // A failed fetch deliberately leaves the previous aircraft on screen so a
    // transient blip doesn't blank the radar. Past the dead-reckoning cap they
    // are frozen and drifting ever further from reality, so drop them and show
    // an honestly empty scope instead.
    if (!aircraft.empty() && lastGoodFetchMs != 0 &&
        now - lastGoodFetchMs > FEED_STALE_CLEAR_MS) {
        aircraft.clear();
        selectedAc = -1;
        redraw();
    }

    // ── Follow: keep the tracked aircraft glued to the centre ───────────────────
    // Continuously re-centre on the target's interpolated position, so it stays
    // in the middle and the fetch box always travels with it (no lag that could
    // let it slip out of coverage). The actual repaint happens at the animation
    // cadence below; the lo-fi background used while following recentres for free.
    if (following) {
        int idx = radar.findByIcao(aircraft, followIcao);
        if (idx >= 0) {
            radar.interpPos(aircraft[idx], followCenterLat, followCenterLon);
        } else if (followHaveLastAc) {
            // Not in this poll's data — most often because the last fetch
            // attempt failed and cleared the whole aircraft list, not because
            // the plane is actually gone. Keep dead-reckoning from its last
            // confirmed state so the fetch box keeps moving with it instead of
            // freezing (and likely missing it again) until the next success.
            float dtS = (float)(millis() - followLastAcMs) / 1000.0f;
            radar.projectForward(followLastAc.lat, followLastAc.lon, followLastAc.heading,
                                 followLastAc.speedMs, followLastAc.onGround, dtS,
                                 FOLLOW_DR_MAX_S, followCenterLat, followCenterLon);
        }
    }

    // ── Periodic redraw (1 Hz — keeps the poll icon's countdown ticking) ─────
    // Only the poll icon (and, if open, the status overlay) needs to animate
    // every second; re-running a full draw() here would re-push the whole
    // map sprite every tick for no reason, which is what was causing the
    // rings/labels to visibly flicker. The aircraft detail panel has no live
    // timer of its own, so it needs no periodic redraw at all — only a new
    // fetch or touch event changes it, both already handled elsewhere.
    // Dead-reckoning animation: if anything airborne is moving, its mark has
    // drifted since the last frame, so repaint the whole radar to let it glide
    // between polls. draw() composites into one sprite and pushes it in a single
    // transfer, so this full redraw is flicker-free. While following we redraw a
    // little faster and always, to keep the tracked aircraft smoothly centred.
    // Otherwise, when nothing's moving, just tick the poll-icon countdown.
    static unsigned long lastDrawMs = 0;
    unsigned long drawInterval = following ? 500UL : 1000UL;
    if (now - lastDrawMs >= drawInterval) {
        lastDrawMs = now;

        bool anyMoving = following;
        if (!anyMoving && !feed::busy() && !radar.statusVisible()) {
            for (const Aircraft& ac : aircraft)
                if (!ac.onGround && ac.speedMs >= INTERP_MIN_SPEED_MS) { anyMoving = true; break; }
        }

        if (anyMoving) {
            redraw();
        } else {
            radar.updatePollIcon(lastUpdateMs, feed::busy());
            radar.updateStatusOverlay();
        }
    }

    // ── Background map precache ────────────────────────────────────────────────
    // When the user's been idle a moment (and no fetch is in flight), cache one
    // more zoom level for the current home so future zoom changes are instant.
    // A compose blocks a few seconds, so gating on idle keeps it off the
    // interactive path; the level nearest the current zoom is done first.
    // (Skipped when the map underlay is disabled — there's nothing to precache.)
    if (!feed::busy() && !following && mapMode() == MAP_FULL &&
        now - lastInteractionMs >= 1500UL) {
        maybePrecacheMaps();
    } else if (mapMode() != MAP_FULL) {
        // Lo-fi / Off: the PNG tile decoder is dead weight, and leaving its
        // ~45 KB resident starved the TLS handshake (the fetch's "SSL - Memory
        // allocation failed") because free heap sat too low/fragmented. Release
        // it so polls have headroom; a later switch to Full re-primes it on
        // demand from an idle compose. Idempotent — a no-op once released.
        mapLayer.releaseDecoder();
    }
}
