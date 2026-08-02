#include "settings.h"
#include "provisioning.h"
#include "config.h"
#include "radar.h"
#include "lofimap.h"
#include "map.h"
#include "encoder_debounce.h"
#include <M5Dial.h>
#include <WiFi.h>
#include <Preferences.h>

// ── Persisted state ───────────────────────────────────────────────────────────
// Each field is a small cycle index into the option-label arrays declared
// alongside MENU_ITEMS below.
struct SettingsState {
    uint8_t labels        = 1;  // Off/Selected/All        — default: Selected
    uint8_t theme         = 0;  // Radar/Amber/Ocean/Neon
    uint8_t units         = 0;  // ft·kts / m·km/h
    uint8_t filter        = 0;  // Airborne/All
    uint8_t minalt        = 0;  // Off/1k/5k/10k/20k ft
    uint8_t trails        = 0;  // On/Off                  — 0 = On
    uint8_t rings         = 0;  // On/Off                  — 0 = On
    uint8_t map           = MAP_LOFI;  // Full/Lo-fi/Off    — default Lo-fi
    uint8_t refresh       = REFRESH_DEFAULT;
    uint8_t buzzEmergency = 0;  // On/Off                  — 0 = On (matches prior default true)
    uint8_t iconStyle     = 0;  // Dot/Plane                — 0 = Dot
};
static SettingsState _s;

bool buzzOnEmergency()        { return _s.buzzEmergency == 0; }
int  flightLabels()           { return _s.labels; }
int  activeTheme()            { return _s.theme; }
int  activeUnits()            { return _s.units; }
int  trafficFilter()          { return _s.filter; }
float minAltitudeM()          { return MIN_ALT_OPTIONS_M[_s.minalt]; }
bool  showTrails()            { return _s.trails == 0; }
bool  showRings()             { return _s.rings == 0; }
int   aircraftIconStyle()     { return _s.iconStyle; }
int   mapMode()               { return _s.map <= MAP_OFF ? _s.map : MAP_FULL; }
void  setMapMode(int m)       { if (m >= 0 && m <= MAP_OFF) _s.map = (uint8_t)m; }
unsigned long refreshIntervalMs() {
    int i = (_s.refresh < REFRESH_OPTION_COUNT) ? _s.refresh : REFRESH_DEFAULT;
    return REFRESH_OPTIONS_MS[i];
}

void loadSettings() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/true);
    _s.labels        = prefs.getUChar("s_labels",  1);
    _s.theme         = prefs.getUChar("s_theme",   0);
    _s.units         = prefs.getUChar("s_units",   0);
    _s.filter        = prefs.getUChar("s_filter",  0);
    _s.minalt        = prefs.getUChar("s_minalt",  0);
    _s.trails        = prefs.getUChar("s_trails",  0);
    _s.rings         = prefs.getUChar("s_rings",   0);
    // Key bumped to "s_map3": the default is now Lo-fi (offline, no tile fetching
    // — far more robust than the raster map on this no-PSRAM board). Bumping the
    // key flips existing devices to the new default too; users who prefer Full can
    // still switch back in the menu (which persists under this key).
    _s.map           = prefs.getUChar("s_map3",    MAP_LOFI);
    // Key bumped to "s_refresh3": the refresh options are now a flat list of
    // fixed intervals (airplanes.live has no quota tiers for "Auto" to adapt
    // between), replacing the old Auto/10s/20s/30s set. Bumping the key gives
    // every device the new 8 s default rather than reinterpreting an old index.
    _s.refresh       = prefs.getUChar("s_refresh3", REFRESH_DEFAULT);
    _s.buzzEmergency = prefs.getUChar("s_buzz",    0);
    _s.iconStyle     = prefs.getUChar("s_icon",    0);
    prefs.end();
}

static void saveSettings() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    prefs.putUChar("s_labels",  _s.labels);
    prefs.putUChar("s_theme",  _s.theme);
    prefs.putUChar("s_units",  _s.units);
    prefs.putUChar("s_filter", _s.filter);
    prefs.putUChar("s_minalt", _s.minalt);
    prefs.putUChar("s_trails", _s.trails);
    prefs.putUChar("s_rings",  _s.rings);
    prefs.putUChar("s_map3",   _s.map);
    prefs.putUChar("s_refresh3",_s.refresh);
    prefs.putUChar("s_buzz",   _s.buzzEmergency);
    prefs.putUChar("s_icon",  _s.iconStyle);
    prefs.end();
}

