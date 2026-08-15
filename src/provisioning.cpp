#include "provisioning.h"
#include "config.h"
#include "map.h"
#include "geolocate.h"
#include "settings.h"
#include "watchdog.h"
#include <M5Dial.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ── Stored home location ──────────────────────────────────────────────────────
static float _homeLat = DEFAULT_HOME_LAT;
static float _homeLon = DEFAULT_HOME_LON;

// Whether a home location has ever been explicitly chosen (manual coords, a
// geocoded place, an IP auto-detect, or a favourite). Distinguishes "user is
// deliberately in London" from "still on the first-boot default", so auto-detect
// only kicks in when the location was genuinely never set.
static bool _homeSet = false;

// A place name typed into the portal, stashed here by the (offline, AP-mode)
// save callback and resolved to coordinates once the device is back online.
static char _pendingPlace[80] = "";

float homeLat() { return _homeLat; }
float homeLon() { return _homeLon; }

void setHomeLocation(float lat, float lon) {
    _homeLat = lat;
    _homeLon = lon;
    _homeSet = true;
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    prefs.putFloat("home_lat", _homeLat);
    prefs.putFloat("home_lon", _homeLon);
    prefs.putBool("home_set", true);
    prefs.end();
}

// ── Saved favourite locations ─────────────────────────────────────────────────
static char  _favName[FAV_COUNT][24] = { "", "", "" };
static float _favLat[FAV_COUNT]      = { 0.0f, 0.0f, 0.0f };
static float _favLon[FAV_COUNT]      = { 0.0f, 0.0f, 0.0f };

const char* favName(int i) { return _favName[i]; }
float       favLat(int i)  { return _favLat[i]; }
float       favLon(int i)  { return _favLon[i]; }

void saveFavourite(int i, const char* name, float lat, float lon) {
    if (i < 0 || i >= FAV_COUNT) return;
    strncpy(_favName[i], name, sizeof(_favName[i]) - 1);
    _favName[i][sizeof(_favName[i]) - 1] = '\0';
    _favLat[i] = lat;
    _favLon[i] = lon;

    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    char key[16];
    snprintf(key, sizeof(key), "fav%d_name", i); prefs.putString(key, _favName[i]);
    snprintf(key, sizeof(key), "fav%d_lat",  i); prefs.putFloat(key, _favLat[i]);
    snprintf(key, sizeof(key), "fav%d_lon",  i); prefs.putFloat(key, _favLon[i]);
    prefs.end();
    Serial.printf("[Provision] Favourite %d saved: %s %.4f,%.4f\n", i, _favName[i], lat, lon);
}

// ── Display helpers (RGB565) ──────────────────────────────────────────────────
static constexpr uint16_t C_BG     = 0x0008;
static constexpr uint16_t C_GREEN  = 0x07E0;
static constexpr uint16_t C_ORANGE = 0xFD20;
static constexpr uint16_t C_GREY   = 0x7BEF;

static void showSetupScreen() {
    auto& d = M5Dial.Display;
    d.fillScreen(C_BG);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);

    d.setTextColor(C_GREEN, C_BG);
    d.drawString("SETUP MODE", 120, 70);

    d.setTextColor(C_GREY, C_BG);
    d.drawString("Connect your phone to:", 120, 100);

    d.setTextColor(C_ORANGE, C_BG);
    d.drawString(SETUP_AP_SSID, 120, 118);

    d.setTextColor(C_GREY, C_BG);
    d.drawString("then open:", 120, 142);
    d.drawString("192.168.4.1", 120, 158);
}

static void showConnectingScreen() {
    auto& d = M5Dial.Display;
    d.fillScreen(C_BG);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);
    d.setTextColor(C_GREY, C_BG);
    d.drawString("Connecting...", 120, 120);
}

// One/two-line status screen shared by the geolocation flows.
static void showGeoScreen(const char* line1, const char* line2 = nullptr) {
    auto& d = M5Dial.Display;
    d.fillScreen(C_BG);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);
    d.setTextColor(C_GREEN, C_BG);
    d.drawString("LOCATION", 120, 96);
    d.setTextColor(C_GREY, C_BG);
    d.drawString(line1, 120, 120);
    if (line2 && line2[0]) d.drawString(line2, 120, 138);
}

