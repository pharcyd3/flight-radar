#include <M5Dial.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "aircraft.h"
#include "opensky.h"
#include "adsblive.h"
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
static bool          fetchInProgress = false;
// Timestamp of the last user interaction (zoom/touch/menu). Background map
// precaching only runs once the user's been idle a moment, so its blocking
// composes never add latency to active interaction.
static unsigned long lastInteractionMs = 0;

// Emergency alert cooldown — don't re-alert the same aircraft within 60 s
static char          lastAlertIcao[8] = "";
static unsigned long lastAlertMs      = 0;

// ── Follow mode ──────────────────────────────────────────────────────────────
// When following, the view + fetch box centre on a tracked aircraft (by icao24)
// instead of home. followCenter* is the current view/map centre; it only jumps
// to the target's fresh position when the target drifts past ~45% of the plot
// radius (the "lazy re-centre" that keeps map composes and OpenSky polls bounded).
static bool  following       = false;
static char  followIcao[8]   = "";
static float followCenterLat = 0.0f;
static float followCenterLon = 0.0f;
// OpenSky data has transient gaps — an airborne aircraft can be absent from a
// run of polls and reappear (sparse ADS-B coverage). Keep following through gaps
// and only give up after this long with no sighting at all (time-based so it's
// independent of the refresh rate).
static unsigned long followLastSeenMs = 0;
static const unsigned long FOLLOW_GRACE_MS = 90000UL;
// Toggled by the SHOW/HIDE OTHERS button while following — hides all but the
// tracked aircraft for an uncluttered chase.
static bool  followHideOthers = false;

static float viewLat() { return following ? followCenterLat : homeLat(); }
static float viewLon() { return following ? followCenterLon : homeLon(); }

// Display radius drives the on-screen map scale. Follow mode uses the extended
// zoom table (out to a whole-earth view); normal mode uses the standard five.
static float displayRadiusKm() {
    if (following) return FOLLOW_ZOOM_STEPS[constrain(zoomIdx, 0, FOLLOW_ZOOM_COUNT - 1)];
    return ZOOM_STEPS[constrain(zoomIdx, 0, ZOOM_COUNT - 1)];
}

// Fetch radius is decoupled from display zoom and capped, so zooming the map out
// in follow mode keeps pulling only the tracked aircraft's local traffic rather
// than ballooning the request (or exceeding airplanes.live's 250 nm limit).
static float fetchRadiusKm() {
    float d = displayRadiusKm();
    return d < FETCH_MAX_KM ? d : FETCH_MAX_KM;
}

static int maxZoomIdx() { return (following ? FOLLOW_ZOOM_COUNT : ZOOM_COUNT) - 1; }

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
    radar.setFollow(following, homeLat(), homeLon(), followLabel(), followHideOthers);
    radar.draw(aircraft, viewLat(), viewLon(), displayRadiusKm(), zoomIdx,
               selectedAc, lastUpdateMs, fetchInProgress);
}

static void startFollow(int idx) {
    if (idx < 0 || idx >= (int)aircraft.size()) return;
    following = true;
    followHideOthers = false;   // start each chase showing all traffic
    followLastSeenMs = millis();
    strncpy(followIcao, aircraft[idx].icao24, sizeof(followIcao) - 1);
    followIcao[sizeof(followIcao) - 1] = '\0';
    followCenterLat = aircraft[idx].lat;
    followCenterLon = aircraft[idx].lon;
    lastFetchMs = 0;   // fetch soon, re-centred on the target, to load its area
}