// ── Colour palette (RGB565) — settings panels always use this fixed look,
// independent of the user's chosen radar theme, so the menu stays legible
// regardless of which theme is active underneath it ───────────────────────
static constexpr uint16_t S_GREEN   = 0x07E0;
static constexpr uint16_t S_ORANGE  = 0xFD20;
static constexpr uint16_t S_GREY    = 0x7BEF;
static constexpr uint16_t S_RED     = 0xF800;
static constexpr uint16_t S_OVERLAY = 0x0861;   // panel background

// Floating panel geometry — a "little box" over the live radar/map, centred
// on screen instead of a full-screen wipe.
static constexpr int PANEL_X = 20, PANEL_Y = 62, PANEL_W = 200, PANEL_H = 116;
static constexpr int PANEL_CX = PANEL_X + PANEL_W / 2;

// Settings panels composite into the shared map sprite (like the radar) and are
// pushed in a single transfer — flicker-free, and captureable for the manual
// (the GC9A01 panel can't be read back). Falls back to the display if no sprite.
static LovyanGFX* panelG()  { return mapLayer.ready() ? (LovyanGFX*)mapLayer.sprite()
                                                       : &M5Dial.Display; }
static void       panelShow() { if (mapLayer.ready()) mapLayer.pushScene(); }

// Redraws the live radar (map, rings, aircraft, poll icon) with no selection,
// so settings panels sit on top of current, real content instead of a blank
// screen — callers draw their panel immediately after this each frame.
static void drawBackdrop(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                          float homeLat, float homeLon, float radiusKm, int zoomIdx,
                          unsigned long lastUpdateMs, bool fetching) {
    radar.draw(aircraft, homeLat, homeLon, radiusKm, zoomIdx, /*selectedIdx=*/-1,
               lastUpdateMs, fetching);
}

static void panelFrame(const char* title, int page = 0, int numPages = 0) {
    auto& d = *panelG();
    d.fillRoundRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 8, S_OVERLAY);
    d.drawRoundRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 8, S_ORANGE);

    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);
    d.setTextColor(S_GREEN, S_OVERLAY);
    if (numPages > 1) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %d/%d", title, page + 1, numPages);
        d.drawString(buf, PANEL_CX, PANEL_Y + 14);
    } else {
        d.drawString(title, PANEL_CX, PANEL_Y + 14);
    }
}

// True once per tap anywhere on screen — every settings panel treats any tap
// as "close/cancel", since touch isn't otherwise bound to menu actions and
// there was previously no way to leave the menu short of a 30 s timeout.
static bool tappedToExit() {
    return M5Dial.Touch.getDetail().wasPressed();
}

// ── Generic cycle-item menu ───────────────────────────────────────────────────

static const char* OPTS_LABELS[]  = { "Off", "Selected", "All" };
static const char* OPTS_THEME[]   = { "Radar", "Amber", "Ocean", "Neon" };
static const char* OPTS_UNITS[]   = { "ft/kts", "m/km/h" };
static const char* OPTS_FILTER[]  = { "Airborne", "All" };
static const char* OPTS_MINALT[]  = { "Off", "1,000ft", "5,000ft", "10,000ft", "20,000ft" };
static const char* OPTS_ONOFF[]   = { "On", "Off" };
static const char* OPTS_MAP[]     = { "Full", "Lo-fi", "Off" };
static const char* OPTS_REFRESH[] = { "5s", "8s", "15s", "30s" };
static const char* OPTS_ICON[]    = { "Dot", "Plane" };

enum class ItemKind : uint8_t { Cycle, Action, Danger };

// What an Action/Danger row actually does. Named rather than inferred from the
// row's position: the dispatch below used to index backwards from the end of
// MENU_ITEMS, so inserting any new action silently shifted every branch onto
// the wrong handler.
enum class MenuAction : uint8_t {
    None, SetLocation, DetectLocation, LocationPortal, SavedLocations,
    PowerOff, FactoryReset,
};

