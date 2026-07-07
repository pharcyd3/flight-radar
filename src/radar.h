#pragma once
#include <M5Dial.h>
#include "aircraft.h"
#include "opensky.h"
#include <vector>

class RadarDisplay {
public:
    void begin();

    // Full radar redraw. `fetching` flips the poll icon into its "request in
    // flight" look instead of its usual countdown-to-next-poll ring.
    void draw(const std::vector<Aircraft>& aircraft,
              float centerLat, float centerLon, float radiusKm,
              int selectedIdx, unsigned long lastUpdateMs, bool fetching);

    // Redraws just the poll icon (its countdown arc ticks every second) —
    // used for the periodic 1 Hz refresh instead of a full draw() so the
    // map/rings/aircraft aren't needlessly re-pushed (and re-flickered) when
    // nothing about them has actually changed.
    void updatePollIcon(unsigned long lastUpdateMs, bool fetching) {
        drawPollIcon(lastUpdateMs, fetching);
    }

    // Redraws just the API status panel (its "Ns ago" line ticks every
    // second) — same reasoning as updatePollIcon(): the panel is a fully
    // opaque, fixed-geometry overlay, so repainting only it (not the whole
    // map/rings/aircraft underneath) avoids the same re-push-the-whole-frame
    // flicker. No-op if the panel isn't currently open.
    void updateStatusOverlay() {
        if (_showStatus) drawApiStatusOverlay();
    }

    void drawBoot();

    // Emergency alert — 3× red ring flash + buzzer beep
    void flashEmergencyRing();

    // Returns index of tapped aircraft, or -1 if none hit
    int hitTest(int tx, int ty,
                const std::vector<Aircraft>& aircraft,
                float centerLat, float centerLon, float radiusKm);

    // True when a tap at (tx,ty) lands on the poll icon (generous target).
    bool hitPollIcon(int tx, int ty) const;

    // Tap-to-view API status panel toggle.
    void setStatusVisible(bool v) { _showStatus = v; }
    bool statusVisible() const    { return _showStatus; }

    // Project world coords onto the 240×240 screen
    void worldToScreen(float lat, float lon,
                       float centerLat, float centerLon, float radiusKm,
                       int& sx, int& sy);

private:
    void drawRings(float radiusKm);
    void drawAircraft(const Aircraft& ac, int sx, int sy, bool selected);
    void drawDetail(const Aircraft& ac);
    void drawPollIcon(unsigned long lastUpdateMs, bool fetching);
    void drawApiStatusOverlay();

    bool _showStatus = false;

    static constexpr int CX     = 120;
    static constexpr int CY     = 120;
    static constexpr int PLOT_R = 105;

    // Poll icon — small ring at bottom centre, just inside the round bezel
    static constexpr int ICON_X  = 120;
    static constexpr int ICON_Y  = 232;
    static constexpr int ICON_R  = 7;    // outer radius
    static constexpr int ICON_R0 = 4;    // inner radius (3 px ring)
};
