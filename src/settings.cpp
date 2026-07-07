#include "settings.h"
#include "provisioning.h"
#include "config.h"
#include "radar.h"
#include "encoder_debounce.h"
#include <M5Dial.h>
#include <Preferences.h>

// ── Persisted state ───────────────────────────────────────────────────────────
// Mirrors the emulator's `self.settings` dict — each field is a small cycle
// index into the option-label arrays declared alongside MENU_ITEMS below.
struct SettingsState {
    uint8_t labels        = 1;  // Off/Selected/All        — default: Selected
    uint8_t theme         = 0;  // Radar/Amber/Ocean/Neon
    uint8_t units         = 0;  // ft·kts / m·km/h
    uint8_t filter        = 0;  // Airborne/All
    uint8_t minalt        = 0;  // Off/1k/5k/10k/20k ft
    uint8_t trails        = 0;  // On/Off                  — 0 = On
    uint8_t rings         = 0;  // On/Off                  — 0 = On
    uint8_t refresh       = REFRESH_DEFAULT;
    uint8_t buzzEmergency = 0;  // On/Off                  — 0 = On (matches prior default true)
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
unsigned long refreshIntervalMs() { return REFRESH_OPTIONS_MS[_s.refresh]; }

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
    _s.refresh       = prefs.getUChar("s_refresh", REFRESH_DEFAULT);
    _s.buzzEmergency = prefs.getUChar("s_buzz",    0);
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
    prefs.putUChar("s_refresh",_s.refresh);
    prefs.putUChar("s_buzz",   _s.buzzEmergency);
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

// Floating panel geometry — a "little box" over the live radar/map, matching
// the emulator's centred settings panel instead of a full-screen wipe.
static constexpr int PANEL_X = 20, PANEL_Y = 62, PANEL_W = 200, PANEL_H = 116;
static constexpr int PANEL_CX = PANEL_X + PANEL_W / 2;

// Redraws the live radar (map, rings, aircraft, poll icon) with no selection,
// so settings panels sit on top of current, real content instead of a blank
// screen — callers draw their panel immediately after this each frame.
static void drawBackdrop(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                          float homeLat, float homeLon, float radiusKm,
                          unsigned long lastUpdateMs, bool fetching) {
    radar.draw(aircraft, homeLat, homeLon, radiusKm, /*selectedIdx=*/-1,
               lastUpdateMs, fetching);
}

static void panelFrame(const char* title, int page = 0, int numPages = 0) {
    auto& d = M5Dial.Display;
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
static const char* OPTS_REFRESH[] = { "10s", "20s", "30s" };

enum class ItemKind : uint8_t { Cycle, Action, Danger };

struct MenuItem {
    const char*       label;
    ItemKind          kind;
    const char* const* options;   // Cycle only
    int               optionCount;
    uint8_t*          value;      // Cycle only — points into _s
};

static void runLocationPicker(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                               float homeLat, float homeLon, float radiusKm,
                               unsigned long lastUpdateMs, bool fetching);
static void runFactoryResetConfirm(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                                    float homeLat, float homeLon, float radiusKm,
                                    unsigned long lastUpdateMs, bool fetching);

static const MenuItem MENU_ITEMS[] = {
    { "Flight labels",       ItemKind::Cycle,  OPTS_LABELS,  3, &_s.labels },
    { "Colour theme",        ItemKind::Cycle,  OPTS_THEME,   4, &_s.theme },
    { "Units",               ItemKind::Cycle,  OPTS_UNITS,   2, &_s.units },
    { "Traffic",             ItemKind::Cycle,  OPTS_FILTER,  2, &_s.filter },
    { "Min altitude",        ItemKind::Cycle,  OPTS_MINALT,  5, &_s.minalt },
    { "Heading trails",      ItemKind::Cycle,  OPTS_ONOFF,   2, &_s.trails },
    { "Range rings",         ItemKind::Cycle,  OPTS_ONOFF,   2, &_s.rings },
    { "Refresh rate",        ItemKind::Cycle,  OPTS_REFRESH, 3, &_s.refresh },
    { "Buzz on Emergency",   ItemKind::Cycle,  OPTS_ONOFF,   2, &_s.buzzEmergency },
    { "Location & API Keys", ItemKind::Action, nullptr,      0, nullptr },
    { "Saved Locations",     ItemKind::Action, nullptr,      0, nullptr },
    { "Factory Reset",       ItemKind::Danger, nullptr,      0, nullptr },
};
static const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// Rows per page — 3 fits the panel height alongside its title and hint line.
static const int MENU_PAGE_SIZE = 3;

static void drawMenuPanel(int idx) {
    int numPages = (MENU_COUNT + MENU_PAGE_SIZE - 1) / MENU_PAGE_SIZE;
    int page     = idx / MENU_PAGE_SIZE;
    int start    = page * MENU_PAGE_SIZE;
    int end      = start + MENU_PAGE_SIZE;
    if (end > MENU_COUNT) end = MENU_COUNT;

    panelFrame("SETTINGS", page, numPages);
    auto& d = M5Dial.Display;

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
}

static void drawLocationPickerPanel(int idx) {
    panelFrame("SAVED LOCATIONS");
    auto& d = M5Dial.Display;
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
}

// Rotate to pick a saved favourite, press to make it the active home location.
// Empty slots are shown but do nothing when selected. Tap or timeout cancels.
static void runLocationPicker(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                               float homeLat, float homeLon, float radiusKm,
                               unsigned long lastUpdateMs, bool fetching) {
    int idx = 0;
    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, lastUpdateMs, fetching);
    drawLocationPickerPanel(idx);

    EncoderDebouncer enc;
    enc.begin(ENC_STABLE_MS_MENU);
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
    auto& d = M5Dial.Display;
    d.setTextDatum(MC_DATUM);

    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("Erases WiFi, OpenSky login,", PANEL_CX, PANEL_Y + 38);
    d.drawString("location & favourites", PANEL_CX, PANEL_Y + 52);

    // Progress ring fills as the button is held
    int cy = PANEL_Y + 78;
    float frac = (float)heldMs / 3000.0f;
    if (frac > 1.0f) frac = 1.0f;
    d.drawArc(PANEL_CX, cy, 16, 12, 0, 360, S_GREY);
    if (frac > 0.01f) d.drawArc(PANEL_CX, cy, 16, 12, 0, frac * 360.0f, S_ORANGE);

    d.setTextColor(S_GREY, S_OVERLAY);
    d.drawString("hold=confirm  tap/release=cancel", PANEL_CX, PANEL_Y + PANEL_H - 8);
}

// Hold the button 3 s to confirm, matching the same gesture as the physical
// reset combo; release early, tap, or leave it untouched (timeout) to cancel.
static void runFactoryResetConfirm(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                                    float homeLat, float homeLon, float radiusKm,
                                    unsigned long lastUpdateMs, bool fetching) {
    unsigned long pressStart   = 0;
    bool          held         = false;
    unsigned long lastActivity = millis();

    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, lastUpdateMs, fetching);
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

void runSettings(RadarDisplay& radar, const std::vector<Aircraft>& aircraft,
                  float homeLat, float homeLon, float radiusKm,
                  unsigned long lastUpdateMs, bool fetching) {
    loadSettings();
    int menuIdx = 0;
    drawBackdrop(radar, aircraft, homeLat, homeLon, radiusKm, lastUpdateMs, fetching);
    drawMenuPanel(menuIdx);

    // Swallow any residual button state from the press that opened us
    delay(200);
    while (M5Dial.BtnA.isPressed()) {
        M5Dial.update();
        delay(10);
    }
    delay(50);

    EncoderDebouncer enc;
    enc.begin(ENC_STABLE_MS_MENU);
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

            if (menuIdx == MENU_COUNT - 3) {        // Location & API Keys
                runLocationPortal();
            } else if (menuIdx == MENU_COUNT - 2) { // Saved Locations
                runLocationPicker(radar, aircraft, homeLat, homeLon, radiusKm, lastUpdateMs, fetching);
            } else {                                 // Factory Reset
                runFactoryResetConfirm(radar, aircraft, homeLat, homeLon, radiusKm, lastUpdateMs, fetching);
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