// After the portal closes and WiFi is back, turn whatever the user gave us into a
// home location: an explicitly typed place name wins; otherwise, if home was
// never set, fall back to IP auto-detect. No-op when neither applies (manual
// coordinates were already saved). Blocking, but only runs during setup.
static void resolvePendingLocation() {
    float lat, lon;
    char  place[80] = "";

    if (_pendingPlace[0]) {
        showGeoScreen("Finding place...");
        if (geocodeCity(_pendingPlace, lat, lon, place, sizeof(place))) {
            setHomeLocation(lat, lon);
            if (mapMode() == MAP_FULL) mapLayer.precacheAll(lat, lon);
            showGeoScreen("Found:", place);
        } else {
            showGeoScreen("Place not found", "keeping location");
        }
        delay(1400);
        _pendingPlace[0] = '\0';
        return;
    }

    if (!_homeSet) {
        showGeoScreen("Locating you...");
        if (ipGeolocate(lat, lon, place, sizeof(place))) {
            setHomeLocation(lat, lon);
            if (mapMode() == MAP_FULL) mapLayer.precacheAll(lat, lon);
            showGeoScreen("Located:", place);
            delay(1400);
        }
    }
}

// Settings-menu action: re-run IP auto-detect on demand (e.g. after moving).
// Returns true if a new home was set. Assumes the device is online.
bool runDetectLocation() {
    float lat, lon;
    char  place[80] = "";
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    showGeoScreen("Locating you...");
    if (ipGeolocate(lat, lon, place, sizeof(place))) {
        setHomeLocation(lat, lon);
        if (mapMode() == MAP_FULL) mapLayer.precacheAll(lat, lon);
        showGeoScreen("Located:", place[0] ? place : "position set");
        delay(1500);
        return true;
    }
    showGeoScreen("Detect failed", "check WiFi");
    delay(1500);
    return false;
}

// ── Provisioning ──────────────────────────────────────────────────────────────

void runProvisioning() {
    // Load any previously saved home location from NVS
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    _homeLat = prefs.getFloat("home_lat", DEFAULT_HOME_LAT);
    _homeLon = prefs.getFloat("home_lon", DEFAULT_HOME_LON);
    _homeSet = prefs.getBool("home_set", false);
    for (int i = 0; i < FAV_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), "fav%d_name", i);
        prefs.getString(key, _favName[i], sizeof(_favName[i]));
        snprintf(key, sizeof(key), "fav%d_lat", i);
        _favLat[i] = prefs.getFloat(key, 0.0f);
        snprintf(key, sizeof(key), "fav%d_lon", i);
        _favLon[i] = prefs.getFloat(key, 0.0f);
    }

    // First boot (home never set): leave the coordinate fields blank so the user
    // can just type a place name — or leave everything blank — and let the device
    // locate itself. On a re-run they prefill with the current coordinates.
    char latStr[16] = "", lonStr[16] = "";
    if (_homeSet) {
        snprintf(latStr, sizeof(latStr), "%.6f", _homeLat);
        snprintf(lonStr, sizeof(lonStr), "%.6f", _homeLon);
    }
    WiFiManagerParameter placeParam(
        "place", "Place name (e.g. Berlin) - or leave all blank to auto-detect", "", 48);
    WiFiManagerParameter homeLatParam(
        "home_lat", "Home latitude (optional, overrides place)", latStr, 15);
    WiFiManagerParameter homeLonParam(
        "home_lon", "Home longitude (optional, overrides place)", lonStr, 15);

    WiFiManager wm;
    wm.addParameter(&placeParam);
    wm.addParameter(&homeLatParam);
    wm.addParameter(&homeLonParam);

    // Show the setup screen while the portal is active
    wm.setAPCallback([](WiFiManager*) {
        showSetupScreen();
    });

    // Save home location to NVS when the user submits the form
    wm.setSaveParamsCallback([&]() {
        // A typed place name is an explicit choice, so it wins — stash it to
        // geocode once we're back online (we're offline in the portal AP now).
        // Otherwise use manually entered coordinates if they're valid.
        const char* placeVal = placeParam.getValue();
        if (placeVal[0]) {
            strncpy(_pendingPlace, placeVal, sizeof(_pendingPlace) - 1);
            _pendingPlace[sizeof(_pendingPlace) - 1] = '\0';
        } else {
            _pendingPlace[0] = '\0';
            float lat = atof(homeLatParam.getValue());
            float lon = atof(homeLonParam.getValue());
            bool valid = (lat >= -90.0f && lat <= 90.0f) &&
                         (lon >= -180.0f && lon <= 180.0f) &&
                         !(lat == 0.0f && lon == 0.0f);  // reject blank/unparsable input
            if (valid) {
                _homeLat = lat;
                _homeLon = lon;
                _homeSet = true;
                prefs.putFloat("home_lat", _homeLat);
                prefs.putFloat("home_lon", _homeLon);
                prefs.putBool("home_set", true);
            } else {
                Serial.println("[Provision] No place and invalid coords — will auto-detect");
            }
        }
    });

    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(300);  // 5 min before giving up and rebooting

    showConnectingScreen();

    // autoConnect() blocks for minutes waiting on the captive portal (its own
    // setConfigPortalTimeout() above bounds it) — without this, that legitimate
    // wait looks identical to a wedged loop and the software watchdog reboots
    // the device out from under the setup screen every ~60s.
    watchdogSuspend();
    bool connected = wm.autoConnect(SETUP_AP_SSID);
    watchdogResume();
    prefs.end();

    if (!connected) {
        // Portal timed out without a connection — restart and try again
        Serial.println("[Provision] Timed out, restarting");
        delay(1000);
        ESP.restart();
    }

    Serial.printf("[Provision] Connected: %s  Home: %.6f, %.6f\n",
                  WiFi.localIP().toString().c_str(),
                  _homeLat, _homeLon);

    // Now that we're online, geocode any typed place / IP-auto-detect if the user
    // didn't give explicit coordinates.
    resolvePendingLocation();
}

