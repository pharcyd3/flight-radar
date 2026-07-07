#include <M5Dial.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "aircraft.h"
#include "opensky.h"
#include "radar.h"
#include "map.h"
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

// Emergency alert cooldown — don't re-alert the same aircraft within 60 s
static char          lastAlertIcao[8] = "";
static unsigned long lastAlertMs      = 0;

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
        float r = ZOOM_STEPS[zoomIdx];
        radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);
        radar.flashEmergencyRing();
        break;  // one alert per check cycle even if multiple emergency a/c
    }
}

static void doFetch() {
    float r = ZOOM_STEPS[zoomIdx];

    // Flip the poll icon into its "request in flight" look immediately,
    // before the (possibly slow) network call blocks everything else.
    fetchInProgress = true;
    radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);

    fetchAircraft(homeLat(), homeLon(), r, aircraft);
    lastUpdateMs = millis();
    fetchInProgress = false;

    // Outcome (incl. failures) is surfaced by the poll icon colour and the
    // tap-to-view status panel, so just redraw the radar either way.
    radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);

    checkEmergency();
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
        runSettings(radar, aircraft, homeLat(), homeLon(), r, lastUpdateMs, fetchInProgress);
        // The map cache is content-addressed by (lat,lon,radius), so a changed
        // home location just means the next radar.draw()/ensure() loads or
        // composes a different cache entry — no need to wipe anything here.
        radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);
        return;
    }

    // ── Encoder: zoom in / out ───────────────────────────────────────────────
    // See encoder_debounce.h — this hardware needs more than a raw-tick ->
    // detent conversion to behave as a reliable "one click = one step" input.
    int zoomDelta;
    if (zoomEncoder.poll(&zoomDelta)) {
        zoomIdx    = constrain(zoomIdx + zoomDelta, 0, ZOOM_COUNT - 1);
        selectedAc = -1;
        doFetch();
        lastFetchMs = millis();
        return;  // redraw handled inside doFetch → we fall through on next loop
    }

    // ── Touch: select / deselect aircraft ────────────────────────────────────
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasPressed()) {
        float r = ZOOM_STEPS[zoomIdx];
        if (radar.hitPollIcon(touch.x, touch.y)) {
            // Tap the poll icon to toggle the API status panel
            radar.setStatusVisible(!radar.statusVisible());
            selectedAc = -1;
        } else {
            radar.setStatusVisible(false);   // any other tap dismisses the panel
            int hit = radar.hitTest(touch.x, touch.y, aircraft, homeLat(), homeLon(), r);
            selectedAc = (hit == selectedAc) ? -1 : hit;  // tap again to deselect
        }
        // Immediate redraw after touch so it feels responsive
        radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);
    }

    // ── Auto-refresh ──────────────────────────────────────────────────────────
    unsigned long now = millis();
    if (now - lastFetchMs >= refreshIntervalMs()) {
        lastFetchMs = now;
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        doFetch();
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
        radar.updatePollIcon(lastUpdateMs, fetchInProgress);
        radar.updateStatusOverlay();
    }
}
