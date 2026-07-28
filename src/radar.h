#pragma once
#include <M5Dial.h>
#include "aircraft.h"
#include "opensky.h"
#include "config.h"
#include <vector>

class RadarDisplay {
public:
    void begin();

    // Full radar redraw. `fetching` flips the poll icon into its "request in
    // flight" look instead of its usual countdown-to-next-poll ring.
    void draw(const std::vector<Aircraft>& aircraft,
              float centerLat, float centerLon, float radiusKm, int zoomIdx,
              int selectedIdx, unsigned long lastUpdateMs, bool fetching);

    // Redraws just the poll icon (its countdown arc ticks every second) —
    // used for the periodic 1 Hz refresh instead of a full draw() so the
    // map/rings/aircraft aren't needlessly re-pushed (and re-flickered) when
    // nothing about them has actually changed.
    void updatePollIcon(unsigned long lastUpdateMs, bool fetching) {
        _g = &M5Dial.Display;   // partial overlay — paint straight onto the display
        drawPollIcon(lastUpdateMs, fetching);
    }

    // Redraws just the API status panel (its "Ns ago" line ticks every
    // second) — same reasoning as updatePollIcon(): the panel is a fully
    // opaque, fixed-geometry overlay, so repainting only it (not the whole
    // map/rings/aircraft underneath) avoids the same re-push-the-whole-frame
    // flicker. No-op if the panel isn't currently open.
    void updateStatusOverlay() {
        if (_showStatus) { _g = &M5Dial.Display; drawApiStatusOverlay(); }
    }

    // Records the current reported positions into each aircraft's breadcrumb
    // trail (matched across polls by icao24). Call once per successful fetch.
    // focusIcao (the selected/followed aircraft, or "") is recorded first so it
    // always keeps a trail slot even when the scene has more aircraft than slots.
    void recordHistory(const std::vector<Aircraft>& aircraft, const char* focusIcao);

    // ── Follow mode ──────────────────────────────────────────────────────────
    // Tells the radar it's tracking an aircraft: centre reticle on the tracked
    // point, home shown as an offset marker, and the UNFOLLOW/SHOW-OTHERS controls.
    // `hideOthers` suppresses every aircraft except the tracked one, for an
    // uncluttered chase. following=false restores the normal home-centred view.
    void setFollow(bool following, float homeLat, float homeLon,
                   const char* label, bool hideOthers);

    // Sets (or clears, if active=false) the followed flight's route for the
    // dotted great-circle line drawn while following (see config.h ROUTE_POINTS).
    // Cheap to call every redraw — the sampled points are only recomputed when
    // the endpoints actually change. Only drawn while following (see draw()).
    void setRoute(bool active, float originLat, float originLon,
                  float destLat, float destLon,
                  const char* originCode, const char* destCode);

    // Great-circle distance in km — used by follow mode to size the auto-fit
    // route view (see config.h FOLLOW_ROUTE_*).
    static float greatCircleKm(float lat1, float lon1, float lat2, float lon2);

    // Index of the aircraft with this icao24, or -1 if it isn't in the set.
    int findByIcao(const std::vector<Aircraft>& aircraft, const char* icao) const;

    // Public wrapper for the dead-reckoned (interpolated) position of an aircraft
    // this frame — follow mode uses it to track a smoothly-moving target.
    void interpPos(const Aircraft& ac, float& lat, float& lon) const {
        effectivePos(ac, lat, lon);
    }

    // True if (lat,lon) projects beyond frac × the plot radius from the centre —
    // follow mode's trigger for re-centring on a target that's drifted too far.
    bool offCenter(float lat, float lon,
                   float centerLat, float centerLon, float radiusKm, float frac);

    // True when a tap lands on the FOLLOW button shown in the detail panel.
    bool hitFollowButton(int tx, int ty) const;

    // True when a tap lands on the "UNFOLLOW <flight>" bar shown while following.
    bool hitUnfollowButton(int tx, int ty) const;

    // True when a tap lands on the SHOW/HIDE OTHERS toggle shown while following.
    bool hitOthersButton(int tx, int ty) const;

    // Renders the offline lo-fi map centred at (lat,lon) with a centre reticle,
    // as the backdrop for the on-device "Set location" screen. Panning is cheap
    // because it's vector, not raster. Pushes a full frame; the caller overlays
    // its own buttons/labels on top afterwards.
    void drawLoFiPan(float centerLat, float centerLon, float radiusKm);

    void drawBoot();

    // Emergency alert — 3× red ring flash + buzzer beep
    void flashEmergencyRing();

    // Returns index of tapped aircraft, or -1 if none hit
    int hitTest(int tx, int ty,
                const std::vector<Aircraft>& aircraft,
                float centerLat, float centerLon, float radiusKm);

