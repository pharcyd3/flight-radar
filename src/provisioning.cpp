#include "provisioning.h"
#include "config.h"
#include "map.h"
#include "geolocate.h"
#include "settings.h"
#include <M5Dial.h>
#include <WiFiManager.h>
#include <Preferences.h>

// ── Stored credentials ────────────────────────────────────────────────────────
// OpenSky OAuth2 API-client credentials (client_id / client_secret).
static char _osUser[64] = "";   // client_id
static char _osPass[64] = "";   // client_secret

const char* openskyClientId()     { return _osUser; }
const char* openskyClientSecret() { return _osPass; }

void setOpenSkyCredentials(const char* clientId, const char* clientSecret) {
    strncpy(_osUser, clientId, sizeof(_osUser) - 1);
    _osUser[sizeof(_osUser) - 1] = '\0';
    strncpy(_osPass, clientSecret, sizeof(_osPass) - 1);
    _osPass[sizeof(_osPass) - 1] = '\0';

    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    prefs.putString("os_cid",    _osUser);
    prefs.putString("os_secret", _osPass);
    prefs.end();

    Serial.printf("[Provision] OpenSky client_id set via serial: %s\n", _osUser);
}

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
    // Load any previously saved OpenSky creds + home location from NVS
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    prefs.getString("os_cid",    _osUser, sizeof(_osUser));
    prefs.getString("os_secret", _osPass, sizeof(_osPass));
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

    // Custom portal fields (OpenSky OAuth2 creds optional, location pre-filled)
    WiFiManagerParameter osUserParam(
        "os_cid", "OpenSky client_id (optional)", _osUser, 63);
    WiFiManagerParameter osPassParam(
        "os_secret", "OpenSky client_secret (optional)", "", 63);

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
    wm.addParameter(&osUserParam);
    wm.addParameter(&osPassParam);
    wm.addParameter(&placeParam);
    wm.addParameter(&homeLatParam);
    wm.addParameter(&homeLonParam);

    // Show the setup screen while the portal is active
    wm.setAPCallback([](WiFiManager*) {
        showSetupScreen();
    });

    // Save OpenSky creds + home location to NVS when the user submits the form
    wm.setSaveParamsCallback([&]() {
        strncpy(_osUser, osUserParam.getValue(), sizeof(_osUser) - 1);
        strncpy(_osPass, osPassParam.getValue(), sizeof(_osPass) - 1);
        prefs.putString("os_cid",    _osUser);
        prefs.putString("os_secret", _osPass);

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

    bool connected = wm.autoConnect(SETUP_AP_SSID);
    prefs.end();

    if (!connected) {
        // Portal timed out without a connection — restart and try again
        Serial.println("[Provision] Timed out, restarting");
        delay(1000);
        ESP.restart();
    }

    Serial.printf("[Provision] Connected: %s  OpenSky client_id: %s  Home: %.6f, %.6f\n",
                  WiFi.localIP().toString().c_str(),
                  _osUser[0] ? _osUser : "(anonymous)",
                  _homeLat, _homeLon);

    // Now that we're online, geocode any typed place / IP-auto-detect if the user
    // didn't give explicit coordinates.
    resolvePendingLocation();
}

void runLocationPortal() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);

    // OpenSky OAuth2 creds — editable here so keys can be added/changed without
    // a factory reset. Secret prefills blank; leave blank to keep the stored one.
    WiFiManagerParameter osUserParam(
        "os_cid", "OpenSky client_id (optional)", _osUser, 63);
    WiFiManagerParameter osPassParam(
        "os_secret", "OpenSky client_secret (blank = keep)", "", 63);

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
    wm.addParameter(&osUserParam);
    wm.addParameter(&osPassParam);
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
        // OpenSky creds: client_id prefills current value so it persists;
        // client_secret prefills blank, so only overwrite when something's typed.
        strncpy(_osUser, osUserParam.getValue(), sizeof(_osUser) - 1);
        const char* newSecret = osPassParam.getValue();
        if (newSecret[0])
            strncpy(_osPass, newSecret, sizeof(_osPass) - 1);
        prefs.putString("os_cid",    _osUser);
        prefs.putString("os_secret", _osPass);

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
    wm.startConfigPortal(SETUP_AP_SSID);
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
// Wipes WiFi credentials, OpenSky login, home location, and saved favourites,
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
