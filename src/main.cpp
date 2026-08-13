#include <M5Dial.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "aircraft.h"
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
// instead of home. followCenter* is the current view/map centre, updated every
// loop from the target's dead-reckoned position so the fetch box travels with
// it continuously — verified in flight to hold the aircraft within ~100 m of
// centre, and to coast accurately through multi-minute coverage gaps.
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

// View pan: dragging shifts the DISPLAY away from its natural centre — the
// tracked aircraft while following, home otherwise. What the view is centred
// *on* for fetching (and the follow reticle) is unaffected, so panning is
// purely a look-around and never moves the search box or your home location.
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

// Free browsing: dragging the radar (when not following) moves the view centre
// itself rather than sliding a fixed window around home, so you can wander
// anywhere on earth and the fetch box goes with you — pan to Iceland and you
// see Iceland's traffic. Home stays exactly where it is; it's just drawn as an
// offset marker until you recentre.
static bool  browsing  = false;
static float browseLat = 0.0f;
static float browseLon = 0.0f;

static float viewLat() { return following ? followCenterLat : (browsing ? browseLat : homeLat()); }
static float viewLon() { return following ? followCenterLon : (browsing ? browseLon : homeLon()); }

// Display radius drives the on-screen map scale — the normal dial-controlled
// five-step range, whether following or not.
static float displayRadiusKm() {
    return ZOOM_STEPS[constrain(zoomIdx, 0, ZOOM_COUNT - 1)];
}

// Follow mode keeps its own, deliberately different pan: a clamped screen-pixel
// offset that shifts only what's *drawn*, leaving the fetch box on the tracked
// aircraft. That's the point of a chase view — the plane must stay findable, so
// this one stays bounded rather than becoming free browsing.
static void followPanDeltaLatLon(float& dLat, float& dLon) {
    dLat = dLon = 0.0f;
    if (!following || (followPanPxX == 0 && followPanPxY == 0)) return;
    float kmPerPx = displayRadiusKm() / 105.0f;   // PLOT_R
    float cl = cosf(viewLat() * (float)M_PI / 180.0f);
    if (cl < 0.05f) cl = 0.05f;
    dLat = (followPanPxY * kmPerPx) / 111.0f;
    dLon = -(followPanPxX * kmPerPx) / (111.0f * cl);
}
static float displayCenterLat() { float dLat, dLon; followPanDeltaLatLon(dLat, dLon); return viewLat() + dLat; }
static float displayCenterLon() { float dLat, dLon; followPanDeltaLatLon(dLat, dLon); return viewLon() + dLon; }

static void resetFollowPan() { followPanPxX = 0; followPanPxY = 0; }

// Back to home (or, while following, back onto the aircraft).
static void recentreView() {
    resetFollowPan();
    browsing = false;
}