struct MenuItem {
    const char*       label;
    ItemKind          kind;
    const char* const* options;   // Cycle only
    int               optionCount;
    uint8_t*          value;      // Cycle only — points into _s
    MenuAction        action;     // Action/Danger only
};

static void runLocationPicker(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                               float homeLat, float homeLon, float radiusKm, int zoomIdx,
                               unsigned long lastUpdateMs, bool fetching);
static void runFactoryResetConfirm(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                                    float homeLat, float homeLon, float radiusKm, int zoomIdx,
                                    unsigned long lastUpdateMs, bool fetching);
static void runSetLocation(RadarDisplay& radar);

// "Web Config" — the page is already live on the home network, so this just
// says where to find it rather than tearing down WiFi to stand up an AP. If
// there's no connection there is nothing to point at, so it falls back to the
// old AP portal, which is also the only way to enter new WiFi credentials.
static void runWebConfigInfo(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                             float homeLat, float homeLon, float radiusKm, int zoomIdx,
                             unsigned long lastUpdateMs, bool fetching) {
    const char* addr = webConfigAddress();
    if (!addr) { runLocationPortal(); return; }

    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, zoomIdx, lastUpdateMs, fetching);
    panelFrame("WEB CONFIG");
    auto& d = *panelG();
    d.setTextDatum(MC_DATUM);
    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("Open in any browser", PANEL_CX, PANEL_Y + 36);
    d.setTextColor(S_GREEN, S_OVERLAY);
    d.drawString(addr, PANEL_CX, PANEL_Y + 56);
    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString(WiFi.localIP().toString().c_str(), PANEL_CX, PANEL_Y + 74);
    d.drawString("press or tap to close", PANEL_CX, PANEL_Y + PANEL_H - 12);
    panelShow();

    unsigned long until = millis() + 30000;
    delay(250);
    while (millis() < until) {
        M5Dial.update();
        webConfigLoop();                       // keep serving while this is shown
        if (M5Dial.BtnA.wasReleased() || tappedToExit()) return;
        delay(20);
    }
}

// Cuts power by releasing the M5Dial's power-hold latch (GPIO46), which
// M5.Power.powerOff() pulses for us. This only truly switches off when running
// on battery — with USB attached the port keeps supplying power, so the device
// comes straight back up. Press the side button to switch on again.
static void runPowerOff() {
    auto& d = *panelG();
    d.fillRoundRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 8, S_OVERLAY);
    d.drawRoundRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 8, S_ORANGE);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);
    d.setTextColor(S_GREEN, S_OVERLAY);
    d.drawString("Powering off...", PANEL_CX, PANEL_Y + 44);
    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("side button to switch on", PANEL_CX, PANEL_Y + 66);
    panelShow();
    delay(1200);
    M5.Power.powerOff();
    // Reached only on USB power, where the latch can't cut the supply.
    delay(400);
}

static const MenuItem MENU_ITEMS[] = {
    { "Flight labels",       ItemKind::Cycle,  OPTS_LABELS,  3, &_s.labels , MenuAction::None },
    { "Colour theme",        ItemKind::Cycle,  OPTS_THEME,   4, &_s.theme , MenuAction::None },
    { "Units",               ItemKind::Cycle,  OPTS_UNITS,   2, &_s.units , MenuAction::None },
    { "Traffic",             ItemKind::Cycle,  OPTS_FILTER,  2, &_s.filter , MenuAction::None },
    { "Min altitude",        ItemKind::Cycle,  OPTS_MINALT,  5, &_s.minalt , MenuAction::None },
    { "Heading trails",      ItemKind::Cycle,  OPTS_ONOFF,   2, &_s.trails , MenuAction::None },
    { "Range rings",         ItemKind::Cycle,  OPTS_ONOFF,   2, &_s.rings , MenuAction::None },
    { "Map",                 ItemKind::Cycle,  OPTS_MAP,     3, &_s.map , MenuAction::None },
    { "Refresh rate",        ItemKind::Cycle,  OPTS_REFRESH, REFRESH_OPTION_COUNT, &_s.refresh , MenuAction::None },
    { "Buzz on Emergency",   ItemKind::Cycle,  OPTS_ONOFF,   2, &_s.buzzEmergency , MenuAction::None },
    { "Aircraft icon",       ItemKind::Cycle,  OPTS_ICON,    2, &_s.iconStyle , MenuAction::None },
    { "Set location",        ItemKind::Action, nullptr,      0, nullptr, MenuAction::SetLocation },
    { "Detect location",     ItemKind::Action, nullptr,      0, nullptr, MenuAction::DetectLocation },
    { "Web Config",          ItemKind::Action, nullptr,      0, nullptr, MenuAction::LocationPortal },
    { "Saved Locations",     ItemKind::Action, nullptr,      0, nullptr, MenuAction::SavedLocations },
    { "Power Off",           ItemKind::Action, nullptr,      0, nullptr, MenuAction::PowerOff },
    { "Factory Reset",       ItemKind::Danger, nullptr,      0, nullptr, MenuAction::FactoryReset },
};
static const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// ── Web-config access ─────────────────────────────────────────────────────────
// Projects the Cycle rows of MENU_ITEMS, so the web page and the on-device menu
// are literally the same list. Indices are positional and only need to stay
// stable between rendering a form and receiving it back, which they do.