void runLocationPortal() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);

    char latStr[16], lonStr[16];
    snprintf(latStr, sizeof(latStr), "%.6f", _homeLat);
    snprintf(lonStr, sizeof(lonStr), "%.6f", _homeLon);
    WiFiManagerParameter placeParam(
        "place", "Change to a place (e.g. Berlin) - overrides coords below", "", 48);
    WiFiManagerParameter homeLatParam(
        "home_lat", "Home latitude (e.g. 51.5007)", latStr, 15);
    WiFiManagerParameter homeLonParam(
        "home_lon", "Home longitude (e.g. -0.1246)", lonStr, 15);

    // Favourite slots — leave name blank to clear a slot
    char favLatStr[FAV_COUNT][16], favLonStr[FAV_COUNT][16];
    char favLabelName[FAV_COUNT][32], favLabelLat[FAV_COUNT][32], favLabelLon[FAV_COUNT][32];
    WiFiManagerParameter* favNameParam[FAV_COUNT];
    WiFiManagerParameter* favLatParam[FAV_COUNT];
    WiFiManagerParameter* favLonParam[FAV_COUNT];
    char favIdName[FAV_COUNT][16], favIdLat[FAV_COUNT][16], favIdLon[FAV_COUNT][16];
    for (int i = 0; i < FAV_COUNT; i++) {
        snprintf(favLatStr[i], sizeof(favLatStr[i]), "%.6f", _favLat[i]);
        snprintf(favLonStr[i], sizeof(favLonStr[i]), "%.6f", _favLon[i]);
        snprintf(favLabelName[i], sizeof(favLabelName[i]), "Favourite %d name", i + 1);
        snprintf(favLabelLat[i], sizeof(favLabelLat[i]), "Favourite %d latitude", i + 1);
        snprintf(favLabelLon[i], sizeof(favLabelLon[i]), "Favourite %d longitude", i + 1);
        snprintf(favIdName[i], sizeof(favIdName[i]), "fav%d_name", i);
        snprintf(favIdLat[i], sizeof(favIdLat[i]), "fav%d_lat", i);
        snprintf(favIdLon[i], sizeof(favIdLon[i]), "fav%d_lon", i);
        favNameParam[i] = new WiFiManagerParameter(
            favIdName[i], favLabelName[i], _favName[i], 23);
        favLatParam[i] = new WiFiManagerParameter(
            favIdLat[i], favLabelLat[i], favLatStr[i], 15);
        favLonParam[i] = new WiFiManagerParameter(
            favIdLon[i], favLabelLon[i], favLonStr[i], 15);
    }

    WiFiManager wm;
    wm.addParameter(&placeParam);
    wm.addParameter(&homeLatParam);
    wm.addParameter(&homeLonParam);
    for (int i = 0; i < FAV_COUNT; i++) {
        wm.addParameter(favNameParam[i]);
        wm.addParameter(favLatParam[i]);
        wm.addParameter(favLonParam[i]);
    }
    wm.setAPCallback([](WiFiManager*) {
        showSetupScreen();
    });
    wm.setBreakAfterConfig(true);  // return once saved, don't linger in the portal

    wm.setSaveParamsCallback([&]() {

        // Typed place name wins (resolved after reconnect); otherwise use coords.
        const char* placeVal = placeParam.getValue();
        if (placeVal[0]) {
            strncpy(_pendingPlace, placeVal, sizeof(_pendingPlace) - 1);
            _pendingPlace[sizeof(_pendingPlace) - 1] = '\0';
        } else {
            _pendingPlace[0] = '\0';
            float lat = atof(homeLatParam.getValue());
            float lon = atof(homeLonParam.getValue());
            bool valid = (lat >= -90.0f && lat <= 90.0f) &&
                         (lon >= -180.0f && lon <= 180.0f) &&
                         !(lat == 0.0f && lon == 0.0f);
            if (valid) {
                _homeLat = lat;
                _homeLon = lon;
                _homeSet = true;
                prefs.putFloat("home_lat", _homeLat);
                prefs.putFloat("home_lon", _homeLon);
                prefs.putBool("home_set", true);
            } else {
                Serial.println("[Provision] Invalid home lat/lon, keeping previous value");
            }
        }

        for (int i = 0; i < FAV_COUNT; i++) {
            const char* fname = favNameParam[i]->getValue();
            float       flat  = atof(favLatParam[i]->getValue());
            float       flon  = atof(favLonParam[i]->getValue());
            bool fvalid = (flat >= -90.0f && flat <= 90.0f) &&
                          (flon >= -180.0f && flon <= 180.0f);

            if (fname[0] == '\0' || !fvalid) {
                _favName[i][0] = '\0';  // blank name (or bad coords) clears the slot
                _favLat[i] = 0.0f;
                _favLon[i] = 0.0f;
            } else {
                strncpy(_favName[i], fname, sizeof(_favName[i]) - 1);
                _favLat[i] = flat;
                _favLon[i] = flon;
            }

            char key[16];
            snprintf(key, sizeof(key), "fav%d_name", i);
            prefs.putString(key, _favName[i]);
            snprintf(key, sizeof(key), "fav%d_lat", i);
            prefs.putFloat(key, _favLat[i]);
            snprintf(key, sizeof(key), "fav%d_lon", i);
            prefs.putFloat(key, _favLon[i]);
        }
    });

    wm.setConfigPortalTimeout(180);  // 3 min before giving up and reconnecting

    showConnectingScreen();
    // Same reasoning as autoConnect() in runProvisioning(): this blocks for
    // minutes (bounded by setConfigPortalTimeout() above), which would
    // otherwise look like a hang to the software watchdog.
    watchdogSuspend();
    wm.startConfigPortal(SETUP_AP_SSID);
    watchdogResume();
    prefs.end();

    for (int i = 0; i < FAV_COUNT; i++) {
        delete favNameParam[i];
        delete favLatParam[i];
        delete favLonParam[i];
    }

    // The portal ran in AP mode — get back onto the home network before any
    // online geocoding / map precaching below.
    WiFi.mode(WIFI_STA);
    WiFi.reconnect();
    for (int t = 0; t < 40 && WiFi.status() != WL_CONNECTED; ++t) delay(250);

    // Resolve a typed place name (or nothing) now that we're back online.
    resolvePendingLocation();

    // Ensure every zoom level of the (possibly changed) home is composed up front
    // so there are no gaps when switching zoom/map mode. No-op for already-cached
    // levels; skipped unless the raster map is in use.
    if (mapMode() == MAP_FULL) mapLayer.precacheAll(_homeLat, _homeLon);

    // Pre-cache each saved favourite's map at the default zoom radius so
    // switching to one via Settings > Saved Locations is instant instead of
    // fetching OSM tiles live. Cheap no-op for favourites already cached.
    bool anyFav = false;
    for (int i = 0; i < FAV_COUNT; i++) if (_favName[i][0]) { anyFav = true; break; }
    if (anyFav) {
        M5Dial.Display.fillScreen(0x0000);
        M5Dial.Display.setTextDatum(middle_center);
        M5Dial.Display.setTextColor(0x7BEF, 0x0000);
        M5Dial.Display.drawString("Caching maps...", 120, 120);
        for (int i = 0; i < FAV_COUNT; i++) {
            if (_favName[i][0])
                mapLayer.precache(_favLat[i], _favLon[i], ZOOM_STEPS[ZOOM_DEFAULT]);
        }
    }

    Serial.printf("[Provision] Location updated: %.6f, %.6f\n", _homeLat, _homeLon);
}

