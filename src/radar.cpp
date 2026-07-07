#include "radar.h"
#include "config.h"
#include <math.h>

// ── Colour palette (RGB565) ──────────────────────────────────────────────────
static constexpr uint16_t COL_BG       = 0x0008;  // near-black
static constexpr uint16_t COL_RING     = 0x0841;  // dim green rings
static constexpr uint16_t COL_RING_LBL = 0x07E0;  // bright green labels
static constexpr uint16_t COL_HOME     = 0xF800;  // red home crosshair
static constexpr uint16_t COL_AC       = 0xFFFF;  // white aircraft
static constexpr uint16_t COL_SEL      = 0xFD20;  // orange selected
static constexpr uint16_t COL_GND      = 0x8410;  // grey on-ground
static constexpr uint16_t COL_STATUS   = 0x7BEF;  // light grey status text
static constexpr uint16_t COL_OVERLAY  = 0x0841;  // detail box background

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
    M5Dial.Display.fillScreen(COL_BG);
    drawRings(radiusKm);

    // Home crosshair
    M5Dial.Display.drawLine(CX - 6, CY, CX + 6, CY, COL_HOME);
    M5Dial.Display.drawLine(CX, CY - 6, CX, CY + 6, COL_HOME);
    M5Dial.Display.drawCircle(CX, CY, 3, COL_HOME);

    int visible = 0;
    for (int i = 0; i < (int)aircraft.size(); ++i) {
        const Aircraft& ac = aircraft[i];
        int sx, sy;
        worldToScreen(ac.lat, ac.lon, cLat, cLon, radiusKm, sx, sy);

        // Skip if outside the radar circle
        int dx = sx - CX, dy = sy - CY;
        if ((dx * dx + dy * dy) > (PLOT_R * PLOT_R)) continue;

        drawAircraft(ac, sx, sy, i == selectedIdx);
        ++visible;
    }

    drawPollIcon(lastUpdateMs, fetching);

    if (selectedIdx >= 0 && selectedIdx < (int)aircraft.size()) {
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

void RadarDisplay::drawError(const char* msg) {
    M5Dial.Display.fillScreen(COL_BG);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(COL_HOME, COL_BG);   // red text
    M5Dial.Display.drawString(msg, CX, CY);
}

// ── Private ──────────────────────────────────────────────────────────────────

void RadarDisplay::drawRings(float radiusKm) {
    // Three concentric rings (100%, 50%, 25% of radius)
    M5Dial.Display.drawCircle(CX, CY, PLOT_R,     COL_RING);
    M5Dial.Display.drawCircle(CX, CY, PLOT_R / 2, COL_RING);
    M5Dial.Display.drawCircle(CX, CY, PLOT_R / 4, COL_RING);

    // North tick + label
    M5Dial.Display.drawLine(CX, CY - PLOT_R, CX, CY - PLOT_R + 8, COL_RING_LBL);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(COL_RING_LBL, COL_BG);
    M5Dial.Display.drawString("N", CX, CY - PLOT_R - 8);

    // Range labels on the 50% and 100% rings (right side)
    char buf[16];
    M5Dial.Display.setTextColor(COL_RING, COL_BG);
    snprintf(buf, sizeof(buf), "%.0fkm", radiusKm * 0.5f);
    M5Dial.Display.drawString(buf, CX + PLOT_R / 2 + 14, CY);
    snprintf(buf, sizeof(buf), "%.0fkm", radiusKm);
    M5Dial.Display.drawString(buf, CX + PLOT_R + 14, CY);
}

void RadarDisplay::drawAircraft(const Aircraft& ac, int sx, int sy, bool selected) {
    uint16_t col = selected ? COL_SEL : (ac.onGround ? COL_GND : COL_AC);

    // Heading arrow (skip if on ground or speed unknown)
    if (!ac.onGround && ac.speedMs > 5.0f) {
        float rad = ac.heading * (float)M_PI / 180.0f;
        int ex = sx + (int)(sinf(rad) * 10.0f);
        int ey = sy - (int)(cosf(rad) * 10.0f);
        M5Dial.Display.drawLine(sx, sy, ex, ey, col);
    }

    // Body dot
    M5Dial.Display.fillCircle(sx, sy, selected ? 4 : 3, col);

    // Callsign above the dot (only when selected — avoids clutter)
    if (selected) {
        M5Dial.Display.setTextSize(1);
        M5Dial.Display.setTextColor(COL_SEL, COL_BG);
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

    // Row 2: altitude ft
    if (ac.altM > 0.0f) {
        snprintf(buf, sizeof(buf), "Alt %d ft", (int)(ac.altM * 3.28084f));
    } else {
        snprintf(buf, sizeof(buf), "Alt  n/a");
    }
    M5Dial.Display.drawString(buf, CX, lineY);   lineY += step;

    // Row 3: speed (kts) + heading
    int kts = (int)(ac.speedMs * 1.94384f);
    snprintf(buf, sizeof(buf), "%d kts  %03.0f\xb0", kts, ac.heading);
    M5Dial.Display.drawString(buf, CX, lineY);   lineY += step;

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

    // Otherwise, bright arc shrinks from full circle down to nothing as the
    // next poll approaches
    unsigned long elapsed = millis() - lastUpdateMs;
    float fraction = (float)elapsed / (float)REFRESH_INTERVAL_MS;
    if (fraction > 1.0f) fraction = 1.0f;
    float remaining = 1.0f - fraction;

    if (remaining > 0.01f) {
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0,
                  0, remaining * 360.0f, COL_RING_LBL);
    }
}