static int cycleRowIndex(int nth) {
    int seen = 0;
    for (int i = 0; i < (int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])); i++) {
        if (MENU_ITEMS[i].kind != ItemKind::Cycle) continue;
        if (seen++ == nth) return i;
    }
    return -1;
}

int settingsCycleCount() {
    int n = 0;
    for (int i = 0; i < (int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])); i++)
        if (MENU_ITEMS[i].kind == ItemKind::Cycle) n++;
    return n;
}

const char* settingsCycleLabel(int i) {
    int r = cycleRowIndex(i);
    return (r < 0) ? "" : MENU_ITEMS[r].label;
}

int settingsCycleValue(int i) {
    int r = cycleRowIndex(i);
    return (r < 0) ? 0 : *MENU_ITEMS[r].value;
}

int settingsCycleOptionCount(int i) {
    int r = cycleRowIndex(i);
    return (r < 0) ? 0 : MENU_ITEMS[r].optionCount;
}

const char* settingsCycleOption(int i, int opt) {
    int r = cycleRowIndex(i);
    if (r < 0 || opt < 0 || opt >= MENU_ITEMS[r].optionCount) return "";
    return MENU_ITEMS[r].options[opt];
}

void settingsApplyCycle(int i, int value) {
    int r = cycleRowIndex(i);
    if (r < 0 || value < 0 || value >= MENU_ITEMS[r].optionCount) return;
    if (*MENU_ITEMS[r].value == (uint8_t)value) return;
    *MENU_ITEMS[r].value = (uint8_t)value;
    saveSettings();
}

// Rows per page — 3 fits the panel height alongside its title and hint line.
static const int MENU_PAGE_SIZE = 3;

static void drawMenuPanel(int idx) {
    int numPages = (MENU_COUNT + MENU_PAGE_SIZE - 1) / MENU_PAGE_SIZE;
    int page     = idx / MENU_PAGE_SIZE;
    int start    = page * MENU_PAGE_SIZE;
    int end      = start + MENU_PAGE_SIZE;
    if (end > MENU_COUNT) end = MENU_COUNT;

    panelFrame("SETTINGS", page, numPages);
    auto& d = *panelG();

    for (int i = start; i < end; i++) {
        int slot = i - start;
        int y    = PANEL_Y + 40 + slot * 22;
        const MenuItem& item = MENU_ITEMS[i];
        bool sel = (i == idx);

        d.setTextDatum(ML_DATUM);
        d.setTextColor(sel ? S_ORANGE : S_GREY, S_OVERLAY);
        d.drawString(item.label, PANEL_X + 10, y);

        d.setTextDatum(MR_DATUM);
        const char* valTxt;
        uint16_t    valCol = sel ? S_GREEN : S_GREY;
        switch (item.kind) {
            case ItemKind::Cycle:  valTxt = item.options[*item.value]; break;
            case ItemKind::Danger: valTxt = "> reset"; valCol = sel ? S_RED : S_GREY; break;
            default:                valTxt = "> edit";  break;
        }
        d.setTextColor(valCol, S_OVERLAY);
        d.drawString(valTxt, PANEL_X + PANEL_W - 10, y);
    }

    d.setTextDatum(MC_DATUM);
    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("rotate=move  press=select", PANEL_CX, PANEL_Y + PANEL_H - 18);
    d.drawString("tap=close", PANEL_CX, PANEL_Y + PANEL_H - 6);
    panelShow();
}