// ── Factory reset ─────────────────────────────────────────────────────────────
// Wipes WiFi credentials, home location, and saved favourites,
// then reboots into first-boot provisioning. Does not return.
void factoryReset() {
    Serial.println("[Provision] Factory reset triggered");
    WiFiManager wm;
    wm.resetSettings();
    Preferences prefs;
    prefs.begin("flightdial", false);
    prefs.clear();
    prefs.end();
    delay(500);
    ESP.restart();
}

// ── Reset combo (3-second hold) / Settings trigger (short press) ─────────────

static bool _settingsRequest = false;

bool settingsRequested() {
    bool r = _settingsRequest;
    _settingsRequest = false;
    return r;
}

void checkResetCombo() {
    static unsigned long pressStart = 0;
    static bool          held       = false;

    bool down = M5Dial.BtnA.isPressed();

    if (down && !held) {
        pressStart = millis();
        held       = true;
    } else if (!down && held) {
        unsigned long duration = millis() - pressStart;
        if (duration >= 50UL && duration < 3000UL)
            _settingsRequest = true;   // short press → open settings
        held = false;
    }

    if (held && (millis() - pressStart) >= 3000UL) {
        factoryReset();
    }
}


// ── Always-on web config ──────────────────────────────────────────────────────
// The portal used to mean dropping the WiFi connection, standing up an AP, and
// blocking the device for up to 3 minutes. WiFiManager can instead run its web
// server against the existing station connection and be serviced incrementally,
// so the same page is reachable from any browser on the network at any time
// without interrupting the radar at all.
//
// Parameters are allocated once and kept for the life of the device: the portal
// is permanently live, so they cannot be stack locals the way the one-shot AP
// portal's were.

