#include "settings.h"
#include "provisioning.h"
#include <M5Dial.h>
#include <Preferences.h>

static bool _buzzEnabled = true;

bool buzzOnEmergency() { return _buzzEnabled; }

void loadSettings() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/true);
    _buzzEnabled = prefs.getBool("buzz_emerg", true);
    prefs.end();
}

static void saveSettings() {
    Preferences prefs;
    prefs.begin("flightdial", /*readOnly=*/false);
    prefs.putBool("buzz_emerg", _buzzEnabled);
    prefs.end();
}

// ── Colour palette (RGB565) — matches radar palette ──────────────────────────
static constexpr uint16_t S_BG     = 0x0008;
static constexpr uint16_t S_GREEN  = 0x07E0;
static constexpr uint16_t S_ORANGE = 0xFD20;
static constexpr uint16_t S_GREY   = 0x7BEF;

static void drawSettingsScreen(bool enabled) {
    auto& d = M5Dial.Display;
    d.fillScreen(S_BG);
    d.setTextDatum(MC_DATUM);

    d.setTextColor(S_GREEN, S_BG);
    d.setTextSize(1);
    d.drawString("SETTINGS", 120, 70);

    d.setTextColor(S_GREY, S_BG);
    d.drawString("Buzz on Emergency", 120, 102);

    d.setTextColor(enabled ? S_GREEN : S_ORANGE, S_BG);
    d.setTextSize(2);
    d.drawString(enabled ? "ON" : "OFF", 120, 128);

    d.setTextSize(1);
    d.setTextColor(S_GREY, S_BG);
    d.drawString("rotate to toggle", 120, 165);
    d.drawString("press to save", 120, 182);
}

static const char* MENU_ITEMS[] = { "Buzz on Emergency", "Change Location", "Saved Locations", "Factory Reset" };
static const int   MENU_COUNT   = 4;

// Rows per page. Kept small enough that row text (drawn centred at x=120,
// y=100+slot*24) never gets close enough to the top/bottom of the round
// screen for its curve to clip the text — and leaves room for the
// "rotate/press" hints fixed at y=165/182 below the last row.
static const int MENU_PAGE_SIZE = 3;

static void drawMenuScreen(int idx) {
    auto& d = M5Dial.Display;
    d.fillScreen(S_BG);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);

    int numPages = (MENU_COUNT + MENU_PAGE_SIZE - 1) / MENU_PAGE_SIZE;
    int page     = idx / MENU_PAGE_SIZE;
    int start    = page * MENU_PAGE_SIZE;
    int end      = start + MENU_PAGE_SIZE;
    if (end > MENU_COUNT) end = MENU_COUNT;

    d.setTextColor(S_GREEN, S_BG);
    if (numPages > 1) {
        char buf[24];
        snprintf(buf, sizeof(buf), "SETTINGS %d/%d", page + 1, numPages);
        d.drawString(buf, 120, 55);
    } else {
        d.drawString("SETTINGS", 120, 55);
    }

    for (int i = start; i < end; i++) {
        int slot = i - start;
        d.setTextColor(i == idx ? S_ORANGE : S_GREY, S_BG);
        d.drawString(MENU_ITEMS[i], 120, 100 + slot * 24);
    }

    d.setTextColor(S_GREY, S_BG);
    d.drawString("rotate to move", 120, 165);
    d.drawString("press to select", 120, 182);
}

// Rotate to toggle, press to save. Returns to caller once saved.
static void runBuzzToggle() {
    bool current = _buzzEnabled;
    drawSettingsScreen(current);

    long lastEnc = M5Dial.Encoder.read();
    unsigned long lastActivity = millis();

    while (true) {
        M5Dial.update();

        long enc = M5Dial.Encoder.read();
        if (enc != lastEnc) {
            current = (enc > lastEnc);  // CW = ON, CCW = OFF
            lastEnc = enc;
            lastActivity = millis();
            drawSettingsScreen(current);
        }

        if (M5Dial.BtnA.wasReleased()) {
            _buzzEnabled = current;
            saveSettings();
            break;
        }

        // Auto-exit after 30 s of no interaction
        if (millis() - lastActivity > 30000UL) break;

        delay(20);
    }
}