static void drawLocationPickerPanel(int idx) {
    panelFrame("SAVED LOCATIONS");
    auto& d = *panelG();
    d.setTextDatum(MC_DATUM);

    for (int i = 0; i < FAV_COUNT; i++) {
        bool set = favName(i)[0] != '\0';
        int  y   = PANEL_Y + 40 + i * 22;
        d.setTextColor(i == idx ? S_ORANGE : S_GREY, S_OVERLAY);
        char buf[32];
        if (set) snprintf(buf, sizeof(buf), "%s", favName(i));
        else     snprintf(buf, sizeof(buf), "Favourite %d (empty)", i + 1);
        d.drawString(buf, PANEL_CX, y);
    }

    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("rotate=move  press=activate", PANEL_CX, PANEL_Y + PANEL_H - 18);
    d.drawString("tap=cancel", PANEL_CX, PANEL_Y + PANEL_H - 6);
    panelShow();
}

// Rotate to pick a saved favourite, press to make it the active home location.
// Empty slots are shown but do nothing when selected. Tap or timeout cancels.
static void runLocationPicker(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                               float homeLat, float homeLon, float radiusKm, int zoomIdx,
                               unsigned long lastUpdateMs, bool fetching) {
    int idx = 0;
    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, zoomIdx, lastUpdateMs, fetching);
    drawLocationPickerPanel(idx);

    EncoderDebouncer enc;
    enc.begin();
    unsigned long lastActivity = millis();

    while (true) {
        M5Dial.update();

        int delta;
        if (enc.poll(&delta)) {
            idx += delta;
            if (idx < 0) idx = 0;
            if (idx > FAV_COUNT - 1) idx = FAV_COUNT - 1;
            lastActivity = millis();
            // Panel is fully opaque and repaints its whole area every call,
            // so it doesn't need the backdrop (a full map-sprite re-push)
            // redrawn first — that was the "flashes on every step" bug.
            drawLocationPickerPanel(idx);
        }

        if (M5Dial.BtnA.wasReleased()) {
            if (favName(idx)[0] != '\0') {
                setHomeLocation(favLat(idx), favLon(idx));
                if (mapMode() == MAP_FULL) mapLayer.precacheAll(favLat(idx), favLon(idx));
            }
            return;
        }

        if (tappedToExit()) return;

        // Auto-exit after 30 s of no interaction
        if (millis() - lastActivity > 30000UL) return;

        delay(20);
    }
}

static void drawFactoryResetPanel(unsigned long heldMs) {
    panelFrame("FACTORY RESET");
    auto& d = *panelG();
    d.setTextDatum(MC_DATUM);

    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("Erases WiFi credentials,", PANEL_CX, PANEL_Y + 38);
    d.drawString("location & favourites", PANEL_CX, PANEL_Y + 52);

    // Progress ring fills as the button is held
    int cy = PANEL_Y + 78;
    float frac = (float)heldMs / 3000.0f;
    if (frac > 1.0f) frac = 1.0f;
    d.drawArc(PANEL_CX, cy, 16, 12, 0, 360, S_GREY);
    if (frac > 0.01f) d.drawArc(PANEL_CX, cy, 16, 12, 0, frac * 360.0f, S_ORANGE);

    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("hold=confirm  tap/release=cancel", PANEL_CX, PANEL_Y + PANEL_H - 8);
    panelShow();
}