static WiFiManager*          _wcWm = nullptr;
static WiFiManagerParameter* _wcPlace = nullptr;
static WiFiManagerParameter* _wcLat   = nullptr;
static WiFiManagerParameter* _wcLon   = nullptr;
static WiFiManagerParameter* _wcFavName[FAV_COUNT] = { nullptr };
static WiFiManagerParameter* _wcFavLat [FAV_COUNT] = { nullptr };
static WiFiManagerParameter* _wcFavLon [FAV_COUNT] = { nullptr };
static bool _wcActive = false;
static char _wcAddress[64] = "";
static bool _wcApplyPending = false;   // set by the save callback, acted on in the loop
static bool _wcHomeChanged  = false;   // home moved: recentre and refetch at once

static const char WC_HOSTNAME[] = "flightradar";

// Buffers the parameters point at. WiFiManagerParameter keeps its own copy of
// the value, but these back the labels/ids, which must outlive construction.
static char _wcFavIdName[FAV_COUNT][16], _wcFavIdLat[FAV_COUNT][16], _wcFavIdLon[FAV_COUNT][16];
static char _wcFavLbName[FAV_COUNT][28], _wcFavLbLat[FAV_COUNT][28], _wcFavLbLon[FAV_COUNT][28];

// Display settings are rendered as <select> dropdowns built from the same list
// the on-device menu uses (settingsCycle* in settings.h). WiFiManagerParameter
// takes a bare pointer to this buffer, so it must be a fixed address that we
// rewrite in place rather than a String that could reallocate.
static char _wcCfgHtml[4096];
static WiFiManagerParameter* _wcCfg = nullptr;