static void stopFollow() {
    following = false;
    followIcao[0] = '\0';
    // Extended zoom-out levels only exist in follow mode; clamp back into the
    // normal range so we don't return to the home view zoomed out to the globe.
    if (zoomIdx > ZOOM_COUNT - 1) zoomIdx = ZOOM_COUNT - 1;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static void doFetch();   // forward declaration (defined below)

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
// as flashing). Handles OpenSky credential entry (SETCREDS:) plus a set of
// screenshot/pose hooks used to drive the device for the manual's screenshots.
static void checkSerialCommands() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) return;

    if (line.startsWith("SETCREDS:")) {
        int sep = line.indexOf(':', 9);
        if (sep < 0) {
            Serial.println("[Serial] SETCREDS malformed, expected SETCREDS:<id>:<secret>");
            return;
        }
        setOpenSkyCredentials(line.substring(9, sep).c_str(),
                              line.substring(sep + 1).c_str());
        return;
    }

    // ── Screenshot / pose hooks ──────────────────────────────────────────────
    if (line == "SHOT")      { captureScreenshot(); return; }
    if (line == "PAUSE")     { screenshotPaused = true;  return; }
    if (line == "RESUME")    { screenshotPaused = false; return; }
    if (line == "SPLASH")    { radar.drawBoot(); return; }
    if (line == "INFO") {
        Serial.printf("INFO ac=%d zoom=%d map=%d sel=%d follow=%d hide=%d heap=%u\n",
                      (int)aircraft.size(), zoomIdx, mapMode(), selectedAc,
                      following ? 1 : 0, followHideOthers ? 1 : 0, ESP.getFreeHeap());
        return;
    }
    if (line.startsWith("MAP:"))   { setMapMode(line.substring(4).toInt()); redraw(); return; }
    if (line.startsWith("SRC:"))   { setDataSource(line.substring(4).toInt()); lastFetchMs = 0; return; }
    if (line.startsWith("ZOOM:"))  { zoomIdx = constrain(line.substring(5).toInt(), 0, maxZoomIdx());
                                     lastFetchMs = 0; redraw(); return; }
    if (line.startsWith("SEL:"))   { selectedAc = line.substring(4).toInt(); redraw(); return; }
    if (line == "FOLLOW")    { if (selectedAc >= 0) startFollow(selectedAc); redraw(); return; }
    if (line == "UNFOLLOW")  { stopFollow(); redraw(); return; }
    if (line == "HIDE")      { followHideOthers = !followHideOthers; redraw(); return; }
    if (line.startsWith("STATUS:")) { radar.setStatusVisible(line.substring(7).toInt() != 0); redraw(); return; }
    if (line == "REFETCH")   { doFetch(); return; }
    if (line == "MENU") {
        renderSettingsPreview(radar, aircraft, homeLat(), homeLon(),
                              ZOOM_STEPS[zoomIdx], zoomIdx, lastUpdateMs, fetchInProgress);
        return;
    }
    if (line == "SETLOC")    { renderSetLocationPreview(radar); return; }
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