// Hold the button 3 s to confirm, matching the same gesture as the physical
// reset combo; release early, tap, or leave it untouched (timeout) to cancel.
static void runFactoryResetConfirm(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                                    float homeLat, float homeLon, float radiusKm, int zoomIdx,
                                    unsigned long lastUpdateMs, bool fetching) {
    unsigned long pressStart   = 0;
    bool          held         = false;
    unsigned long lastActivity = millis();

    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, zoomIdx, lastUpdateMs, fetching);
    drawFactoryResetPanel(0);

    // Swallow any residual button state from the press that opened us
    delay(200);
    while (M5Dial.BtnA.isPressed()) {
        M5Dial.update();
        delay(10);
    }
    delay(50);

    while (true) {
        M5Dial.update();

        bool down = M5Dial.BtnA.isPressed();
        if (down && !held) {
            pressStart   = millis();
            held         = true;
            lastActivity = millis();
        } else if (down && held) {
            unsigned long heldMs = millis() - pressStart;
            // Panel is fully opaque and repaints its whole area every call,
            // so it doesn't need the backdrop redrawn first — this runs on
            // every loop iteration while held, so that was a bad flicker.
            drawFactoryResetPanel(heldMs);
            if (heldMs >= 3000UL) {
                factoryReset();  // does not return — reboots the device
            }
        } else if (!down && held) {
            return;  // released early — cancel
        }

        if (tappedToExit()) return;

        // Auto-cancel after 15 s of no interaction
        if (millis() - lastActivity > 15000UL) return;

        delay(20);
    }
}

// ── On-device "Set location" (drag the map, tap SET) ─────────────────────────

// Pan/zoom radii for the set-location map, coarse (find your country) to fine
// (place within a town). Rotating the dial steps through these.
static const float SETLOC_RADII[] = { 1500.0f, 700.0f, 300.0f, 120.0f, 50.0f, 20.0f };
static const int   SETLOC_COUNT   = sizeof(SETLOC_RADII) / sizeof(SETLOC_RADII[0]);

// SET / SAVE buttons along the bottom (kept inside the round bezel's chord).
static bool hitSetBtn(int x, int y) { return x >= 42 && x <= 116 && y >= 198 && y <= 224; }
static bool hitFavBtn(int x, int y) { return x >= 124 && x <= 198 && y >= 198 && y <= 224; }

// Buttons + readout composited into the pan-map sprite, then pushed in one frame.
static void drawSetLocationOverlay(float lat, float lon, float km, const char* city) {
    auto& d = *panelG();
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);

    // Top info pill: nearest city + coordinates + current pan radius.
    d.fillRoundRect(22, 6, 196, 34, 6, S_OVERLAY);
    d.setTextColor(S_GREEN, S_OVERLAY);
    char buf[40];
    snprintf(buf, sizeof(buf), "near %s", city[0] ? city : "?");
    d.drawString(buf, 120, 17);
    d.setTextColor(S_GREY, S_OVERLAY);
    snprintf(buf, sizeof(buf), "%.3f, %.3f  %.0fkm", lat, lon, km);
    d.drawString(buf, 120, 31);

    // Bottom buttons.
    d.fillRoundRect(42, 198, 74, 26, 5, S_OVERLAY);
    d.drawRoundRect(42, 198, 74, 26, 5, S_GREEN);
    d.setTextColor(S_GREEN, S_OVERLAY);
    d.drawString("SET HOME", 79, 211);

    d.fillRoundRect(124, 198, 74, 26, 5, S_OVERLAY);
    d.drawRoundRect(124, 198, 74, 26, 5, S_ORANGE);
    d.setTextColor(S_ORANGE, S_OVERLAY);
    d.drawString("SAVE FAV", 161, 211);
    panelShow();
}