static void wcBuildSettingsHtml() {
    int n = settingsCycleCount();
    size_t w = 0;
    w += snprintf(_wcCfgHtml + w, sizeof(_wcCfgHtml) - w,
                  "<h3>Display &amp; behaviour</h3>");
    for (int i = 0; i < n && w < sizeof(_wcCfgHtml) - 256; i++) {
        w += snprintf(_wcCfgHtml + w, sizeof(_wcCfgHtml) - w,
                      "<br/><label for='cfg%d'>%s</label>"
                      "<select id='cfg%d' name='cfg%d' style='width:100%%'>",
                      i, settingsCycleLabel(i), i, i);
        int cur = settingsCycleValue(i);
        for (int o = 0; o < settingsCycleOptionCount(i) && w < sizeof(_wcCfgHtml) - 96; o++) {
            w += snprintf(_wcCfgHtml + w, sizeof(_wcCfgHtml) - w,
                          "<option value='%d'%s>%s</option>",
                          o, (o == cur ? " selected" : ""), settingsCycleOption(i, o));
        }
        w += snprintf(_wcCfgHtml + w, sizeof(_wcCfgHtml) - w, "</select>");
    }
}

// Push the current in-memory values into the form fields, so the page always
// opens showing what the device actually holds — including changes made on the
// device itself (Set location, favourites) since boot.
static void wcRefreshFields() {
    if (!_wcActive) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.6f", _homeLat); _wcLat->setValue(buf, 15);
    snprintf(buf, sizeof(buf), "%.6f", _homeLon); _wcLon->setValue(buf, 15);
    for (int i = 0; i < FAV_COUNT; i++) {
        _wcFavName[i]->setValue(_favName[i], 23);
        snprintf(buf, sizeof(buf), "%.6f", _favLat[i]); _wcFavLat[i]->setValue(buf, 15);
        snprintf(buf, sizeof(buf), "%.6f", _favLon[i]); _wcFavLon[i]->setValue(buf, 15);
    }
}

static void wcSave() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);

    const char* placeVal = _wcPlace->getValue();
    if (placeVal[0]) {
        // Resolved in webConfigLoop(): geocoding blocks for seconds, and this
        // runs inside the web server's request handler.
        strncpy(_pendingPlace, placeVal, sizeof(_pendingPlace) - 1);
        _pendingPlace[sizeof(_pendingPlace) - 1] = '\0';
        _wcPlace->setValue("", 48);        // one-shot field; don't re-apply on next save
    } else {
        float lat = atof(_wcLat->getValue());
        float lon = atof(_wcLon->getValue());
        bool valid = (lat >= -90.0f && lat <= 90.0f) &&
                     (lon >= -180.0f && lon <= 180.0f) && !(lat == 0.0f && lon == 0.0f);
        if (valid) {
            _homeLat = lat; _homeLon = lon; _homeSet = true;
            prefs.putFloat("home_lat", _homeLat);
            prefs.putFloat("home_lon", _homeLon);
            prefs.putBool("home_set", true);
            _wcHomeChanged = true;   // make the radar jump there, not wait a poll
            Serial.printf("[WebConfig] home set to %.5f,%.5f\n", _homeLat, _homeLon);
        } else {
            Serial.printf("[WebConfig] rejected home lat/lon '%s','%s' — keeping previous\n",
                          _wcLat->getValue(), _wcLon->getValue());
        }
    }

    for (int i = 0; i < FAV_COUNT; i++) {
        const char* fname = _wcFavName[i]->getValue();
        float flat = atof(_wcFavLat[i]->getValue());
        float flon = atof(_wcFavLon[i]->getValue());
        bool fvalid = (flat >= -90.0f && flat <= 90.0f) && (flon >= -180.0f && flon <= 180.0f);
        if (fname[0] == '\0' || !fvalid) {
            _favName[i][0] = '\0'; _favLat[i] = 0.0f; _favLon[i] = 0.0f;
        } else {
            strncpy(_favName[i], fname, sizeof(_favName[i]) - 1);
            _favLat[i] = flat; _favLon[i] = flon;
        }
        char key[16];
        snprintf(key, sizeof(key), "fav%d_name", i); prefs.putString(key, _favName[i]);
        snprintf(key, sizeof(key), "fav%d_lat",  i); prefs.putFloat (key, _favLat[i]);
        snprintf(key, sizeof(key), "fav%d_lon",  i); prefs.putFloat (key, _favLon[i]);
    }
    prefs.end();

    // The display settings are raw <select> fields rather than
    // WiFiManagerParameters, so WiFiManager doesn't collect them — read them
    // off the request directly. Absent fields are simply left alone.
    for (int i = 0; i < settingsCycleCount(); i++) {
        char id[12];
        snprintf(id, sizeof(id), "cfg%d", i);
        if (_wcWm->server->hasArg(id))
            settingsApplyCycle(i, _wcWm->server->arg(id).toInt());
    }

    _wcApplyPending = true;
    Serial.println("[WebConfig] settings saved");
}