    // Cycle the current selection to the next drawn aircraft in direction
    // `dir` (+1 / -1), wrapping. Only considers aircraft that are actually
    // visible (same filter/inside-circle test as draw()), in vector order.
    // Returns the new selected index, or -1 if none are selectable. If
    // `current` isn't currently selectable it jumps to the first/last visible.
    int nextSelectable(int current, int dir,
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
    // Whether an aircraft is currently drawn/selectable: passes the traffic &
    // min-altitude filters and lands inside the radar circle. Shared by draw()
    // and nextSelectable() so scrolling matches exactly what's on screen.
    bool isSelectable(const Aircraft& ac,
                      float centerLat, float centerLon, float radiusKm);

    // Traffic/min-altitude filter only (no projection) — split out of
    // isSelectable() so draw() can test it before doing the single projection.
    bool passesFilter(const Aircraft& ac) const;

    // The aircraft's dead-reckoned position for the current frame: its reported
    // position advanced along its heading at its ground speed for the time since
    // the last fetch (see INTERP_* in config.h). Airborne, moving aircraft only;
    // everything else returns the reported position unchanged.
    void effectivePos(const Aircraft& ac, float& lat, float& lon) const;

    // effectivePos() + worldToScreen() — the projection used for every on-screen
    // aircraft, so marks, hit-testing and trails all agree on where a plane is.
    void acToScreen(const Aircraft& ac,
                    float centerLat, float centerLon, float radiusKm,
                    int& sx, int& sy);

    void drawLoFiMap(float centerLat, float centerLon, float radiusKm);
    // Airport markers + IATA codes — drawn over both the raster and lo-fi maps.
    void drawAirports(float centerLat, float centerLon, float radiusKm);
    // Records a label rect if it doesn't collide with one already placed this
    // frame; returns true if it was placed (so the caller should draw the text).
    bool placeLabel(int lx, int ly, int textW);
    void drawRings(float radiusKm);
    void drawZoomDots(int zoomIdx);
    void drawAircraft(const Aircraft& ac, int sx, int sy, bool selected);
    void drawTrail(const Aircraft& ac, float centerLat, float centerLon, float radiusKm);
    void drawRoute(float centerLat, float centerLon, float radiusKm);
    void drawDetail(const Aircraft& ac);
    void drawFollowButton();
    void drawUnfollowBar();
    void drawOthersButton();
    void drawPollIcon(unsigned long lastUpdateMs, bool fetching);
    void drawApiStatusOverlay();

    // Current render target: the map sprite while compositing a full frame (so it
    // pushes in one flicker-free transfer), or the live display for partial
    // overlay repaints (poll icon / status panel ticks). All draw*() helpers emit
    // through this so the same code serves both paths.
    LovyanGFX* _g = &M5Dial.Display;

    // millis() timestamp of the last fetch, captured at the top of draw() — the
    // time base dead-reckoning interpolation measures elapsed movement from.
    unsigned long _fetchMs = 0;

    // Follow-mode context (set by setFollow()). When active, the view is centred
    // on the tracked aircraft and home is drawn at _homeMark* instead.
    bool  _following        = false;
    bool  _followHideOthers = false;   // hide all but the tracked aircraft while following
    float _homeMarkLat      = 0.0f;
    float _homeMarkLon      = 0.0f;
    char  _followLabel[12]  = "";

    // Route line context (set by setRoute()). _routeLat/_routeLon are sampled
    // great-circle points from origin to destination, precomputed once when the
    // endpoints change (not every frame) and re-projected each redraw.
    bool  _haveRoute   = false;
    float _routeOLat = 0, _routeOLon = 0, _routeDLat = 0, _routeDLon = 0;
    char  _routeOCode[8] = "", _routeDCode[8] = "";
    float _routeLat[ROUTE_POINTS], _routeLon[ROUTE_POINTS];

    bool _showStatus = false;

    // Per-frame label-collision list, shared by city and airport labels so they
    // don't overprint. Reset at the top of each draw().
    struct LabelBox { int x0, y0, x1, y1; };
    LabelBox _labels[16];
    int      _nLabels = 0;

    static constexpr int CX     = 120;
    static constexpr int CY     = 120;
    static constexpr int PLOT_R = 105;

    // Poll icon — small ring at bottom centre, just inside the round bezel
    static constexpr int ICON_X  = 120;
    static constexpr int ICON_Y  = 232;
    static constexpr int ICON_R  = 7;    // outer radius
    static constexpr int ICON_R0 = 4;    // inner radius (3 px ring)

    // FOLLOW button — sits just above the detail pill (which starts y=148)
    static constexpr int FBTN_X = 68;
    static constexpr int FBTN_Y = 126;
    static constexpr int FBTN_W = 104;
    static constexpr int FBTN_H = 20;

    // UNFOLLOW <flight> bar — a single wide button along the bottom shown while
    // following (replaces the detail panel, so the chase view stays uncluttered).
    static constexpr int UFBTN_X = 40;
    static constexpr int UFBTN_Y = 194;
    static constexpr int UFBTN_W = 160;
    static constexpr int UFBTN_H = 26;

    // SHOW/HIDE OTHERS toggle — near the top while following (below the zoom dots).
    static constexpr int OBTN_X = 55;
    static constexpr int OBTN_Y = 40;
    static constexpr int OBTN_W = 130;
    static constexpr int OBTN_H = 22;
};
