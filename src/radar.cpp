#include "radar.h"
#include "config.h"
#include "map.h"
#include "settings.h"
#include <math.h>

// ── Colour themes (RGB565) — mirrors the emulator's THEMES list ─────────────
struct Theme {
    uint16_t bg, ring, ringLbl, home, ac, sel, gnd, status, overlay;
};
static const Theme THEMES[] = {
    // Radar
    { 0x0001, 0x0941, 0x06E0, 0xF800, 0xFFFF, 0xFD20, 0x8410, 0xA514, 0x0861 },
    // Amber
    { 0x0820, 0x6180, 0xFDC0, 0xFA80, 0xFEEF, 0xFB20, 0xA325, 0xD50A, 0x1040 },
    // Ocean
    { 0x0022, 0x018D, 0x05FF, 0xFA8A, 0xB73F, 0x07F9, 0x53D4, 0x7D5A, 0x0063 },
    // Neon
    { 0x0001, 0x400B, 0xD81F, 0xF9E7, 0x07F6, 0xFF20, 0x9295, 0xBB3B, 0x1002 },
};
static const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

static const Theme& theme() {
    int i = activeTheme();
    if (i < 0 || i >= THEME_COUNT) i = 0;
    return THEMES[i];
}

// Colour macros so the rest of this file reads the same as before the theme
// system existed — each expands to a lookup against the active theme.
#define COL_BG        (theme().bg)
#define COL_RING      (theme().ring)
#define COL_RING_LBL  (theme().ringLbl)
#define COL_HOME      (theme().home)
#define COL_AC        (theme().ac)
#define COL_SEL       (theme().sel)
#define COL_GND       (theme().gnd)
#define COL_STATUS    (theme().status)
#define COL_OVERLAY   (theme().overlay)

// Fixed (not theme-dependent) dark outline drawn behind every aircraft mark.
// The map underlay is a real, light-coloured OSM tile now — a plain small
// dot/thin line with no outline easily disappears against it. The emulator
// already does this (its HALO constant); it was never ported to the device.
static constexpr uint16_t COL_HALO = 0x0000;

static constexpr float EARTH_R = 6371.0f;  // km

// ── Public ───────────────────────────────────────────────────────────────────

void RadarDisplay::begin() {
    M5Dial.Display.setTextDatum(MC_DATUM);
}

void RadarDisplay::worldToScreen(float lat, float lon,
                                  float cLat, float cLon, float radiusKm,
                                  int& sx, int& sy) {
    float cLatRad = cLat * (float)M_PI / 180.0f;
    float dx = (lon - cLon) * cosf(cLatRad) * EARTH_R * ((float)M_PI / 180.0f);
    float dy = (lat - cLat) * EARTH_R * ((float)M_PI / 180.0f);
    sx = CX + (int)((dx / radiusKm) * PLOT_R);
    sy = CY - (int)((dy / radiusKm) * PLOT_R);   // screen Y is inverted
}

int RadarDisplay::hitTest(int tx, int ty,
                           const std::vector<Aircraft>& aircraft,
                           float cLat, float cLon, float radiusKm) {
    // Scan in reverse so topmost-drawn aircraft wins on overlap
    for (int i = (int)aircraft.size() - 1; i >= 0; --i) {
        int sx, sy;
        worldToScreen(aircraft[i].lat, aircraft[i].lon, cLat, cLon, radiusKm, sx, sy);
        int dx = tx - sx, dy = ty - sy;
        if ((dx * dx + dy * dy) <= 144) return i;  // 12 px hit radius
    }
    return -1;
}