void startWebConfig() {
    if (_wcActive || WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WebConfig] not starting (active=%d wifi=%d)\n",
                      _wcActive, WiFi.status() == WL_CONNECTED);
        return;
    }

    char latStr[16], lonStr[16];
    snprintf(latStr, sizeof(latStr), "%.6f", _homeLat);
    snprintf(lonStr, sizeof(lonStr), "%.6f", _homeLon);

    // Raw-HTML pseudo-parameter: heads the Setup page so it explains itself,
    // rather than presenting bare fields under WiFiManager's generic title.
    static WiFiManagerParameter heading(
        "<h3>Home location &amp; favourites</h3>"
        "<p style='opacity:.75'>Saved straight to the device &mdash; no restart needed. "
        "Leave a favourite's name blank to clear that slot.</p>"
        "<p>Flash new firmware: connect the device by USB and open "
        "<a href='" FLASH_PAGE_URL "' target='_blank'>" FLASH_PAGE_URL "</a> "
        "in Chrome or Edge.</p>");
    _wcPlace = new WiFiManagerParameter(
        "place", "Place name (e.g. Berlin) - overrides the coordinates below", "", 48);
    _wcLat = new WiFiManagerParameter("home_lat", "Home latitude",  latStr, 15);
    _wcLon = new WiFiManagerParameter("home_lon", "Home longitude", lonStr, 15);

    _wcWm = new WiFiManager();
    wcBuildSettingsHtml();
    _wcCfg = new WiFiManagerParameter(_wcCfgHtml);
    _wcWm->addParameter(&heading);
    _wcWm->addParameter(_wcPlace);
    _wcWm->addParameter(_wcLat);
    _wcWm->addParameter(_wcLon);
    for (int i = 0; i < FAV_COUNT; i++) {
        char v[16];
        snprintf(_wcFavIdName[i], sizeof(_wcFavIdName[i]), "fav%d_name", i);
        snprintf(_wcFavIdLat [i], sizeof(_wcFavIdLat [i]), "fav%d_lat",  i);
        snprintf(_wcFavIdLon [i], sizeof(_wcFavIdLon [i]), "fav%d_lon",  i);
        snprintf(_wcFavLbName[i], sizeof(_wcFavLbName[i]), "Favourite %d name (blank clears)", i + 1);
        snprintf(_wcFavLbLat [i], sizeof(_wcFavLbLat [i]), "Favourite %d latitude",  i + 1);
        snprintf(_wcFavLbLon [i], sizeof(_wcFavLbLon [i]), "Favourite %d longitude", i + 1);
        _wcFavName[i] = new WiFiManagerParameter(_wcFavIdName[i], _wcFavLbName[i], _favName[i], 23);
        snprintf(v, sizeof(v), "%.6f", _favLat[i]);
        _wcFavLat[i]  = new WiFiManagerParameter(_wcFavIdLat[i],  _wcFavLbLat[i],  v, 15);
        snprintf(v, sizeof(v), "%.6f", _favLon[i]);
        _wcFavLon[i]  = new WiFiManagerParameter(_wcFavIdLon[i],  _wcFavLbLon[i],  v, 15);
        _wcWm->addParameter(_wcFavName[i]);
        _wcWm->addParameter(_wcFavLat[i]);
        _wcWm->addParameter(_wcFavLon[i]);
    }
    _wcWm->addParameter(_wcCfg);

    // Product branding, and a menu that leads with the settings the user
    // actually came for. "Configure WiFi" led the default menu even though
    // location is what this page is mostly used for.
    _wcWm->setTitle(PRODUCT_NAME);
    _wcWm->setHostname(WC_HOSTNAME);   // also what the page subtitle shows
    static const char* menu[] = { "param", "wifi", "info", "sep", "restart" };
    _wcWm->setMenu(menu, 5);
    _wcWm->setCustomHeadElement(
        "<style>"
        "body{background:#111;color:#eee}"
        "button,input[type=submit]{background:#0a7;border:0}"
        "a,h1,h3{color:#0c8}"
        ".msg{border-color:#0a7}"
        "</style>"
        // The params page button is labelled "Setup" by a PROGMEM string inside
        // WiFiManager; renaming it there would be lost on any library update, so
        // relabel it in the page instead.
        "<script>addEventListener('DOMContentLoaded',function(){"
        "document.querySelectorAll(\"form[action='/param'] button\")"
        ".forEach(function(b){b.textContent='Config';});});</script>");

    _wcWm->setConfigPortalBlocking(false);   // serviced from webConfigLoop()
    _wcWm->setSaveParamsCallback(wcSave);
    Serial.printf("[WebConfig] %d params registered, cfg html %u bytes\n",
                  _wcWm->getParametersCount(), (unsigned)strlen(_wcCfgHtml));
    _wcWm->startWebPortal();
    _wcActive = true;

    if (MDNS.begin(WC_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WebConfig] http://%s.local/\n", WC_HOSTNAME);
    } else {
        Serial.println("[WebConfig] mDNS failed — IP only");
    }
    snprintf(_wcAddress, sizeof(_wcAddress), "%s.local", WC_HOSTNAME);
    Serial.printf("[WebConfig] also http://%s/\n", WiFi.localIP().toString().c_str());
}