// Modal chooser: tap a slot to store (lat,lon) there; tap outside / button to cancel.
static void runFavSlotChooser(float lat, float lon) {
    auto& d = M5Dial.Display;
    while (true) {
        M5Dial.update();

        d.fillRoundRect(30, 66, 180, 108, 8, S_OVERLAY);
        d.drawRoundRect(30, 66, 180, 108, 8, S_ORANGE);
        d.setTextDatum(MC_DATUM);
        d.setTextSize(1);
        d.setTextColor(S_GREEN, S_OVERLAY);
        d.drawString("Save to which slot?", 120, 80);
        for (int i = 0; i < FAV_COUNT; i++) {
            int by = 100 + i * 22;
            d.drawRoundRect(44, by - 9, 152, 18, 4, S_GREY);
            char buf[40];
            snprintf(buf, sizeof(buf), "Favourite %d  %s",
                     i + 1, favName(i)[0] ? "(replace)" : "(empty)");
            d.setTextColor(S_GREY, S_OVERLAY);
            d.drawString(buf, 120, by);
        }

        auto t = M5Dial.Touch.getDetail();
        if (t.wasPressed()) {
            for (int i = 0; i < FAV_COUNT; i++) {
                int by = 100 + i * 22;
                if (t.x >= 44 && t.x <= 196 && t.y >= by - 9 && t.y <= by + 9) {
                    char nm[24];
                    snprintf(nm, sizeof(nm), "Favourite %d", i + 1);
                    saveFavourite(i, nm, lat, lon);
                    d.fillRoundRect(30, 66, 180, 108, 8, S_OVERLAY);
                    d.setTextColor(S_GREEN, S_OVERLAY);
                    d.drawString(nm, 120, 112);
                    d.drawString("saved", 120, 128);
                    delay(800);
                    return;
                }
            }
            // Tapped outside the panel — cancel.
            if (!(t.x >= 30 && t.x <= 210 && t.y >= 66 && t.y <= 174)) return;
        }
        if (M5Dial.BtnA.wasReleased()) return;
        delay(20);
    }
}

static void runSetLocation(RadarDisplay& radar) {
    float lat = homeLat();
    float lon = homeLon();
    int   idx = 2;   // start ~300 km (region level)

    EncoderDebouncer enc;
    enc.begin();

    // Swallow the button press that opened this screen.
    delay(200);
    while (M5Dial.BtnA.isPressed()) { M5Dial.update(); delay(10); }

    int  lastX = 0, lastY = 0, downX = 0, downY = 0;
    bool onButton = false, moved = false;
    bool dirty = true;
    char city[24] = "";
    unsigned long lastActivity = millis();

    while (true) {
        M5Dial.update();

        // Dial: zoom the pan view (finer as you turn one way).
        int dz;
        if (enc.poll(&dz)) {
            idx = constrain(idx + dz, 0, SETLOC_COUNT - 1);
            dirty = true;
            lastActivity = millis();
        }

        auto t = M5Dial.Touch.getDetail();
        if (t.wasPressed()) {
            lastX = downX = t.x;
            lastY = downY = t.y;
            onButton = hitSetBtn(t.x, t.y) || hitFavBtn(t.x, t.y);
            moved = false;
            lastActivity = millis();
        } else if (t.isPressed()) {
            int dx = t.x - lastX, dy = t.y - lastY;
            if (!onButton && (dx != 0 || dy != 0)) {
                // Drag the map: the point under the finger stays put, so the
                // centre moves opposite. Convert screen pixels -> degrees.
                float kmPerPx = SETLOC_RADII[idx] / 105.0f;   // PLOT_R
                float cl = cosf(lat * (float)M_PI / 180.0f);
                if (cl < 0.05f) cl = 0.05f;
                lat += (dy * kmPerPx) / 111.0f;
                lon -= (dx * kmPerPx) / (111.0f * cl);
                if (lat >  85.0f) lat =  85.0f;
                if (lat < -85.0f) lat = -85.0f;
                if (lon > 180.0f) lon -= 360.0f;
                if (lon < -180.0f) lon += 360.0f;
                moved = true;
                dirty = true;
            }
            lastX = t.x;
            lastY = t.y;
            lastActivity = millis();
        } else if (t.wasReleased()) {
            if (onButton && !moved) {
                if (hitSetBtn(downX, downY)) {
                    setHomeLocation(lat, lon);
                    if (mapMode() == MAP_FULL) mapLayer.precacheAll(lat, lon);
                    return;
                }
                if (hitFavBtn(downX, downY)) {
                    runFavSlotChooser(lat, lon);
                    dirty = true;
                }
            }
            lastActivity = millis();
        }

        if (M5Dial.BtnA.wasReleased()) return;             // press = cancel
        if (millis() - lastActivity > 45000UL) return;     // idle timeout

        if (dirty) {
            lofi::nearestCity(lat, lon, city, sizeof(city));
            radar.drawLoFiPan(lat, lon, SETLOC_RADII[idx]);
            drawSetLocationOverlay(lat, lon, SETLOC_RADII[idx], city);
            dirty = false;
        }
        delay(8);
    }
}

