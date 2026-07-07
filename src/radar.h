#pragma once
#include <M5Dial.h>
#include "aircraft.h"
#include <vector>

class RadarDisplay {
public:
    void begin();

    // Full radar redraw. `fetching` flips the poll icon into its "request in
    // flight" look instead of its usual countdown-to-next-poll ring.
    void draw(const std::vector<Aircraft>& aircraft,
              float centerLat, float centerLon, float radiusKm,
              int selectedIdx, unsigned long lastUpdateMs, bool fetching);

    // Status screens
    void drawBoot();
    void drawError(const char* msg);

    // Emergency alert — 3× red ring flash + buzzer beep
    void flashEmergencyRing();

    // Returns index of tapped aircraft, or -1 if none hit
    int hitTest(int tx, int ty,
                const std::vector<Aircraft>& aircraft,
                float centerLat, float centerLon, float radiusKm);

    // Project world coords onto the 240×240 screen
    void worldToScreen(float lat, float lon,
                       float centerLat, float centerLon, float radiusKm,
                       int& sx, int& sy);

private:
    void drawRings(float radiusKm);
    void drawAircraft(const Aircraft& ac, int sx, int sy, bool selected);
    void drawDetail(const Aircraft& ac);
    void drawPollIcon(unsigned long lastUpdateMs, bool fetching);

    static constexpr int CX     = 120;
    static constexpr int CY     = 120;
    static constexpr int PLOT_R = 105;

    // Poll icon — small ring at bottom centre, just inside the round bezel
    static constexpr int ICON_X  = 120;
    static constexpr int ICON_Y  = 232;
    static constexpr int ICON_R  = 7;    // outer radius
    static constexpr int ICON_R0 = 4;    // inner radius (3 px ring)
};