// Shift the browse centre by a drag, in screen pixels. Unbounded on purpose:
// latitude is clamped only where the projection gives out near the poles, and
// longitude wraps, so you can drag right around the world.
static void browsePan(int dxPx, int dyPx) {
    if (!browsing) { browsing = true; browseLat = homeLat(); browseLon = homeLon(); }
    float kmPerPx = displayRadiusKm() / 105.0f;   // PLOT_R
    float cl = cosf(browseLat * (float)M_PI / 180.0f);
    if (cl < 0.05f) cl = 0.05f;                   // don't divide away near the poles
    browseLat += (dyPx * kmPerPx) / 111.0f;
    browseLon -= (dxPx * kmPerPx) / (111.0f * cl);
    if (browseLat >  85.0f) browseLat =  85.0f;
    if (browseLat < -85.0f) browseLat = -85.0f;
    while (browseLon >  180.0f) browseLon -= 360.0f;
    while (browseLon < -180.0f) browseLon += 360.0f;
}

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
    radar.setViewPanned(browsing || followPanPxX != 0 || followPanPxY != 0);
    // Tell the chase view when the target has been unseen long enough to be
    // worth saying so, rather than leaving a coasting mark looking like a hang.
    unsigned long lostMs = following ? (millis() - followLastSeenMs) : 0;
    radar.setFollowSignal(following && lostMs > FOLLOW_LOST_NOTICE_MS, lostMs / 1000);
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
    browsing  = false;   // back to home rather than wherever the chase ended
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
    // Triggers the emergency-squawk ring flash + buzzer directly, without a
    // real squawking aircraft (or the checkEmergency() cooldown) in the way —
    // for tuning/testing the alert itself.
    if (line == "BUZZ")      { radar.flashEmergencyRing(); return; }
    // Full simulation: injects a fake 7700-squawking aircraft near the current
    // view so it actually renders on the radar (red mark, ring, forced label)
    // exactly like a real one, then fires the same alert a real detection
    // would — including the auto-follow (see checkEmergency()). Bypasses the
    // cooldown so it always fires on request. Overwritten by the next
    // successful poll, same as any other stale entry.
    if (line == "SQUAWK") {
        Aircraft fake{};
        strncpy(fake.icao24,   "7DEBUG",  sizeof(fake.icao24) - 1);
        strncpy(fake.callsign, "TEST7700", sizeof(fake.callsign) - 1);
        strncpy(fake.squawk,   "7700",    sizeof(fake.squawk) - 1);
        strncpy(fake.country,  "TEST",    sizeof(fake.country) - 1);   // type field
        fake.lat      = viewLat() + 0.02f;   // offset so it isn't hidden under the home mark
        fake.lon      = viewLon() + 0.02f;
        fake.altM     = 3000.0f;
        fake.speedMs  = 120.0f;
        fake.heading  = 45.0f;
        fake.onGround = false;
        aircraft.push_back(fake);
        Serial.println("[Debug] injected simulated 7700 squawk");
        if (!following || strcmp(followIcao, fake.icao24) != 0)
            startFollow((int)aircraft.size() - 1);
        redraw();
        radar.flashEmergencyRing();
        return;
    }
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
    // Drives the free-browse centre the way a drag would, so browsing can be
    // exercised without a finger on the glass.
    if (line.startsWith("BROWSE:")) {
        int sep = line.indexOf(',', 7);
        if (sep > 0) {
            browsing  = true;
            browseLat = line.substring(7, sep).toFloat();
            browseLon = line.substring(sep + 1).toFloat();
            selectedAc = -1;
            lastFetchMs = 0;   // pull traffic for the new area immediately
            redraw();
        }
        return;
    }
    if (line == "RECENTRE" || line == "RECENTER") {
        recentreView(); lastFetchMs = 0; redraw(); return;
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
    // Raw power-IC readings behind the battery gauge's present/charging
    // decision — the gauge deliberately hides itself when these look
    // implausible, so this is how to tell "no battery" from "bad reading".
    if (line == "BATT") {
        Serial.printf("BATT level=%d voltage=%dmV current=%dmA charging=%d\n",
                      (int)M5.Power.getBatteryLevel(),
                      (int)M5.Power.getBatteryVoltage(),
                      (int)M5.Power.getBatteryCurrent(),
                      (int)M5.Power.isCharging());
        return;
    }
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

    for (int i = 0; i < (int)aircraft.size(); ++i) {
        const Aircraft& ac = aircraft[i];
        if (!ac.isEmergency()) continue;

        unsigned long now = millis();
        bool sameAc   = strcmp(ac.icao24, lastAlertIcao) == 0;
        bool cooldown  = sameAc && (now - lastAlertMs) < 60000UL;
        if (cooldown) continue;

        strncpy(lastAlertIcao, ac.icao24, sizeof(lastAlertIcao) - 1);
        lastAlertMs = now;

        Serial.printf("[Alert] Emergency squawk %s on %s\n",
                      ac.squawk, ac.icao24);

        // Lock onto it automatically — the whole point of an emergency alert
        // is that this is the one aircraft worth watching right now. No-op if
        // already following this exact aircraft, so a repeat alert on an
        // ongoing emergency doesn't yank the view/reset the follow-pan.
        if (!following || strcmp(followIcao, ac.icao24) != 0) startFollow(i);

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
    // Give the fetch the biggest contiguous heap this no-PSRAM board can
    // offer: release the map's PNG decode buffer if it's currently resident
    // (e.g. mid-precache-round — previously only released once the *whole*
    // round finished, which could leave it held for minutes) rather than
    // leaving it to compete with the TLS handshake below. Cheap and
    // idempotent — a later compose just re-primes it on demand.
    mapLayer.releaseDecoder();

    // While following, name the tracked aircraft so it survives the
    // MAX_AIRCRAFT cap even in dense airspace (see adsblive.h).
    feed::request(viewLat(), viewLon(), displayRadiusKm(),
                  following ? followIcao : nullptr);
}

// Store the followed aircraft's latest state, deriving a velocity when the feed
// doesn't give one.
//
// Some sources report a position with no ground speed at all (satellite and
// relayed reports do this routinely — every mid-Atlantic aircraft observed had
// gs missing). speedMs then reads 0, dead reckoning declines to move anything
// that slow, and the fetch box stops coasting — so the first coverage gap loses
// the aircraft, which is precisely when coasting matters most.
//
// Two consecutive fixes give the missing velocity directly. Anything outside a
// sane airliner range is ignored rather than trusted.
static void updateFollowSnapshot(const Aircraft& ac) {
    if (followHaveLastAc && !ac.onGround && ac.speedMs < INTERP_MIN_SPEED_MS) {
        float dtS = (float)(millis() - followLastAcMs) / 1000.0f;
        if (dtS > 1.0f) {
            float north = (ac.lat - followLastAc.lat) * 111.0f;
            float east  = (ac.lon - followLastAc.lon) * 111.0f *
                          cosf(ac.lat * (float)M_PI / 180.0f);
            float km    = sqrtf(north * north + east * east);
            float sp    = km * 1000.0f / dtS;
            if (sp >= INTERP_MIN_SPEED_MS && sp < 400.0f) {   // < ~1440 km/h
                followLastAc         = ac;
                followLastAc.speedMs = sp;
                float brg = atan2f(east, north) * 180.0f / (float)M_PI;
                followLastAc.heading = (brg < 0.0f) ? brg + 360.0f : brg;
                followLastAcMs       = millis();
                followHaveLastAc     = true;
                return;
            }
        }
    }
    followLastAc     = ac;
    followLastAcMs   = millis();
    followHaveLastAc = true;
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
            updateFollowSnapshot(aircraft[idx]);
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
    startWebConfig();    // always-on config page at http://flightradar.local
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
    webConfigLoop();     // serve the web config page a slice at a time

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
                int nx = radar.nextSelectable(selectedAc, dir, aircraft,
                                              displayCenterLat(), displayCenterLon(), r);
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
                recentreView();     // plain tap on empty space recentres
            }
            redraw();
        }
    } else if (touch.wasPressed()) {
        lastInteractionMs = millis();
        followTouchDownX = touch.x; followTouchDownY = touch.y;
        followTouchLastX = touch.x; followTouchLastY = touch.y;
        followTouchDragged = false;
    } else if (touch.isPressed()) {
        // Drag moves the view centre itself — free browsing, no bounds.
        int dx = touch.x - followTouchLastX, dy = touch.y - followTouchLastY;
        if (dx != 0 || dy != 0) {
            // Only count it as a drag once it's clearly past finger jitter,
            // or an ordinary tap would stop selecting aircraft.
            int tdx = touch.x - followTouchDownX, tdy = touch.y - followTouchDownY;
            if (tdx * tdx + tdy * tdy > 36) followTouchDragged = true;
            if (followTouchDragged) {
                browsePan(dx, dy);
                selectedAc = -1;          // the old index means nothing over there
                // Pull traffic for wherever we've landed, once the drag settles.
                zoomFetchPending = true;
                zoomFetchDueMs   = millis() + ZOOM_FETCH_DEBOUNCE_MS;
            }
            lastInteractionMs = millis();
            static unsigned long lastDragDrawMs = 0;
            unsigned long tnow = millis();
            if (tnow - lastDragDrawMs >= 80UL) { lastDragDrawMs = tnow; redraw(); }
        }
        followTouchLastX = touch.x; followTouchLastY = touch.y;
    } else if (touch.wasReleased() && !followTouchDragged) {
        // Hit-test the press-down position, for the same reason follow mode
        // does: the tracked position drifts during an ordinary tap.
        const int tx = followTouchDownX, ty = followTouchDownY;
        float r = displayRadiusKm();
        if (radar.hitSetHomeIcon(tx, ty) && browsing) {
            // Adopt the spot you've dragged to. The home crosshair jumping to
            // the centre is its own confirmation, and it's undone by dragging
            // somewhere else and tapping again.
            setHomeLocation(browseLat, browseLon);
            recentreView();
            zoomFetchPending = true;
            zoomFetchDueMs   = millis() + ZOOM_FETCH_DEBOUNCE_MS;
        } else if (radar.hitRecenterIcon(tx, ty) && browsing) {
            recentreView();
            zoomFetchPending = true;              // refetch around home
            zoomFetchDueMs   = millis() + ZOOM_FETCH_DEBOUNCE_MS;
        } else if (radar.hitPollIcon(tx, ty)) {
            // Tap the poll icon to toggle the API status panel
            radar.setStatusVisible(!radar.statusVisible());
            selectedAc = -1;
        } else if (selectedAc >= 0 && !radar.statusVisible() &&
                   radar.hitFollowButton(tx, ty)) {
            // FOLLOW button in the detail panel starts tracking (and hides the panel).
            startFollow(selectedAc);
            // Prime the follow handler's gesture state: this same physical
            // touch is already down, so it will never see a wasPressed() edge.
            followTouchOnButton = true;
            followTouchDragged  = false;
        } else if (selectedAc >= 0 && !radar.statusVisible() &&
                   radar.hitDetailPill(tx, ty)) {
            // Tap the detail pill itself (not the FOLLOW button above it) to
            // cycle to its next page.
            radar.advanceDetailPage();
        } else {
            radar.setStatusVisible(false);   // any other tap dismisses the panel
            // Hit-test against what's actually drawn, which is the panned
            // centre — not the unpanned one the fetch uses.
            int hit = radar.hitTest(tx, ty, aircraft,
                                    displayCenterLat(), displayCenterLon(), r);
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

    // Safety net: the feed was working at some point but hasn't landed a good
    // fetch in FEED_STUCK_RESTART_MS despite repeated retries — almost always
    // heap fragmentation after long uptime, which doesn't clear on its own.
    // Restart for a clean heap rather than leaving an unattended device
    // silently broken for hours.
    if (lastGoodFetchMs != 0 && now - lastGoodFetchMs > FEED_STUCK_RESTART_MS) {
        Serial.println("[Feed] stuck for 5 min with no good fetch — restarting for a clean heap");
        Serial.flush();
        delay(200);
        ESP.restart();
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