// ── Screenshot previews ──────────────────────────────────────────────────────
// Render a single frame of a modal screen into the sprite (no interactive loop),
// so the screenshot hooks can capture it. Both composite into the sprite and push.

void renderSettingsPreview(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                            float homeLat, float homeLon, float radiusKm, int zoomIdx,
                            unsigned long lastUpdateMs, bool fetching) {
    loadSettings();
    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, zoomIdx, lastUpdateMs, fetching);
    drawMenuPanel(0);
}

void renderSetLocationPreview(RadarDisplay& radar) {
    float lat = homeLat(), lon = homeLon(), km = 300.0f;
    char  city[24] = "";
    lofi::nearestCity(lat, lon, city, sizeof(city));
    radar.drawLoFiPan(lat, lon, km);
    drawSetLocationOverlay(lat, lon, km, city);
}

void runSettings(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                  float homeLat, float homeLon, float radiusKm, int zoomIdx,
                  unsigned long lastUpdateMs, bool fetching) {
    loadSettings();
    int menuIdx = 0;
    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, zoomIdx, lastUpdateMs, fetching);
    drawMenuPanel(menuIdx);

    // Swallow any residual button state from the press that opened us
    delay(200);
    while (M5Dial.BtnA.isPressed()) {
        M5Dial.update();
        delay(10);
    }
    delay(50);

    EncoderDebouncer enc;
    enc.begin();
    unsigned long lastActivity = millis();
    unsigned long lastTickMs   = millis();

    while (true) {
        M5Dial.update();

        int delta;
        if (enc.poll(&delta)) {
            menuIdx += delta;
            if (menuIdx < 0) menuIdx = 0;
            if (menuIdx > MENU_COUNT - 1) menuIdx = MENU_COUNT - 1;
            lastActivity = millis();
            // Panel is fully opaque and repaints its whole area every call,
            // so it doesn't need the backdrop (a full map-sprite re-push)
            // redrawn first — that was the "flashes on every step" bug.
            drawMenuPanel(menuIdx);
        }

        if (M5Dial.BtnA.wasReleased()) {
            const MenuItem& item = MENU_ITEMS[menuIdx];
            lastActivity = millis();

            if (item.kind == ItemKind::Cycle) {
                *item.value = (*item.value + 1) % item.optionCount;
                saveSettings();
                drawMenuPanel(menuIdx);   // stay in the menu, show the new value
                // Swallow the release so it isn't immediately re-read as a press
                delay(150);
                continue;
            }

            switch (item.action) {
                case MenuAction::SetLocation:    runSetLocation(radar); break;
                case MenuAction::DetectLocation: runDetectLocation();   break;
                case MenuAction::LocationPortal: runWebConfigInfo(radar, aircraft, homeLat, homeLon,
                                                                 radiusKm, zoomIdx, lastUpdateMs, fetching); break;
                case MenuAction::SavedLocations:
                    runLocationPicker(radar, aircraft, homeLat, homeLon, radiusKm,
                                      zoomIdx, lastUpdateMs, fetching);
                    break;
                case MenuAction::PowerOff:       runPowerOff();         break;
                case MenuAction::FactoryReset:
                    runFactoryResetConfirm(radar, aircraft, homeLat, homeLon, radiusKm,
                                           zoomIdx, lastUpdateMs, fetching);
                    break;
                default: break;
            }
            return;  // back to radar after handling the selected item
        }

        if (tappedToExit()) return;

        // Periodic tick so the poll icon's countdown keeps ticking while idle
        // in the menu. Just the icon, not a full drawBackdrop()+drawMenuPanel()
        // — re-pushing the whole map sprite every second was exactly the
        // "flickers every second" bug already fixed for the main radar screen,
        // and it applied here too since the icon sits below the panel anyway.
        unsigned long now = millis();
        if (now - lastTickMs >= 1000UL) {
            lastTickMs = now;
            radar.updatePollIcon(lastUpdateMs, fetching);
        }

        // Auto-exit after 30 s of no interaction
        if (millis() - lastActivity > 30000UL) return;

        delay(20);
    }
}