void RadarDisplay::draw(const std::vector<Aircraft>& aircraft,
                         float cLat, float cLon, float radiusKm,
                         int selectedIdx, unsigned long lastUpdateMs, bool fetching) {
    // Map underlay if available, otherwise a solid background.
    mapLayer.ensure(cLat, cLon, radiusKm);
    if (!mapLayer.blitTo())
        M5Dial.Display.fillScreen(COL_BG);
    drawRings(radiusKm);

    // Home crosshair
    M5Dial.Display.drawLine(CX - 6, CY, CX + 6, CY, COL_HOME);
    M5Dial.Display.drawLine(CX, CY - 6, CX, CY + 6, COL_HOME);
    M5Dial.Display.drawCircle(CX, CY, 3, COL_HOME);

    bool  airborneOnly = trafficFilter() == 0;
    float minAlt       = minAltitudeM();

    for (int i = 0; i < (int)aircraft.size(); ++i) {
        const Aircraft& ac = aircraft[i];

        if (airborneOnly && ac.onGround) continue;
        if (ac.altM > 0.0f && ac.altM < minAlt) continue;

        int sx, sy;
        worldToScreen(ac.lat, ac.lon, cLat, cLon, radiusKm, sx, sy);

        // Skip if outside the radar circle
        int dx = sx - CX, dy = sy - CY;
        if ((dx * dx + dy * dy) > (PLOT_R * PLOT_R)) continue;

        drawAircraft(ac, sx, sy, i == selectedIdx);
    }

    drawPollIcon(lastUpdateMs, fetching);

    if (_showStatus) {
        drawApiStatusOverlay();          // status panel takes precedence
    } else if (selectedIdx >= 0 && selectedIdx < (int)aircraft.size()) {
        drawDetail(aircraft[selectedIdx]);
    }
}

void RadarDisplay::drawBoot() {
    M5Dial.Display.fillScreen(COL_BG);
    M5Dial.Display.setTextColor(COL_RING_LBL, COL_BG);
    M5Dial.Display.setTextSize(2);
    M5Dial.Display.drawString("FlightDial", CX, CY - 16);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(COL_STATUS, COL_BG);
    M5Dial.Display.drawString("Live Aircraft Radar", CX, CY + 12);
}

// ── Private ──────────────────────────────────────────────────────────────────

void RadarDisplay::drawRings(float radiusKm) {
    if (!showRings()) return;

    // Three concentric rings (100%, 50%, 25% of radius)
    M5Dial.Display.drawCircle(CX, CY, PLOT_R,     COL_RING);
    M5Dial.Display.drawCircle(CX, CY, PLOT_R / 2, COL_RING);
    M5Dial.Display.drawCircle(CX, CY, PLOT_R / 4, COL_RING);

    // North tick + label
    M5Dial.Display.drawLine(CX, CY - PLOT_R, CX, CY - PLOT_R + 8, COL_RING_LBL);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(COL_RING_LBL, COL_BG);
    M5Dial.Display.drawString("N", CX, CY - PLOT_R - 8);

    // Range labels on the 50% and 100% rings (right side). Uses the bright
    // ring-label colour (same as "N"), not the dim ring colour itself — the
    // dim colour is nearly identical to the background for every theme, so
    // the text body was essentially invisible and only faint anti-aliased
    // glyph edges showed, which is what read as "little black triangles".
    char buf[16];
    M5Dial.Display.setTextColor(COL_RING_LBL, COL_BG);
    snprintf(buf, sizeof(buf), "%.0fkm", radiusKm * 0.5f);
    M5Dial.Display.drawString(buf, CX + PLOT_R / 2 + 14, CY);
    snprintf(buf, sizeof(buf), "%.0fkm", radiusKm);
    M5Dial.Display.drawString(buf, CX + PLOT_R + 14, CY);
}