const char* webConfigAddress() {
    return (_wcActive && WiFi.status() == WL_CONNECTED) ? _wcAddress : nullptr;
}

void webConfigLoop() {
    if (!_wcActive) return;

    // Throttle the web server poll. ESP32's WebServer::handleClient() calls
    // delay(1) whenever no client is waiting, so servicing it every iteration
    // pinned the whole loop at ~1 kHz. That is still responsive, but it spends
    // a millisecond of every pass idling for no reason. Polling at ~200 Hz is
    // far more than a config page needs and leaves the loop free otherwise.
    static unsigned long lastProcessMs = 0;
    unsigned long now = millis();
    if (now - lastProcessMs < 5) return;
    lastProcessMs = now;

    _wcWm->process();

    if (!_wcApplyPending) return;
    _wcApplyPending = false;

    // A typed place name geocodes here rather than in the request handler —
    // it blocks for seconds and would stall the web server mid-response.
    if (_wcHomeChanged) {
        _wcHomeChanged = false;
        // A save that doesn't visibly do anything reads as a save that failed.
        // Drop any browse offset and force a fetch so the radar is centred on
        // the new home immediately rather than up to a refresh interval later.
        webConfigHomeApplied();
    }

    if (_pendingPlace[0]) {
        float glat, glon; char place[48] = "";
        showConnectingScreen();
        if (geocodeCity(_pendingPlace, glat, glon, place, sizeof(place))) {
            setHomeLocation(glat, glon);
            webConfigHomeApplied();
            Serial.printf("[WebConfig] '%s' -> %.4f,%.4f\n", _pendingPlace, glat, glon);
        } else {
            Serial.printf("[WebConfig] could not geocode '%s'\n", _pendingPlace);
        }
        _pendingPlace[0] = '\0';
    }
    wcRefreshFields();
    wcBuildSettingsHtml();   // reflect the new values next time the page opens
    if (mapMode() == MAP_FULL) mapLayer.precacheAll(_homeLat, _homeLon);
}
