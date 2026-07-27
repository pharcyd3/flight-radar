#include <M5Dial.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "aircraft.h"
#include "opensky.h"
#include "radar.h"
#include "map.h"
#include "lofimap.h"
#include "provisioning.h"
#include "settings.h"
#include "encoder_debounce.h"

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

static float viewLat() { return following ? followCenterLat : homeLat(); }
static float viewLon() { return following ? followCenterLon : homeLon(); }

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
    radar.setFollow(following, homeLat(), homeLon(), followLabel());
    radar.draw(aircraft, viewLat(), viewLon(), ZOOM_STEPS[zoomIdx], zoomIdx,
               selectedAc, lastUpdateMs, fetchInProgress);
}

static void startFollow(int idx) {
    if (idx < 0 || idx >= (int)aircraft.size()) return;
    following = true;
    strncpy(followIcao, aircraft[idx].icao24, sizeof(followIcao) - 1);
    followIcao[sizeof(followIcao) - 1] = '\0';
    followCenterLat = aircraft[idx].lat;
    followCenterLon = aircraft[idx].lon;
    lastFetchMs = 0;   // fetch soon, re-centred on the target, to load its area
}

static void stopFollow() {
    following = false;
    followIcao[0] = '\0';
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// Debug/setup convenience: accepts `SETCREDS:<client_id>:<client_secret>\n`
// over USB serial so OpenSky OAuth2 credentials can be set without going
// through the WiFi captive portal. Local USB access only — same trust level
// as flashing the device.
static void checkSerialCommands() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!line.startsWith("SETCREDS:")) return;

    int sep = line.indexOf(':', 9);
    if (sep < 0) {
        Serial.println("[Serial] SETCREDS malformed, expected SETCREDS:<id>:<secret>");
        return;
    }
    String cid = line.substring(9, sep);
    String sec = line.substring(sep + 1);
    setOpenSkyCredentials(cid.c_str(), sec.c_str());
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
    float r = ZOOM_STEPS[zoomIdx];

    // Flip the poll icon into its "request in flight" look immediately,
    // before the (possibly slow) network call blocks everything else.
    fetchInProgress = true;
    redraw();

    // Fetch is centred on the view centre — home normally, the tracked aircraft
    // while following.
    bool ok = fetchAircraft(viewLat(), viewLon(), r, aircraft);
    // Append this poll's reported positions to the breadcrumb trails (only on a
    // real success — a failed fetch clears `aircraft`, and we don't want an empty
    // frame wiping the history that interpolation and the selected-trail draw on).
    if (ok) radar.recordHistory(aircraft);
    lastUpdateMs = millis();
    fetchInProgress = false;

    // Re-lock the follow selection onto the target in the fresh set; if it's no
    // longer there (landed / left coverage / filtered out), stop following.
    if (following) {
        int idx = radar.findByIcao(aircraft, followIcao);
        if (idx < 0) { stopFollow(); selectedAc = -1; }
        else         { selectedAc = idx; }
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

    radar.begin();
    mapLayer.begin();
    if (!lofi::begin())
        Serial.println("[LoFi] vector map blob missing/invalid — lo-fi map disabled");
    radar.drawBoot();
    delay(1500);

    runProvisioning();   // blocks until WiFi connected (runs portal if needed)
    loadSettings();
    doFetch();

    lastFetchMs = millis();
    zoomEncoder.begin(ENC_STABLE_MS_ZOOM);
}

void loop() {
    M5Dial.update();
    checkResetCombo();
    checkSerialCommands();

    if (settingsRequested()) {
        float r = ZOOM_STEPS[zoomIdx];
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
            float r   = ZOOM_STEPS[zoomIdx];
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
        if (!following) selectedAc = -1;   // keep the locked target while following
        doFetch();
        lastFetchMs = millis();
        return;  // redraw handled inside doFetch → we fall through on next loop
    }

    // ── Touch: select / deselect aircraft ────────────────────────────────────
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasPressed()) {
        lastInteractionMs = millis();
        float r = ZOOM_STEPS[zoomIdx];
        if (radar.hitPollIcon(touch.x, touch.y)) {
            // Tap the poll icon to toggle the API status panel
            radar.setStatusVisible(!radar.statusVisible());
            selectedAc = -1;
            stopFollow();
        } else if (selectedAc >= 0 && !radar.statusVisible() &&
                   radar.hitFollowButton(touch.x, touch.y)) {
            // FOLLOW / STOP button in the detail panel toggles tracking.
            if (following) stopFollow();
            else           startFollow(selectedAc);
        } else {
            radar.setStatusVisible(false);   // any other tap dismisses the panel
            // Hit-test against the current view centre before any follow exit
            // changes it, so the tap lands where the marks are drawn.
            int hit = radar.hitTest(touch.x, touch.y, aircraft, viewLat(), viewLon(), r);
            if (following) stopFollow();       // selecting anew leaves follow mode
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

    // ── Follow: lazy re-centre on the tracked aircraft ──────────────────────────
    // The target drifts within the view (smoothly, via interpolation); once it
    // wanders past ~45% of the plot radius, snap the centre back onto it. That
    // recomposes the map at the new centre (OSM tiles only — not an OpenSky poll);
    // the fetch box catches up at the next scheduled refresh.
    if (following) {
        int idx = radar.findByIcao(aircraft, followIcao);
        if (idx >= 0) {
            float ilat, ilon;
            radar.interpPos(aircraft[idx], ilat, ilon);
            if (radar.offCenter(ilat, ilon, followCenterLat, followCenterLon,
                                ZOOM_STEPS[zoomIdx], 0.45f)) {
                followCenterLat = aircraft[idx].lat;
                followCenterLon = aircraft[idx].lon;
                redraw();   // snap the view (and map) onto the target now
            }
        }
    }

    // ── Periodic redraw (1 Hz — keeps the poll icon's countdown ticking) ─────
    // Only the poll icon (and, if open, the status overlay) needs to animate
    // every second; re-running a full draw() here would re-push the whole
    // map sprite every tick for no reason, which is what was causing the
    // rings/labels to visibly flicker. The aircraft detail panel has no live
    // timer of its own, so it needs no periodic redraw at all — only a new
    // fetch or touch event changes it, both already handled elsewhere.
    static unsigned long lastDrawMs = 0;
    if (now - lastDrawMs >= 1000UL) {
        lastDrawMs = now;

        // Dead-reckoning animation: if anything airborne is moving, its mark has
        // drifted since the last frame, so repaint the whole radar to let it glide
        // between polls. draw() composites into one sprite and pushes it in a
        // single transfer, so this full redraw is flicker-free (unlike the old
        // push-then-overlay path, which is why this used to be icon-only). When
        // nothing is moving — or a fetch/overlay is up — just tick the poll icon.
        bool anyMoving = false;
        if (!fetchInProgress && !radar.statusVisible()) {
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
    }
}