static void drawLocationPicker(int idx) {
    auto& d = M5Dial.Display;
    d.fillScreen(S_BG);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);

    d.setTextColor(S_GREEN, S_BG);
    d.drawString("SAVED LOCATIONS", 120, 55);

    for (int i = 0; i < FAV_COUNT; i++) {
        bool set = favName(i)[0] != '\0';
        d.setTextColor(i == idx ? S_ORANGE : S_GREY, S_BG);
        char buf[32];
        if (set) snprintf(buf, sizeof(buf), "%s", favName(i));
        else     snprintf(buf, sizeof(buf), "Favourite %d (empty)", i + 1);
        d.drawString(buf, 120, 95 + i * 24);
    }

    d.setTextColor(S_GREY, S_BG);
    d.drawString("rotate to move", 120, 175);
    d.drawString("press to activate", 120, 192);
}

// Rotate to pick a saved favourite, press to make it the active home location.
// Empty slots are shown but do nothing when selected.
static void runLocationPicker() {
    int idx = 0;
    drawLocationPicker(idx);

    long lastEnc = M5Dial.Encoder.read();
    unsigned long lastActivity = millis();

    while (true) {
        M5Dial.update();

        long enc = M5Dial.Encoder.read();
        if (enc != lastEnc) {
            idx += (enc > lastEnc) ? 1 : -1;
            if (idx < 0) idx = 0;
            if (idx > FAV_COUNT - 1) idx = FAV_COUNT - 1;
            lastEnc = enc;
            lastActivity = millis();
            drawLocationPicker(idx);
        }

        if (M5Dial.BtnA.wasReleased()) {
            if (favName(idx)[0] != '\0') {
                setHomeLocation(favLat(idx), favLon(idx));
            }
            break;
        }

        // Auto-exit after 30 s of no interaction
        if (millis() - lastActivity > 30000UL) break;

        delay(20);
    }
}

static void drawFactoryResetScreen(unsigned long heldMs) {
    auto& d = M5Dial.Display;
    d.fillScreen(S_BG);
    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);

    d.setTextColor(S_ORANGE, S_BG);
    d.drawString("FACTORY RESET", 120, 62);

    d.setTextColor(S_GREY, S_BG);
    d.drawString("Erases WiFi, OpenSky login,", 120, 90);
    d.drawString("location & favourites", 120, 106);

    // Progress ring fills as the button is held
    float frac = (float)heldMs / 3000.0f;
    if (frac > 1.0f) frac = 1.0f;
    d.drawArc(120, 140, 20, 15, 0, 360, S_GREY);
    if (frac > 0.01f) d.drawArc(120, 140, 20, 15, 0, frac * 360.0f, S_ORANGE);

    d.drawString("hold to confirm", 120, 172);
    d.drawString("release to cancel", 120, 188);
}

// Hold the button 3 s to confirm, matching the same gesture as the physical
// reset combo; release early (or leave it untouched) to cancel.
static void runFactoryResetConfirm() {
    unsigned long pressStart   = 0;
    bool          held         = false;
    unsigned long lastActivity = millis();

    drawFactoryResetScreen(0);

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
            drawFactoryResetScreen(heldMs);
            if (heldMs >= 3000UL) {
                factoryReset();  // does not return — reboots the device
            }
        } else if (!down && held) {
            break;  // released early — cancel
        }

        // Auto-cancel after 15 s of no interaction
        if (millis() - lastActivity > 15000UL) break;

        delay(20);
    }
}

void runSettings() {
    loadSettings();
    int menuIdx = 0;
    drawMenuScreen(menuIdx);

    // Swallow any residual button state from the press that opened us
    delay(200);
    while (M5Dial.BtnA.isPressed()) {
        M5Dial.update();
        delay(10);
    }
    delay(50);

    long lastEnc = M5Dial.Encoder.read();
    unsigned long lastActivity = millis();

    while (true) {
        M5Dial.update();

        long enc = M5Dial.Encoder.read();
        if (enc != lastEnc) {
            menuIdx += (enc > lastEnc) ? 1 : -1;
            if (menuIdx < 0) menuIdx = 0;
            if (menuIdx > MENU_COUNT - 1) menuIdx = MENU_COUNT - 1;
            lastEnc = enc;
            lastActivity = millis();
            drawMenuScreen(menuIdx);
        }

        if (M5Dial.BtnA.wasReleased()) {
            if (menuIdx == 0) {
                runBuzzToggle();
            } else if (menuIdx == 1) {
                runLocationPortal();
            } else if (menuIdx == 2) {
                runLocationPicker();
            } else {
                runFactoryResetConfirm();
            }
            return;  // back to radar after handling the selected item
        }

        // Auto-exit after 30 s of no interaction
        if (millis() - lastActivity > 30000UL) return;

        delay(20);
    }
}