static void doFetch() {
    float r = fetchRadiusKm();   // capped; decoupled from display zoom in follow mode

    // Remember the selected aircraft by icao24: fetchAircraft() rebuilds the
    // vector (usually reordered, possibly shorter), so the bare index is stale
    // afterwards — without this the selection jumps to a different plane or the
    // detail panel vanishes on every poll.
    char selIcao[8] = "";
    if (selectedAc >= 0 && selectedAc < (int)aircraft.size())
        strncpy(selIcao, aircraft[selectedAc].icao24, sizeof(selIcao) - 1);

    // Flip the poll icon into its "request in flight" look immediately,
    // before the (possibly slow) network call blocks everything else.
    fetchInProgress = true;
    redraw();

    // Fetch is centred on the view centre — home normally, the tracked aircraft
    // while following — from whichever data source is selected.
    bool ok = (dataSource() == SOURCE_ADSBLIVE)
                  ? fetchAircraftAdsbLive(viewLat(), viewLon(), r, aircraft)
                  : fetchAircraftOpenSky(viewLat(), viewLon(), r, aircraft);
    // Append this poll's reported positions to the breadcrumb trails (only on a
    // real success — a failed fetch clears `aircraft`, and we don't want an empty
    // frame wiping the history that interpolation and the selected-trail draw on).
    if (ok) radar.recordHistory(aircraft, following ? followIcao : selIcao);
    lastUpdateMs = millis();
    fetchInProgress = false;

    // Re-locate the selection by icao24 in the fresh set (follow target takes
    // precedence). While following, keep going through OpenSky coverage gaps and
    // only give up after FOLLOW_GRACE_MS with no sighting at all.
    if (following) {
        int idx = radar.findByIcao(aircraft, followIcao);
        if (idx >= 0) {
            selectedAc       = idx;
            followLastSeenMs = millis();
        } else {
            selectedAc = -1;       // not seen this poll; view stays put
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
        // dead weight (composes are cache hits). Free its ~45 KB so OpenSky
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
                   selectedAc, lastUpdateMs, fetchInProgress);
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

    doFetch();

    lastFetchMs = millis();
    zoomEncoder.begin(ENC_STABLE_MS_ZOOM);
}

void loop() {
    watchdogFeed();      // heartbeat — a stalled loop for 60s triggers auto-reset
    M5Dial.update();
    checkResetCombo();
    checkSerialCommands();

    // Screenshot freeze: hold the current posed frame (no fetch/redraw/input) so
    // it can be captured deterministically. Serial commands are still processed
    // above, so PAUSE/SHOT/RESUME keep working.
    if (screenshotPaused) return;

    if (settingsRequested()) {
        float r = displayRadiusKm();
        runSettings(radar, aircraft, viewLat(), viewLon(), r, zoomIdx, lastUpdateMs, fetchInProgress);
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

        zoomIdx = constrain(zoomIdx + zoomDelta, 0, maxZoomIdx());
        if (!following) { selectedAc = -1; doFetch(); lastFetchMs = millis(); return; }
        // Following: zooming the map out only changes the display (the fetch box
        // is capped), so just redraw — no need to re-poll for a wider area.
        redraw();
        return;
    }

    // ── Touch: select / deselect aircraft ────────────────────────────────────
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasPressed()) {
        lastInteractionMs = millis();
        float r = displayRadiusKm();
        if (following) {
            // Follow is a mode: the only control is UNFOLLOW. Tapping anywhere
            // else on the map does nothing, so the chase can't be dropped by an
            // accidental tap. Unfollowing re-selects the plane so its detail panel
            // (and FOLLOW button) come back.
            if (radar.hitUnfollowButton(touch.x, touch.y)) {
                selectedAc = radar.findByIcao(aircraft, followIcao);
                stopFollow();
            } else if (radar.hitOthersButton(touch.x, touch.y)) {
                followHideOthers = !followHideOthers;   // toggle other traffic
            }
        } else if (radar.hitPollIcon(touch.x, touch.y)) {
            // Tap the poll icon to toggle the API status panel
            radar.setStatusVisible(!radar.statusVisible());
            selectedAc = -1;
        } else if (selectedAc >= 0 && !radar.statusVisible() &&
                   radar.hitFollowButton(touch.x, touch.y)) {
            // FOLLOW button in the detail panel starts tracking (and hides the panel).
            startFollow(selectedAc);
        } else {
            radar.setStatusVisible(false);   // any other tap dismisses the panel
            int hit = radar.hitTest(touch.x, touch.y, aircraft, viewLat(), viewLon(), r);
            selectedAc = (hit == selectedAc) ? -1 : hit;  // tap again to deselect
        }
        // Immediate redraw after touch so it feels responsive
        redraw();
    }

    // ── Auto-refresh ──────────────────────────────────────────────────────────
    unsigned long now = millis();
    if (now - lastFetchMs >= refreshIntervalMs()) {
        lastFetchMs = now;
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        doFetch();
    }

    // ── Follow: keep the tracked aircraft glued to the centre ───────────────────
    // Continuously re-centre on the target's interpolated position, so it stays
    // in the middle and the fetch box always travels with it (no lag that could
    // let it slip out of coverage). The actual repaint happens at the animation
    // cadence below; the lo-fi background used while following recentres for free.
    if (following) {
        int idx = radar.findByIcao(aircraft, followIcao);
        if (idx >= 0)
            radar.interpPos(aircraft[idx], followCenterLat, followCenterLon);
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
        if (!anyMoving && !fetchInProgress && !radar.statusVisible()) {
            for (const Aircraft& ac : aircraft)
                if (!ac.onGround && ac.speedMs >= INTERP_MIN_SPEED_MS) { anyMoving = true; break; }
        }

        if (anyMoving) {
            redraw();
        } else {
            radar.updatePollIcon(lastUpdateMs, fetchInProgress);
            radar.updateStatusOverlay();
        }
    }

    // ── Background map precache ────────────────────────────────────────────────
    // When the user's been idle a moment (and no fetch is in flight), cache one
    // more zoom level for the current home so future zoom changes are instant.
    // A compose blocks a few seconds, so gating on idle keeps it off the
    // interactive path; the level nearest the current zoom is done first.
    // (Skipped when the map underlay is disabled — there's nothing to precache.)
    if (!fetchInProgress && !following && mapMode() == MAP_FULL &&
        now - lastInteractionMs >= 1500UL) {
        maybePrecacheMaps();
    } else if (mapMode() != MAP_FULL) {
        // Lo-fi / Off: the PNG tile decoder is dead weight, and leaving its
        // ~45 KB resident starved the TLS handshake (OpenSky's "SSL - Memory
        // allocation failed") because free heap sat too low/fragmented. Release
        // it so polls have headroom; a later switch to Full re-primes it on
        // demand from an idle compose. Idempotent — a no-op once released.
        mapLayer.releaseDecoder();
    }
}