void RadarDisplay::drawAircraft(const Aircraft& ac, int sx, int sy, bool selected) {
    bool emergency = ac.isEmergency();
    uint16_t col = emergency  ? COL_HOME   // 7500/7600/7700 — always red, unmissable
                 : selected   ? COL_SEL
                 : ac.onGround ? COL_GND : COL_AC;

    int radius = selected ? 5 : 4;   // bumped up a notch — small dots were hard to spot
    if (emergency) radius += 1;

    // Heading arrow — starts just outside the body dot's edge instead of at
    // its exact centre (the dot is filled *after* this, on top, so a line
    // starting at the centre had its first few px hidden underneath it) and
    // is long enough to actually read as a direction indicator. A dark halo
    // drawn first (wider) gives it contrast against the map underlay,
    // mirroring the emulator's HALO treatment — a bare 1px line in aircraft
    // colour all but disappears over light-coloured map tiles.
    if (showTrails() && !ac.onGround && ac.speedMs > 5.0f) {
        float rad  = ac.heading * (float)M_PI / 180.0f;
        float sinR = sinf(rad), cosR = cosf(rad);
        const int   gap = radius + 1;
        const float len = 13.0f;
        float sx0 = sx + sinR * gap,       sy0 = sy - cosR * gap;
        float ex  = sx + sinR * (gap + len), ey = sy - cosR * (gap + len);
        M5Dial.Display.drawWideLine(sx0, sy0, ex, ey, 2.6f, COL_HALO);
        M5Dial.Display.drawWideLine(sx0, sy0, ex, ey, 1.2f, col);
    }

    // Body dot — dark halo ring behind it for the same map-contrast reason,
    // then an extra red ring further out still for emergencies so they read
    // as "highlighted" at a glance rather than just "a red dot instead of
    // white".
    M5Dial.Display.fillCircle(sx, sy, radius + 2, COL_HALO);
    if (emergency) {
        M5Dial.Display.drawCircle(sx, sy, radius + 4, COL_HOME);
    }
    M5Dial.Display.fillCircle(sx, sy, radius, col);

    // Callsign label: Off (never) / Selected (only this one) / All
    int labels = flightLabels();
    bool showLabel = (labels == 2) || (labels == 1 && selected) || emergency;
    if (showLabel) {
        M5Dial.Display.setTextSize(1);
        M5Dial.Display.setTextColor(emergency ? COL_HOME : (selected ? COL_SEL : COL_STATUS), COL_BG);
        M5Dial.Display.drawString(ac.callsign, sx, sy - 16);
    }
}

void RadarDisplay::drawDetail(const Aircraft& ac) {
    // Pill-shaped overlay in the lower portion of the round screen
    M5Dial.Display.fillRoundRect(18, 148, 204, 76, 6, COL_OVERLAY);
    M5Dial.Display.drawRoundRect(18, 148, 204, 76, 6, COL_SEL);

    char buf[48];
    int lineY = 160;
    const int step = 16;

    // Row 1: callsign + ICAO
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(COL_SEL, COL_OVERLAY);
    snprintf(buf, sizeof(buf), "%s  [%s]",
             ac.callsign[0] ? ac.callsign : "N/A", ac.icao24);
    M5Dial.Display.drawString(buf, CX, lineY);   lineY += step;

    M5Dial.Display.setTextColor(COL_STATUS, COL_OVERLAY);

    bool metric = activeUnits() == 1;

    // Row 2: altitude (ft or m)
    if (ac.altM > 0.0f) {
        if (metric) snprintf(buf, sizeof(buf), "Alt %d m", (int)ac.altM);
        else        snprintf(buf, sizeof(buf), "Alt %d ft", (int)(ac.altM * 3.28084f));
    } else {
        snprintf(buf, sizeof(buf), "Alt  n/a");
    }
    M5Dial.Display.drawString(buf, CX, lineY);   lineY += step;

    // Row 3: speed (kts or km/h) + heading. The default font has no glyph at
    // all for the degree sign — a correct UTF-8 encoding still fell back to
    // a placeholder "tofu" box, because the font itself simply lacks that
    // character, not because of an encoding mismatch. Drawing a small hollow
    // circle by hand right after the number sidesteps needing the font to
    // have it at all.
    int speed = metric ? (int)(ac.speedMs * 3.6f) : (int)(ac.speedMs * 1.94384f);
    snprintf(buf, sizeof(buf), "%d %s  %03.0f", speed, metric ? "km/h" : "kts", ac.heading);
    M5Dial.Display.drawString(buf, CX, lineY);
    int textW = M5Dial.Display.textWidth(buf);
    M5Dial.Display.drawCircle(CX + textW / 2 + 5, lineY - 4, 2, COL_STATUS);
    lineY += step;

    // Row 4: country / on-ground flag
    snprintf(buf, sizeof(buf), "%s%s", ac.country, ac.onGround ? "  [GND]" : "");
    M5Dial.Display.drawString(buf, CX, lineY);
}

