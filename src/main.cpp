#include <M5Dial.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "aircraft.h"
#include "opensky.h"
#include "radar.h"
#include "provisioning.h"
#include "settings.h"

// ── State ────────────────────────────────────────────────────────────────────
static RadarDisplay         radar;
static std::vector<Aircraft> aircraft;

static int           zoomIdx      = ZOOM_DEFAULT;
static int           selectedAc   = -1;
static unsigned long lastFetchMs  = 0;
static unsigned long lastUpdateMs = 0;
static long          lastEncVal   = 0;
static bool          fetchInProgress = false;

// Emergency alert cooldown — don't re-alert the same aircraft within 60 s
static char          lastAlertIcao[8] = "";
static unsigned long lastAlertMs      = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

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

    bool ok = fetchAircraft(homeLat(), homeLon(), r, aircraft);
    lastUpdateMs = millis();
    fetchInProgress = false;

    if (!ok && aircraft.empty()) {
        radar.drawError("API error");
        delay(2000);
    } else {
        radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);
    }

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
    radar.drawBoot();
    delay(1500);

    runProvisioning();   // blocks until WiFi connected (runs portal if needed)
    loadSettings();
    doFetch();

    lastFetchMs  = millis();
    lastEncVal   = M5Dial.Encoder.read();
}

void loop() {
    M5Dial.update();
    checkResetCombo();

    if (settingsRequested()) {
        runSettings();
        // Redraw radar immediately after exiting settings
        float r = ZOOM_STEPS[zoomIdx];
        radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);
        return;
    }

    // ── Encoder: zoom in / out ───────────────────────────────────────────────
    long enc = M5Dial.Encoder.read();
    if (enc != lastEncVal) {
        int delta  = (enc > lastEncVal) ? 1 : -1;
        zoomIdx    = constrain(zoomIdx + delta, 0, ZOOM_COUNT - 1);
        lastEncVal = enc;
        selectedAc = -1;
        doFetch();
        lastFetchMs = millis();
        return;  // redraw handled inside doFetch → we fall through on next loop
    }

    // ── Touch: select / deselect aircraft ────────────────────────────────────
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasPressed()) {
        float r  = ZOOM_STEPS[zoomIdx];
        int   hit = radar.hitTest(touch.x, touch.y, aircraft, homeLat(), homeLon(), r);
        selectedAc = (hit == selectedAc) ? -1 : hit;  // tap again to deselect
        // Immediate redraw after touch so it feels responsive
        radar.draw(aircraft, homeLat(), homeLon(), r, selectedAc, lastUpdateMs, fetchInProgress);
    }

    // ── Auto-refresh ──────────────────────────────────────────────────────────
    unsigned long now = millis();
    if (now - lastFetchMs >= REFRESH_INTERVAL_MS) {
        lastFetchMs = now;
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        doFetch();
    }

    // ── Periodic redraw (1 Hz — keeps the "X s ago" counter ticking) ─────────
    static unsigned long lastDrawMs = 0;
    if (now - lastDrawMs >= 1000UL) {
        lastDrawMs = now;
        radar.draw(aircraft, homeLat(), homeLon(),
                   ZOOM_STEPS[zoomIdx], selectedAc, lastUpdateMs, fetchInProgress);
    }
}