void RadarDisplay::flashEmergencyRing() {
    // 3 px thick ring just inside the round screen bezel (radius 115–117)
    auto& d = M5Dial.Display;
    for (int flash = 0; flash < 3; flash++) {
        for (int r = 115; r <= 117; r++)
            d.drawCircle(CX, CY, r, COL_HOME);   // COL_HOME = red
        M5Dial.Speaker.tone(2200, 150);
        delay(300);
        for (int r = 115; r <= 117; r++)
            d.drawCircle(CX, CY, r, COL_BG);
        if (flash < 2) delay(180);
    }
}

void RadarDisplay::drawPollIcon(unsigned long lastUpdateMs, bool fetching) {
    auto& d = M5Dial.Display;

    // Dim track — full circle
    d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, 0, 360, COL_RING);

    if (fetching) {
        // Solid bright ring while a request is in flight
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, 0, 360, COL_SEL);
        return;
    }

    // Last fetch failed (HTTP/JSON/network error or states:null) — solid red
    // ring so a stalled feed is obvious at a glance. Tap it for the reason.
    if (apiFailed(apiStatus().state)) {
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, 0, 360, COL_HOME);
        return;
    }

    // Otherwise, bright arc shrinks from full circle down to nothing as the
    // next poll approaches
    unsigned long elapsed = millis() - lastUpdateMs;
    float fraction = (float)elapsed / (float)refreshIntervalMs();
    if (fraction > 1.0f) fraction = 1.0f;
    float remaining = 1.0f - fraction;

    if (remaining > 0.01f) {
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0,
                  0, remaining * 360.0f, COL_RING_LBL);
    }
}

bool RadarDisplay::hitPollIcon(int tx, int ty) const {
    // Generous target — the icon itself is tiny and sits near the bottom bezel.
    int dx = tx - ICON_X, dy = ty - ICON_Y;
    return (dx * dx + dy * dy) <= (22 * 22);
}

void RadarDisplay::drawApiStatusOverlay() {
    auto& d = M5Dial.Display;
    const ApiStatus& s = apiStatus();

    d.fillRoundRect(28, 74, 184, 104, 8, COL_OVERLAY);
    d.drawRoundRect(28, 74, 184, 104, 8, COL_SEL);

    const char* label;
    uint16_t    col;
    switch (s.state) {
        case ApiState::Ok:         label = "ONLINE";     col = COL_RING_LBL; break;
        case ApiState::NoData:     label = "NO DATA";    col = COL_SEL;      break;
        case ApiState::HttpError:  label = "HTTP ERROR"; col = COL_HOME;     break;
        case ApiState::ParseError: label = "BAD DATA";   col = COL_HOME;     break;
        case ApiState::NetError:   label = "OFFLINE";    col = COL_HOME;     break;
        default:                   label = "WAITING";    col = COL_STATUS;   break;
    }

    d.setTextDatum(MC_DATUM);
    d.setTextSize(1);
    d.setTextColor(COL_RING_LBL, COL_OVERLAY);
    d.drawString("API STATUS", CX, 90);

    d.setTextSize(2);
    d.setTextColor(col, COL_OVERLAY);
    d.drawString(label, CX, 112);

    char buf[48];
    d.setTextSize(1);
    d.setTextColor(COL_STATUS, COL_OVERLAY);

    // Reason / aircraft count from the last attempt
    d.drawString(s.detail[0] ? s.detail : "-", CX, 134);

    // HTTP code + payload size
    if (s.httpCode)
        snprintf(buf, sizeof(buf), "HTTP %d   %dB", s.httpCode, s.bytes);
    else
        snprintf(buf, sizeof(buf), "no request yet");
    d.drawString(buf, CX, 150);

    // Age of last attempt
    if (s.lastMs) {
        unsigned long age = (millis() - s.lastMs) / 1000UL;
        snprintf(buf, sizeof(buf), "%lus ago", age);
        d.drawString(buf, CX, 166);
    }
}
