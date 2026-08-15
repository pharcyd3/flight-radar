#include "radar.h"
#include "config.h"
#include "map.h"
#include "lofimap.h"
#include "settings.h"
#include <math.h>
#include <string.h>

// ── Colour themes (RGB565) ────────────────────────────────────────────────────
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
// dot/thin line with no outline easily disappears against it.
static constexpr uint16_t COL_HALO = 0x0000;

static constexpr float EARTH_R    = 6371.0f;              // km
static constexpr float KM_PER_DEG = EARTH_R * 0.0174532925f;  // km per degree latitude

// Scale an RGB565 colour toward black by factor f (0=black, 1=unchanged) — used
// to fade older trail breadcrumbs. Per-channel multiply on the packed value.
static uint16_t fade(uint16_t c, float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (int)(r * f); g = (int)(g * f); b = (int)(b * f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// ── Trail store ────────────────────────────────────────────────────────────────
// Fixed static ring-buffer of recent reported positions per tracked aircraft,
// keyed by icao24. Static (BSS) rather than heap so it never fragments the
// contiguous free region the TLS handshake / PNG decoder rely on (see config.h).
namespace {
struct Trail {
    char     icao[8];
    float    lat[TRAIL_LEN];
    float    lon[TRAIL_LEN];
    uint8_t  count    = 0;   // valid points held (<= TRAIL_LEN)
    uint8_t  head     = 0;   // next write index (ring buffer)
    uint32_t lastSeen = 0;   // _fetchSeq when last updated — least-recent is evicted
    uint32_t lastPtMs = 0;   // millis() of the newest point — paces TRAIL_MIN_INTERVAL_MS
};
Trail    _trails[MAX_TRAILS];
uint32_t _fetchSeq = 0;

// Index of the trail slot for `icao`, or -1 if none is currently tracked.
int trailSlot(const char* icao) {
    for (int i = 0; i < MAX_TRAILS; ++i)
        if (_trails[i].count && strncmp(_trails[i].icao, icao, sizeof(_trails[i].icao)) == 0)
            return i;
    return -1;
}
}  // namespace

// ── Public ───────────────────────────────────────────────────────────────────

void RadarDisplay::begin() {
    M5Dial.Display.setTextDatum(MC_DATUM);
}

// Recompute the per-frame projection constants. Cheap (three trig calls) and
// hit only when the centre or radius actually changes — i.e. once per frame,
// not once per point. See the cache members in radar.h.
void RadarDisplay::updateProjCache(float cLat, float cLon, float radiusKm) {
    const float RAD = (float)M_PI / 180.0f;
    _pjCLat     = cLat;
    _pjCLon     = cLon;
    _pjRadiusKm = radiusKm;
    float p0    = cLat * RAD;
    _pjSinLat0  = sinf(p0);
    _pjCosLat0  = cosf(p0);
    // Folds EARTH_R (km per radian of arc) and the km→pixel scale into one
    // multiply, replacing the old per-point `EARTH_R * x / radiusKm * PLOT_R`.
    _pjPxPerRad = EARTH_R * (float)PLOT_R / radiusKm;
}

void RadarDisplay::worldToScreen(float lat, float lon,
                                  float cLat, float cLon, float radiusKm,
                                  int& sx, int& sy) {
    // Azimuthal-equidistant projection centred on (cLat,cLon): straight-line
    // (great-circle) distance from the centre maps linearly to screen radius, and
    // bearing is preserved — exactly the model a range-ring radar assumes. At the
    // small distances of normal zoom it is numerically identical to a flat
    // lat/lon projection, but unlike that flat approximation it stays correct all
    // the way out to a whole-earth view (used by follow mode's extended zoom).
    //
    // This is *the* hot path: the lo-fi vector map calls it once per polyline
    // vertex, so it used to spend eight transcendental calls (sinf/cosf of the
    // centre latitude — identical for every point in the frame — plus acosf and
    // sinf for the k factor) on work that is either frame-invariant or
    // negligible at normal zoom. Both are now avoided; the maths is otherwise
    // unchanged, and the wide/global path is still exact.
    if (cLat != _pjCLat || cLon != _pjCLon || radiusKm != _pjRadiusKm)
        updateProjCache(cLat, cLon, radiusKm);

    const float RAD = (float)M_PI / 180.0f;
    float p  = lat * RAD;
    float dl = (lon - cLon) * RAD;

    float sinp  = sinf(p),  cosp  = cosf(p);
    float cosdl = cosf(dl), sindl = sinf(dl);

    float cosc = _pjSinLat0 * sinp + _pjCosLat0 * cosp * cosdl;   // cos(angular distance)
    if (cosc >  1.0f) cosc =  1.0f;
    if (cosc < -1.0f) cosc = -1.0f;

    // k = c/sin(c), where c = acos(cosc). Expanding about c=0 gives
    // k ≈ 1 + c²/6, and c² ≈ 2(1-cosc) to the same order, hence 1 + (1-cosc)/3.
    // At cosc = 0.99 (c ≈ 8.1°, ~900 km from centre — beyond the widest zoom
    // step) the series and the exact value agree to six decimal places, i.e.
    // far under a pixel. Only genuinely wide views pay for acosf/sinf.
    float k;
    if (cosc > 0.99f) k = 1.0f + (1.0f - cosc) * (1.0f / 3.0f);
    else              { float c = acosf(cosc); k = c / sinf(c); }

    float east  = k * cosp * sindl;
    float north = k * (_pjCosLat0 * sinp - _pjSinLat0 * cosp * cosdl);

    sx = CX + (int)(east  * _pjPxPerRad);
    sy = CY - (int)(north * _pjPxPerRad);   // screen Y is inverted
}

// Shared dead-reckoning math: advance (lat0,lon0) along headingDeg at speedMs
// for dt seconds (already elapsed, already capped by the caller).
static void projectLatLon(float lat0, float lon0, float headingDeg, float speedMs,
                          float dt, float& lat, float& lon) {
    float distKm = speedMs * dt / 1000.0f;
    float hdg    = headingDeg * (float)M_PI / 180.0f;     // heading 0 = north, 90 = east
    lat = lat0 + (distKm * cosf(hdg)) / KM_PER_DEG;
    lon = lon0 + (distKm * sinf(hdg)) / (KM_PER_DEG * cosf(lat0 * (float)M_PI / 180.0f));
}

void RadarDisplay::effectivePos(const Aircraft& ac, float& lat, float& lon) const {
    lat = ac.lat;
    lon = ac.lon;
    // Dead reckoning: only for airborne aircraft actually moving. Parked/taxiing
    // marks would just jitter, and on-ground traffic isn't tracked this way.
    if (ac.onGround || ac.speedMs < INTERP_MIN_SPEED_MS) return;

    // Elapsed = how stale the position already was at fetch (posAgeS) + time since
    // the fetch. Starting from the position's true age keeps the mark at its real
    // current estimate, so successive polls line up instead of snapping backward.
    float dt = ac.posAgeS + (float)(millis() - _fetchMs) / 1000.0f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > INTERP_MAX_S) dt = INTERP_MAX_S;             // don't fling stale marks

    projectLatLon(ac.lat, ac.lon, ac.heading, ac.speedMs, dt, lat, lon);
}

// Same dead reckoning as effectivePos(), but for a snapshot that isn't (or may
// not be) in the current poll's aircraft list — follow mode's fallback for
// keeping the fetch box moving with the plane through a failed/empty poll,
// where the normal per-frame path has nothing to look up. dtS is the caller's
// own elapsed-since-last-known-position clock, not tied to the latest fetch
// attempt (which may not be about this aircraft at all).
void RadarDisplay::projectForward(float lat0, float lon0, float headingDeg, float speedMs,
                                  bool onGround, float dtS, float maxS,
                                  float& lat, float& lon) const {
    lat = lat0;
    lon = lon0;
    if (onGround || speedMs < INTERP_MIN_SPEED_MS) return;
    if (dtS < 0.0f) dtS = 0.0f;
    if (dtS > maxS) dtS = maxS;
    projectLatLon(lat0, lon0, headingDeg, speedMs, dtS, lat, lon);
}

void RadarDisplay::acToScreen(const Aircraft& ac,
                              float cLat, float cLon, float radiusKm,
                              int& sx, int& sy) {
    float lat, lon;
    effectivePos(ac, lat, lon);
    worldToScreen(lat, lon, cLat, cLon, radiusKm, sx, sy);
}

int RadarDisplay::hitTest(int tx, int ty,
                           const std::vector<Aircraft>& aircraft,
                           float cLat, float cLon, float radiusKm) {
    // Scan in reverse so topmost-drawn aircraft wins on overlap. Uses the same
    // interpolated position the mark is drawn at, so taps land where the dot is.
    for (int i = (int)aircraft.size() - 1; i >= 0; --i) {
        int sx, sy;
        acToScreen(aircraft[i], cLat, cLon, radiusKm, sx, sy);
        int dx = tx - sx, dy = ty - sy;
        // Generous: the mark is ~5 px but a fingertip covers far more, and
        // there is nothing else on the map to hit by mistake.
        if ((dx * dx + dy * dy) <= 18 * 18) return i;
    }
    return -1;
}

bool RadarDisplay::passesFilter(const Aircraft& ac) const {
    if (trafficFilter() == 0 && ac.onGround) return false;      // airborne-only
    if (ac.altM > 0.0f && ac.altM < minAltitudeM()) return false;
    return true;
}

bool RadarDisplay::isSelectable(const Aircraft& ac,
                                float cLat, float cLon, float radiusKm) {
    if (!passesFilter(ac)) return false;
    int sx, sy;
    acToScreen(ac, cLat, cLon, radiusKm, sx, sy);
    int dx = sx - CX, dy = sy - CY;
    return (dx * dx + dy * dy) <= (PLOT_R * PLOT_R);            // inside circle
}

int RadarDisplay::nextSelectable(int current, int dir,
                                  const std::vector<Aircraft>& aircraft,
                                  float cLat, float cLon, float radiusKm) {
    // Selectable indices, in the same (vector) order they're drawn.
    std::vector<int> vis;
    for (int i = 0; i < (int)aircraft.size(); ++i)
        if (isSelectable(aircraft[i], cLat, cLon, radiusKm)) vis.push_back(i);
    if (vis.empty()) return -1;

    // Where does the current selection sit in that list?
    int pos = -1;
    for (int k = 0; k < (int)vis.size(); ++k)
        if (vis[k] == current) { pos = k; break; }

    // Not currently on a visible aircraft — enter the list at the near end.
    if (pos < 0) return dir >= 0 ? vis.front() : vis.back();

    int n  = (int)vis.size();
    int np = ((pos + dir) % n + n) % n;   // step with wrap-around
    return vis[np];
}

void RadarDisplay::setFollow(bool following, float homeLat, float homeLon,
                             const char* label, bool hideOthers,
                             float targetLat, float targetLon) {
    _following        = following;
    _followHideOthers = hideOthers;
    _homeMarkLat      = homeLat;
    _homeMarkLon      = homeLon;
    _followTargetLat  = targetLat;
    _followTargetLon  = targetLon;
    if (label) {
        strncpy(_followLabel, label, sizeof(_followLabel) - 1);
        _followLabel[sizeof(_followLabel) - 1] = '\0';
    } else {
        _followLabel[0] = '\0';
    }
}

int RadarDisplay::findByIcao(const std::vector<Aircraft>& aircraft,
                             const char* icao) const {
    if (!icao || !icao[0]) return -1;
    for (int i = 0; i < (int)aircraft.size(); ++i)
        if (strncmp(aircraft[i].icao24, icao, sizeof(aircraft[i].icao24)) == 0)
            return i;
    return -1;
}

void RadarDisplay::draw(const std::vector<Aircraft>& aircraft,
                         float cLat, float cLon, float radiusKm, int zoomIdx,
                         int selectedIdx, unsigned long lastUpdateMs, bool fetching) {
    // Time base for dead-reckoning interpolation (see effectivePos()).
    _fetchMs = lastUpdateMs;
    _nLabels = 0;   // reset the per-frame label-collision list (cities + airports)

    // Composite the whole frame into the off-screen map sprite and push it in one
    // transfer, so the frame is never seen half-drawn (the flicker the old
    // push-then-overlay path suffered) and interpolation can animate every redraw.
    // Fall back to drawing straight to the display only if the sprite isn't ready.
    bool useSprite = mapLayer.ready();
    _g = useSprite ? (LovyanGFX*)mapLayer.sprite() : &M5Dial.Display;

    int mm = mapMode();
    // Never use the raster map while the view is moving — following a plane or
    // being dragged around. It would recompose ("loading map...") every time the
    // centre crosses into new tiles, which blocks the UI for seconds at a time
    // and competes with the flight-data fetch for the scarce heap the HTTPS
    // handshake needs. The offline vector map re-centres instantly and covers
    // the whole world, so free browsing stays smooth.
    if ((_following || _viewPanned) && mm == MAP_FULL) mm = MAP_LOFI;

    if (mm == MAP_FULL) {
        // beginScene() leaves the pristine map background in the sprite (loading
        // or restoring as needed); false means no map available → solid fill.
        if (!mapLayer.beginScene(cLat, cLon, radiusKm))
            _g->fillScreen(COL_BG);
    } else {
        // Lo-fi or Off: start from a plain themed background (the raster sprite
        // no longer holds the map, so a later Full frame must restore it).
        _g->fillScreen(COL_BG);
        if (useSprite) mapLayer.markSceneDirty();
        if (mm == MAP_LOFI) drawLoFiMap(cLat, cLon, radiusKm);
    }

    _g->setTextDatum(MC_DATUM);
    drawRings(radiusKm);
    drawZoomDots(zoomIdx);
    pollBattery();
    drawBatteryGauge();

    // Home is always drawn at its true projected position. With the view at rest
    // that is exactly the centre, so this covers the plain case and the moved
    // ones (browsing, or a follow-pan) without a special case — and while
    // browsing it doubles as a pointer back to where home actually is.
    {
        int hx, hy;
        worldToScreen(_homeMarkLat, _homeMarkLon, cLat, cLon, radiusKm, hx, hy);
        _g->drawLine(hx - 6, hy, hx + 6, hy, COL_HOME);
        _g->drawLine(hx, hy - 6, hx, hy + 6, COL_HOME);
        _g->drawCircle(hx, hy, 3, COL_HOME);
    }

    if (_following) {
        // Reticle on the tracked aircraft's true position — normally the centre,
        // but projected wherever it actually falls when the view has been
        // dragged away from it (see main.cpp's follow-pan). Red instead of the
        // usual theme colour while tracking an emergency squawk — checkEmergency()
        // (main.cpp) auto-follows onto exactly this aircraft, so selectedIdx is
        // already resolved to it whenever this is true.
        bool trackedEmergency = selectedIdx >= 0 && selectedIdx < (int)aircraft.size() &&
                                aircraft[selectedIdx].isEmergency();
        int tx, ty;
        worldToScreen(_followTargetLat, _followTargetLon, cLat, cLon, radiusKm, tx, ty);
        drawReticleGlyph(tx, ty, trackedEmergency ? COL_HOME : COL_SEL);

        // Target missing from this fetch (an ADS-B coverage gap) — the loop
        // below never finds a matching aircraft to call drawFollowInfo() on,
        // so without this the reticle sits there with no readout, looking
        // exactly like a follow that's already given up.
        if (selectedIdx < 0 || selectedIdx >= (int)aircraft.size())
            drawFollowSearching(tx, ty);

        // The view is centred exactly on the target (tx,ty == CX,CY) unless
        // main.cpp's follow-pan has shifted it — in which case show a tappable
        // recentre icon (drawn later, over the aircraft) so it's not lost
        // underneath them.
        _showRecenter = (tx != CX || ty != CY);
    } else {
        _showRecenter = _viewPanned;
    }

    // Airports over the map (raster Full or Lo-fi), beneath the aircraft. The
    // lo-fi path already draws its own city labels; airports are added on both.
    // Skipped when zoomed way out — too dense to read.
    if (mm != MAP_OFF && radiusKm <= 1500.0f) drawAirports(cLat, cLon, radiusKm);

    // Breadcrumb trail for the selected aircraft, drawn under the marks.
    if (showTrails() && selectedIdx >= 0 && selectedIdx < (int)aircraft.size())
        drawTrail(aircraft[selectedIdx], cLat, cLon, radiusKm);

    for (int i = 0; i < (int)aircraft.size(); ++i) {
        const Aircraft& ac = aircraft[i];

        // "Hide others" while following: draw only the tracked aircraft.
        if (_following && _followHideOthers && i != selectedIdx) continue;

        // Filter first (cheap), then a single projection — reused for the
        // inside-circle test and the mark, instead of projecting twice.
        if (!passesFilter(ac)) continue;
        int sx, sy;
        acToScreen(ac, cLat, cLon, radiusKm, sx, sy);
        int dx = sx - CX, dy = sy - CY;
        if ((dx * dx + dy * dy) > (PLOT_R * PLOT_R)) continue;   // outside the circle

        drawAircraft(ac, sx, sy, i == selectedIdx);
        if (_following && i == selectedIdx) drawFollowInfo(ac, sx, sy);
    }

    drawPollIcon(fetching);

    if (_showStatus) {
        drawApiStatusOverlay();          // status panel takes precedence
    } else {
        if (_following) {
            drawUnfollowBar();           // chase view controls
            drawOthersButton();
            if (_followLost) drawSignalLostNotice();
            _detailIcao[0] = '\0';      // pill isn't shown here — start fresh
            _detailPage    = 0;         // next time it is (a different plane, most likely)
        } else if (selectedIdx >= 0 && selectedIdx < (int)aircraft.size()) {
            const Aircraft& selAc = aircraft[selectedIdx];
            // A different aircraft than the pill last showed (new selection, or
            // this one re-resolved to a new index after a fetch) always starts
            // back on the summary page.
            if (strncmp(_detailIcao, selAc.icao24, sizeof(_detailIcao)) != 0) {
                strncpy(_detailIcao, selAc.icao24, sizeof(_detailIcao) - 1);
                _detailIcao[sizeof(_detailIcao) - 1] = '\0';
                _detailPage = 0;
            }
            drawDetail(selAc, _detailPage);
            drawFollowButton();          // FOLLOW control above the detail pill
        } else {
            _detailIcao[0] = '\0';
            _detailPage    = 0;
        }
        // Panning works in both modes, so the recentre affordance does too.
        if (_showRecenter) {
            drawRecenterIcon();
            // "Make this home" only makes sense when the centre is a place you
            // chose, not an aircraft that's flying away from you.
            if (!_following) drawSetHomeIcon();
        }
    }

    if (useSprite) mapLayer.pushScene();
}

// Draw the selected aircraft's breadcrumb trail: recorded reported positions
// oldest→newest, fading in from dim, with a final segment to the live
// (interpolated) position so the trail connects to where the mark actually is.
void RadarDisplay::drawTrail(const Aircraft& ac,
                             float cLat, float cLon, float radiusKm) {
    int slot = trailSlot(ac.icao24);
    if (slot < 0) return;
    const Trail& t = _trails[slot];
    if (t.count < 1) return;

    int prevx = 0, prevy = 0;
    bool have = false;
    for (int k = 0; k < t.count; ++k) {
        // Ring order: oldest point is count back from head, newest at head-1.
        int idx = (t.head + TRAIL_LEN - t.count + k) % TRAIL_LEN;
        int px, py;
        worldToScreen(t.lat[idx], t.lon[idx], cLat, cLon, radiusKm, px, py);

        float f = t.count > 1 ? (float)k / (float)(t.count - 1) : 1.0f;  // 0=old..1=new
        if (have)
            _g->drawLine(prevx, prevy, px, py, fade(COL_SEL, 0.25f + 0.75f * f));
        _g->fillCircle(px, py, 1, fade(COL_SEL, 0.30f + 0.70f * f));
        prevx = px; prevy = py; have = true;
    }

    // Final segment: newest reported point → current interpolated position.
    int cx, cy;
    acToScreen(ac, cLat, cLon, radiusKm, cx, cy);
    _g->drawLine(prevx, prevy, cx, cy, COL_SEL);
}

// Append the current reported positions to each aircraft's trail, keyed by
// icao24. Called once per successful fetch (not per frame — trails are the
// *reported* breadcrumbs; interpolation fills in between them).
// Append one aircraft's current position to its trail (allocating/evicting a
// slot as needed). Crucially, a slot already updated *this* poll is never evicted
// — otherwise, in a scene with more aircraft than trail slots, planes would wipe
// each other's trails within a single poll (which is what made the followed
// plane's trail vanish over a busy airport). Overflow aircraft simply get no
// trail; only the selected one is ever drawn, and it's recorded first (below).
static void recordTrailPoint(const Aircraft& ac) {
    int slot = trailSlot(ac.icao24);
    if (slot < 0) {
        int oldest = -1;
        uint32_t oldestSeen = 0xFFFFFFFFu;
        for (int i = 0; i < MAX_TRAILS; ++i) {
            if (_trails[i].count == 0) { slot = i; break; }
            if (_trails[i].lastSeen != _fetchSeq && _trails[i].lastSeen < oldestSeen) {
                oldestSeen = _trails[i].lastSeen; oldest = i;
            }
        }
        if (slot < 0) slot = oldest;
        if (slot < 0) return;   // every slot already used this poll — skip overflow
        Trail& nt = _trails[slot];
        strncpy(nt.icao, ac.icao24, sizeof(nt.icao) - 1);
        nt.icao[sizeof(nt.icao) - 1] = '\0';
        nt.count = 0;
        nt.head  = 0;
    }

    Trail& t = _trails[slot];
    uint32_t now = millis();
    if (t.count) {
        // Pace points by time, not by poll, so the trail spans a consistent
        // stretch of flight instead of shrinking as the refresh rate rises
        // (see TRAIL_MIN_INTERVAL_MS). Still mark the slot seen, or a plane
        // present every poll would look stale and get evicted.
        if (now - t.lastPtMs < TRAIL_MIN_INTERVAL_MS) { t.lastSeen = _fetchSeq; return; }
        // Skip a new point if the aircraft has barely moved (~<50 m), so a
        // near-stationary target doesn't pile identical breadcrumbs.
        int last = (t.head + TRAIL_LEN - 1) % TRAIL_LEN;
        float dlat = ac.lat - t.lat[last], dlon = ac.lon - t.lon[last];
        if (dlat * dlat + dlon * dlon < 2.5e-7f) { t.lastSeen = _fetchSeq; return; }
    }
    t.lat[t.head] = ac.lat;
    t.lon[t.head] = ac.lon;
    t.head = (t.head + 1) % TRAIL_LEN;
    if (t.count < TRAIL_LEN) t.count++;
    t.lastSeen = _fetchSeq;
    t.lastPtMs = now;
}

void RadarDisplay::recordHistory(const std::vector<Aircraft>& aircraft,
                                 const char* focusIcao) {
    _fetchSeq++;
    // Record the focused (selected / followed) aircraft first, so it always
    // secures a trail slot even when the scene has more aircraft than slots.
    if (focusIcao && focusIcao[0]) {
        for (const Aircraft& ac : aircraft)
            if (strncmp(ac.icao24, focusIcao, sizeof(ac.icao24)) == 0) {
                recordTrailPoint(ac);
                break;
            }
    }
    for (const Aircraft& ac : aircraft) recordTrailPoint(ac);
}

void RadarDisplay::debugDumpTrails(const char* focusIcao, float radiusKm) const {
    Serial.printf("TRAILS seq=%lu showTrails=%d focus=%s slot=%d\n",
                  (unsigned long)_fetchSeq, showTrails() ? 1 : 0,
                  focusIcao && focusIcao[0] ? focusIcao : "-",
                  (focusIcao && focusIcao[0]) ? trailSlot(focusIcao) : -1);
    for (int i = 0; i < MAX_TRAILS; ++i) {
        const Trail& t = _trails[i];
        if (!t.count) continue;
        // End-to-end span, in km and in screen pixels at the current zoom —
        // the number that actually decides whether the trail is visible at all
        // rather than buried under the aircraft mark's ~7 px halo.
        int oldest = (t.head + TRAIL_LEN - t.count) % TRAIL_LEN;
        int newest = (t.head + TRAIL_LEN - 1) % TRAIL_LEN;
        float dLatKm = (t.lat[newest] - t.lat[oldest]) * KM_PER_DEG;
        float dLonKm = (t.lon[newest] - t.lon[oldest]) * KM_PER_DEG *
                       cosf(t.lat[newest] * (float)M_PI / 180.0f);
        float km = sqrtf(dLatKm * dLatKm + dLonKm * dLonKm);
        Serial.printf("TRAILS  slot=%d icao=%s count=%u head=%u lastSeen=%lu "
                      "span=%.2fkm (%.0fpx@%.0fkm)\n",
                      i, t.icao, t.count, t.head, (unsigned long)t.lastSeen,
                      km, km / radiusKm * PLOT_R, radiusKm);
    }
}

void RadarDisplay::drawBoot() {
    // Composite into the sprite and push (like draw()), so the splash can also be
    // captured for screenshots — the panel itself can't be read back.
    bool useSprite = mapLayer.ready();
    _g = useSprite ? (LovyanGFX*)mapLayer.sprite() : &M5Dial.Display;
    auto& d = *_g;
    d.fillScreen(COL_BG);
    d.setTextDatum(MC_DATUM);

    // ── Plane icon: a top-down airliner silhouette, nose up, drawn from vector
    // primitives so it stays crisp and picks up the active theme colour ──
    const int      px    = CX, py = 80;   // icon centre
    const uint16_t plane = COL_RING_LBL;

    // Swept main wings from mid-fuselage
    d.fillTriangle(px, py - 4,  px - 36, py + 16,  px, py + 10, plane);
    d.fillTriangle(px, py - 4,  px + 36, py + 16,  px, py + 10, plane);
    // Swept tailplane (horizontal stabiliser)
    d.fillTriangle(px, py + 14, px - 15, py + 28,  px, py + 24, plane);
    d.fillTriangle(px, py + 14, px + 15, py + 28,  px, py + 24, plane);
    // Fuselage + pointed nose cone
    d.fillRoundRect(px - 4, py - 26, 8, 56, 4, plane);
    d.fillTriangle(px - 4, py - 22, px + 4, py - 22, px, py - 34, plane);

    // ── Title ──
    d.setTextColor(COL_RING_LBL, COL_BG);
    d.setTextSize(2);
    d.drawString("Frank's", CX, CY + 30);
    d.drawString("Flight Radar", CX, CY + 54);

    if (useSprite) mapLayer.pushScene();
}

// ── Private ──────────────────────────────────────────────────────────────────

// Draws the embedded lo-fi vector map (coastlines/borders/rivers + city labels)
// as themed lines into the current target, culling each feature by its stored
// bounding box. The plain themed background has already been filled by draw().
void RadarDisplay::drawLoFiMap(float cLat, float cLon, float radiusKm) {
    if (!lofi::ready()) return;

    // View bounding box in degrees, with a small margin so features straddling
    // the edge still draw. cosf(cLat) accounts for longitude compression.
    float cosLat = cosf(cLat * (float)M_PI / 180.0f);
    if (cosLat < 0.01f) cosLat = 0.01f;
    float dLat = (radiusKm / KM_PER_DEG) * 1.15f;
    float dLon = (radiusKm / (KM_PER_DEG * cosLat)) * 1.15f;
    float vMinLon = cLon - dLon, vMaxLon = cLon + dLon;
    float vMinLat = cLat - dLat, vMaxLat = cLat + dLat;
    float u = lofi::degPerUnit();

    // At very wide (regional→global) zoom, nothing is culled, so drop the dense
    // layers to keep the redraw quick and the picture legible: rivers/lakes and
    // city labels just become clutter that far out, leaving coastlines + borders.
    bool wide = radiusKm > 1500.0f;

    // Rivers and lakes are by far the densest layer — many short polylines, so
    // the per-line bounding-box test rejects few of them and the cost scales
    // with view area. They also stop reading as anything but noise once a single
    // pixel is a couple of km. Dropping them past the 100 km zoom step keeps the
    // two widest steps cheap, which is where the frame time was worst; coastlines,
    // borders, cities and airports (the actually useful landmarks) all stay.
    bool dropWater = radiusKm > 150.0f;

    // ── Lines ──
    const uint8_t* p = lofi::linesBegin();
    lofi::Line L;
    for (uint32_t i = 0; i < lofi::lineCount(); ++i) {
        p = lofi::readLine(p, L);
        if (dropWater && L.layer == lofi::LAYER_WATER) continue;   // see dropWater above
        if (L.maxLon * u < vMinLon || L.minLon * u > vMaxLon ||
            L.maxLat * u < vMinLat || L.minLat * u > vMaxLat) continue;   // cull

        uint16_t col = (L.layer == lofi::LAYER_COAST)  ? COL_RING_LBL
                     : (L.layer == lofi::LAYER_BORDER) ? COL_GND
                                                       : COL_RING;   // water
        // The bounding-box test above only rejects lines that miss the view
        // entirely. A long polyline (a whole national coastline is one record)
        // usually *overlaps* the view while lying almost entirely outside it, and
        // every one of those far-away vertices used to cost a full projection
        // plus a drawLine that clipped away to nothing. That is what made wide
        // zooms crawl, since bbox culling stops helping as the view grows.
        //
        // So: give each point a Cohen-Sutherland outcode against the (already
        // margined) view box — four float compares, no trig — and skip any
        // segment whose endpoints both lie beyond the same edge, as such a
        // segment cannot cross the view. Points are then projected lazily, only
        // when a segment actually survives. Segments that might be visible are
        // drawn exactly as before, so the picture is unchanged.
        int   prevOut = 0;
        float prevLon = 0.0f, prevLat = 0.0f;
        int   px = 0, py = 0;
        bool  havePrev = false, prevProjected = false;

        for (uint16_t k = 0; k < L.nPts; ++k) {
            float lon, lat;
            lofi::linePoint(L, k, lon, lat);

            int out = 0;
            if      (lon < vMinLon) out |= 1;
            else if (lon > vMaxLon) out |= 2;
            if      (lat < vMinLat) out |= 4;
            else if (lat > vMaxLat) out |= 8;

            if (havePrev && (out & prevOut) == 0) {
                if (!prevProjected)
                    worldToScreen(prevLat, prevLon, cLat, cLon, radiusKm, px, py);
                int sx, sy;
                worldToScreen(lat, lon, cLat, cLon, radiusKm, sx, sy);
                _g->drawLine(px, py, sx, sy, col);   // sprite clips off-screen
                px = sx; py = sy;
                prevProjected = true;
            } else {
                prevProjected = false;   // px,py no longer match the previous point
            }

            prevLon = lon; prevLat = lat; prevOut = out; havePrev = true;
        }
    }

    // ── Cities: dots + de-collided labels (blob is rank-sorted, biggest first).
    // Reserve most of the shared label budget for airports (placed afterwards) ──
    const uint8_t* c = lofi::citiesBegin();
    lofi::City C;
    int dots = 0;
    for (uint32_t i = 0; !wide && i < lofi::cityCount() && dots < 40; ++i) {
        c = lofi::readCity(c, C);
        float lon = C.lon * u, lat = C.lat * u;
        if (lon < vMinLon || lon > vMaxLon || lat < vMinLat || lat > vMaxLat) continue;

        int sx, sy;
        worldToScreen(lat, lon, cLat, cLon, radiusKm, sx, sy);
        int ddx = sx - CX, ddy = sy - CY;
        if (ddx * ddx + ddy * ddy > PLOT_R * PLOT_R) continue;   // inside the scope only

        _g->fillCircle(sx, sy, 1, COL_RING_LBL);
        ++dots;

        char nm[25];
        int  nl = C.nameLen < 24 ? C.nameLen : 24;
        memcpy(nm, C.name, nl);
        nm[nl] = '\0';

        if (_nLabels < 8 && placeLabel(sx + 3, sy - 3, nl * 6)) {
            _g->setTextDatum(ML_DATUM);
            _g->setTextSize(1);
            _g->setTextColor(COL_STATUS, COL_BG);
            _g->drawString(nm, sx + 3, sy - 3);
        }
    }
    _g->setTextDatum(MC_DATUM);   // restore the default datum for later text
}

bool RadarDisplay::placeLabel(int lx, int ly, int textW) {
    LabelBox r{ lx - 1, ly - 5, lx + textW, ly + 5 };
    for (int j = 0; j < _nLabels; ++j) {
        LabelBox& q = _labels[j];
        if (!(r.x1 < q.x0 || r.x0 > q.x1 || r.y1 < q.y0 || r.y0 > q.y1)) return false;
    }
    if (_nLabels >= (int)(sizeof(_labels) / sizeof(_labels[0]))) return false;
    _labels[_nLabels++] = r;
    return true;
}

// Airports: a bold square marker + IATA code in the accent colour, distinct from
// the round city dots. Drawn over both the raster and lo-fi maps (the key
// landmarks for a flight radar). Shares the frame's label-collision budget.
void RadarDisplay::drawAirports(float cLat, float cLon, float radiusKm) {
    if (!lofi::ready()) return;

    float cosLat = cosf(cLat * (float)M_PI / 180.0f);
    if (cosLat < 0.01f) cosLat = 0.01f;
    float dLat = (radiusKm / KM_PER_DEG) * 1.15f;
    float dLon = (radiusKm / (KM_PER_DEG * cosLat)) * 1.15f;
    float vMinLon = cLon - dLon, vMaxLon = cLon + dLon;
    float vMinLat = cLat - dLat, vMaxLat = cLat + dLat;
    float u = lofi::degPerUnit();

    const uint8_t* ap = lofi::airportsBegin();
    lofi::City A;   // airports share the city record layout; A.name is the IATA code
    int adots = 0;
    for (uint32_t i = 0; i < lofi::airportCount() && adots < 24; ++i) {
        ap = lofi::readCity(ap, A);
        float lon = A.lon * u, lat = A.lat * u;
        if (lon < vMinLon || lon > vMaxLon || lat < vMinLat || lat > vMaxLat) continue;

        int sx, sy;
        worldToScreen(lat, lon, cLat, cLon, radiusKm, sx, sy);
        int ddx = sx - CX, ddy = sy - CY;
        if (ddx * ddx + ddy * ddy > PLOT_R * PLOT_R) continue;

        _g->fillRect(sx - 3, sy - 3, 6, 6, COL_HALO);   // dark halo for contrast
        _g->fillRect(sx - 2, sy - 2, 4, 4, COL_SEL);
        ++adots;

        char code[8];
        int  cl = A.nameLen < 6 ? A.nameLen : 6;
        memcpy(code, A.name, cl);
        code[cl] = '\0';

        if (placeLabel(sx + 5, sy - 4, cl * 6)) {
            _g->setTextDatum(ML_DATUM);
            _g->setTextSize(1);
            _g->setTextColor(COL_SEL, COL_BG);
            _g->drawString(code, sx + 5, sy - 4);
        }
    }
    _g->setTextDatum(MC_DATUM);
}

void RadarDisplay::drawLoFiPan(float cLat, float cLon, float radiusKm) {
    bool useSprite = mapLayer.ready();
    _g = useSprite ? (LovyanGFX*)mapLayer.sprite() : &M5Dial.Display;

    _g->fillScreen(COL_BG);
    if (useSprite) mapLayer.markSceneDirty();
    _nLabels = 0;
    drawLoFiMap(cLat, cLon, radiusKm);   // themed vector lines + city labels
    drawAirports(cLat, cLon, radiusKm);  // airport markers + IATA codes

    // Centre reticle — where the crosshair lands becomes the chosen location.
    _g->drawCircle(CX, CY, 7, COL_SEL);
    _g->drawLine(CX - 12, CY, CX - 4, CY, COL_SEL);
    _g->drawLine(CX + 4, CY, CX + 12, CY, COL_SEL);
    _g->drawLine(CX, CY - 12, CX, CY - 4, COL_SEL);
    _g->drawLine(CX, CY + 4, CX, CY + 12, COL_SEL);
    _g->fillCircle(CX, CY, 2, COL_HOME);

    if (useSprite) mapLayer.pushScene();
}

// Refresh the cached battery reading, at most every BATT_POLL_MS.
//
// Caveat worth knowing before debugging this: M5Unified does not configure any
// battery sensing for board_M5Dial. Boards that do (the near-identical
// M5DinMeter, for one) name an ADC channel and divider ratio in
// Power_Class::begin(); M5Dial names none, so getBatteryLevel() falls through
// to `return -2`. If that is still true on your library version the gauge
// simply won't appear — which is the intended failure, not a silent bug.
//
// So take a level where we can get one and fall back to deriving it from
// voltage, since a board can report a usable voltage without a percentage.
// Requiring a plausible cell voltage either way keeps a gauge from being drawn
// off the USB rail on a device with no cell fitted. The BATT debug command
// prints the raw values behind this decision.
void RadarDisplay::pollBattery() {
    unsigned long now = millis();
    if (_battReadMs != 0 && now - _battReadMs < BATT_POLL_MS) return;
    _battReadMs = now;

    int32_t level = M5.Power.getBatteryLevel();
    int32_t mv    = M5.Power.getBatteryVoltage();

    // A single LiPo cell: ~3.3 V effectively empty, ~4.2 V full. Anything
    // outside this band is not a cell we're reading.
    const bool haveMv = (mv > 3000 && mv < 4500);
    if ((level < 0 || level > 100) && haveMv) {
        level = ((int32_t)mv - 3300) * 100 / (4200 - 3300);
        if (level < 0)   level = 0;
        if (level > 100) level = 100;
    }

    _battPresent  = (level >= 0 && level <= 100 && haveMv);
    _battLevel    = _battPresent ? (int)level : -1;
    _battCharging = (M5.Power.isCharging() == m5::Power_Class::is_charging);
}

// Small battery cell, top right. Outline + proportional fill, with a bolt
// through it while charging — the same glyph in both states, so it reads as
// one indicator rather than two different ones appearing and disappearing.
void RadarDisplay::drawBatteryGauge() {
    if (!_battPresent) return;

    const int x = BATT_X, y = BATT_Y, w = BATT_W, h = BATT_H;

    // Dark halo behind the whole glyph, for the same reason the aircraft marks
    // have one: this sits over the map, which can be light.
    _g->fillRect(x - 1, y - 1, w + 5, h + 2, COL_HALO);

    uint16_t col = (_battLevel <= 10) ? COL_HOME        // critical
                 : (_battLevel <= 30) ? COL_SEL         // low
                                      : COL_RING_LBL;   // normal

    _g->drawRect(x, y, w, h, col);                      // body
    _g->fillRect(x + w, y + 3, 2, h - 6, col);          // terminal nub

    // Proportional fill inside a 1 px gap from the outline.
    int innerW = w - 4;
    int fill   = (_battLevel * innerW + 50) / 100;
    if (fill > 0) _g->fillRect(x + 2, y + 2, fill, h - 4, col);

    if (_battCharging) {
        // Bolt, drawn as two triangles. Outlined in the halo colour first so it
        // stays legible whether it lands on the filled or the empty part.
        int bx = x + w / 2, by = y + h / 2;
        for (int d = 1; d >= 0; --d) {
            uint16_t c = d ? COL_HALO : COL_SEL;
            _g->fillTriangle(bx + 1 + d, by - 4 - d, bx - 3 - d, by + 1 + d, bx + d, by + 1, c);
            _g->fillTriangle(bx - 1 - d, by + 4 + d, bx + 3 + d, by - 1 - d, bx - d, by - 1, c);
        }
    }
}

void RadarDisplay::drawRings(float radiusKm) {
    if (!showRings()) return;

    // Three concentric rings (100%, 50%, 25% of radius)
    _g->drawCircle(CX, CY, PLOT_R,     COL_RING);
    _g->drawCircle(CX, CY, PLOT_R / 2, COL_RING);
    _g->drawCircle(CX, CY, PLOT_R / 4, COL_RING);

    // North tick + label
    _g->drawLine(CX, CY - PLOT_R, CX, CY - PLOT_R + 8, COL_RING_LBL);
    _g->setTextSize(1);
    _g->setTextColor(COL_RING_LBL, COL_BG);
    _g->drawString("N", CX, CY - PLOT_R - 8);

    // Range labels on the 50% and 100% rings (right side). Uses the bright
    // ring-label colour (same as "N"), not the dim ring colour itself — the
    // dim colour is nearly identical to the background for every theme, so
    // the text body was essentially invisible and only faint anti-aliased
    // glyph edges showed, which is what read as "little black triangles".
    char buf[16];
    _g->setTextColor(COL_RING_LBL, COL_BG);
    // Both labels sit just INSIDE their own ring, right-aligned, so a wider
    // string (e.g. "400km" at the max zoom step) grows away from the other
    // label instead of into it. The inner label used to be centred on a
    // fixed offset that assumed a short string — fine at "25km", but wide
    // enough at "200km"/"400km" to visibly overlap the outer label, which
    // read as the two labels squashed together.
    _g->setTextDatum(MR_DATUM);
    snprintf(buf, sizeof(buf), "%.0fkm", radiusKm * 0.5f);
    _g->drawString(buf, CX + PLOT_R / 2 - 4, CY);
    // Outer label: right-aligned at CX + PLOT_R - 6 rather than centred at
    // CX + PLOT_R + 14, which landed at x=239 on a 240 px display — most of
    // it was clipped off the edge, and the round bezel cuts in further still.
    snprintf(buf, sizeof(buf), "%.0fkm", radiusKm);
    _g->drawString(buf, CX + PLOT_R - 6, CY);
    _g->setTextDatum(MC_DATUM);   // restore — the rest of the frame assumes it
}

// Row of dots near the top of the round display — one per zoom step, with the
// current level highlighted (bigger + bright) — so the zoom level reads at a
// glance without having to parse the "Nkm" ring labels. Each dot gets a dark
// halo behind it for the same map-contrast reason as the aircraft marks.
void RadarDisplay::drawZoomDots(int zoomIdx) {
    static constexpr int DOT_Y        = 26;   // just below the "N" tick
    static constexpr int DOT_GAP      = 11;   // fits ZOOM_COUNT dots clear of the battery gauge
    static constexpr int DOT_R        = 3;
    static constexpr int DOT_R_ACTIVE = 4;

    int count = ZOOM_COUNT;
    int startX = CX - (count - 1) * DOT_GAP / 2;
    for (int i = 0; i < count; i++) {
        int  x      = startX + i * DOT_GAP;
        bool active = (i == zoomIdx);
        int  r      = active ? DOT_R_ACTIVE : DOT_R;
        _g->fillCircle(x, DOT_Y, r + 2, COL_HALO);
        _g->fillCircle(x, DOT_Y, r, active ? COL_SEL : COL_RING_LBL);
    }
}

void RadarDisplay::drawAircraft(const Aircraft& ac, int sx, int sy, bool selected) {
    bool emergency = ac.isEmergency();
    uint16_t col = emergency  ? COL_HOME   // 7500/7600/7700 — always red, unmissable
                 : selected   ? COL_SEL
                 : ac.onGround ? COL_GND : COL_AC;

    if (aircraftIconStyle() == 1) {
        drawAircraftPlaneIcon(ac, sx, sy, selected, emergency, col);
    } else {
        int radius = selected ? 5 : 4;   // bumped up a notch — small dots were hard to spot
        if (emergency) radius += 1;

        // Heading arrow — starts just outside the body dot's edge instead of at
        // its exact centre (the dot is filled *after* this, on top, so a line
        // starting at the centre had its first few px hidden underneath it) and
        // is long enough to actually read as a direction indicator. A dark halo
        // drawn first (wider) gives it contrast against the map underlay —
        // a bare 1px line in aircraft colour all but disappears over
        // light-coloured map tiles.
        if (showTrails() && !ac.onGround && ac.speedMs > 5.0f) {
            float rad  = ac.heading * (float)M_PI / 180.0f;
            float sinR = sinf(rad), cosR = cosf(rad);
            const int   gap = radius + 1;
            const float len = 13.0f;
            float sx0 = sx + sinR * gap,       sy0 = sy - cosR * gap;
            float ex  = sx + sinR * (gap + len), ey = sy - cosR * (gap + len);
            _g->drawWideLine(sx0, sy0, ex, ey, 2.6f, COL_HALO);
            _g->drawWideLine(sx0, sy0, ex, ey, 1.2f, col);
        }

        // Body dot — dark halo ring behind it for the same map-contrast reason,
        // then an extra red ring further out still for emergencies so they read
        // as "highlighted" at a glance rather than just "a red dot instead of
        // white".
        _g->fillCircle(sx, sy, radius + 2, COL_HALO);
        if (emergency) {
            _g->drawCircle(sx, sy, radius + 4, COL_HOME);
        }
        _g->fillCircle(sx, sy, radius, col);
    }

    // Callsign label: Off (never) / Selected (only this one) / All
    int labels = flightLabels();
    bool showLabel = (labels == 2) || (labels == 1 && selected) || emergency;
    if (showLabel) {
        _g->setTextSize(1);
        _g->setTextColor(emergency ? COL_HOME : (selected ? COL_SEL : COL_STATUS), COL_BG);
        _g->drawString(ac.callsign, sx, sy - 16);
    }
}

// A single dart-shaped triangle pointing along ac.heading — nose forward,
// two back corners swept out to a point behind. Stands in for both the dot
// and its separate heading arrow at once, so there's no redundant line to
// also draw here.
void RadarDisplay::drawAircraftPlaneIcon(const Aircraft& ac, int sx, int sy,
                                         bool selected, bool emergency, uint16_t col) {
    int nose = selected ? 9 : 7;
    int back = selected ? 4 : 3;
    int wing = selected ? 5 : 4;
    if (emergency) { nose += 1; wing += 1; }

    // Dark halo behind it for map contrast, same reasoning as the dot mark;
    // sized to cover the triangle's footprint rather than just the centre.
    _g->fillCircle(sx, sy, nose + 2, COL_HALO);
    if (emergency) _g->drawCircle(sx, sy, nose + 4, COL_HOME);

    float rad = ac.heading * (float)M_PI / 180.0f;
    float s = sinf(rad), c = cosf(rad);   // heading direction (0=up, matches worldToScreen)
    float px = c, py = s;                 // unit vector perpendicular to heading

    int nx = sx + (int)(s * nose), ny = sy - (int)(c * nose);
    int bx = sx - (int)(s * back), by = sy + (int)(c * back);
    int lx = bx - (int)(px * wing), ly = by - (int)(py * wing);
    int rx = bx + (int)(px * wing), ry = by + (int)(py * wing);
    _g->fillTriangle(nx, ny, lx, ly, rx, ry, col);
}

void RadarDisplay::drawFollowInfo(const Aircraft& ac, int sx, int sy) {
    bool metric = activeUnits() == 1;
    char altBuf[16];
    if (ac.altM > 0.0f) {
        if (metric) snprintf(altBuf, sizeof(altBuf), "%d m", (int)ac.altM);
        else        snprintf(altBuf, sizeof(altBuf), "%d ft", (int)(ac.altM * 3.28084f));
    } else {
        snprintf(altBuf, sizeof(altBuf), "n/a");
    }
    int speed = metric ? (int)(ac.speedMs * 3.6f) : (int)(ac.speedMs * 1.94384f);

    char buf[32];
    snprintf(buf, sizeof(buf), "%s  %d %s", altBuf, speed, metric ? "km/h" : "kts");

    _g->setTextSize(1);
    _g->setTextColor(COL_SEL, COL_BG);
    _g->drawString(buf, sx, sy + 16);

    if (ac.isEmergency()) {
        char sq[16];
        snprintf(sq, sizeof(sq), "SQUAWK %s", ac.squawk[0] ? ac.squawk : "----");
        _g->setTextColor(COL_HOME, COL_BG);
        _g->drawString(sq, sx, sy + 32);
    }
}

void RadarDisplay::drawFollowSearching(int tx, int ty) {
    _g->setTextSize(1);
    _g->setTextColor(COL_SEL, COL_BG);
    _g->drawString("SEARCHING...", tx, ty + 16);
}

void RadarDisplay::drawDetail(const Aircraft& ac, int page) {
    // Pill-shaped overlay in the lower portion of the round screen
    _g->fillRoundRect(DETAIL_X, DETAIL_Y, DETAIL_W, DETAIL_H, 6, COL_OVERLAY);
    _g->drawRoundRect(DETAIL_X, DETAIL_Y, DETAIL_W, DETAIL_H, 6, COL_SEL);

    char buf[48];
    int lineY = 160;
    const int step = 16;

    // Row 1: callsign + ICAO — shown on both pages, so flipping never reads
    // as having jumped to a different aircraft.
    _g->setTextSize(1);
    _g->setTextColor(COL_SEL, COL_OVERLAY);
    snprintf(buf, sizeof(buf), "%s  [%s]",
             ac.callsign[0] ? ac.callsign : "N/A", ac.icao24);
    _g->drawString(buf, CX, lineY);   lineY += step;

    _g->setTextColor(COL_STATUS, COL_OVERLAY);

    bool metric = activeUnits() == 1;

    if (page == 0) {
        // Row 2: altitude (ft or m)
        if (ac.altM > 0.0f) {
            if (metric) snprintf(buf, sizeof(buf), "Alt %d m", (int)ac.altM);
            else        snprintf(buf, sizeof(buf), "Alt %d ft", (int)(ac.altM * 3.28084f));
        } else {
            snprintf(buf, sizeof(buf), "Alt  n/a");
        }
        _g->drawString(buf, CX, lineY);   lineY += step;

        // Row 3: speed (kts or km/h) + heading. The default font has no glyph
        // at all for the degree sign — a correct UTF-8 encoding still fell
        // back to a placeholder "tofu" box, because the font itself simply
        // lacks that character, not because of an encoding mismatch. Drawing
        // a small hollow circle by hand right after the number sidesteps
        // needing the font to have it at all.
        int speed = metric ? (int)(ac.speedMs * 3.6f) : (int)(ac.speedMs * 1.94384f);
        snprintf(buf, sizeof(buf), "%d %s  %03.0f", speed, metric ? "km/h" : "kts", ac.heading);
        _g->drawString(buf, CX, lineY);
        int textW = _g->textWidth(buf);
        _g->drawCircle(CX + textW / 2 + 5, lineY - 4, 2, COL_STATUS);
        lineY += step;

        // Row 4: type / on-ground flag
        snprintf(buf, sizeof(buf), "%s%s", ac.country, ac.onGround ? "  [GND]" : "");
        _g->drawString(buf, CX, lineY);
    } else if (page == 1) {
        // Row 2: raw position — the summary page already covers type/on-ground
        // (row 4) and distance-from-home below covers a derived value, but not
        // the plane-spotter-useful raw lat/lon itself.
        snprintf(buf, sizeof(buf), "%.3f, %.3f", ac.lat, ac.lon);
        _g->drawString(buf, CX, lineY);   lineY += step;

        // Row 3: squawk code
        snprintf(buf, sizeof(buf), "Squawk %s", ac.squawk[0] ? ac.squawk : "n/a");
        _g->drawString(buf, CX, lineY);   lineY += step;

        // Row 4: distance from home + how fresh this position report is.
        // Planar approximation, same idiom as the trail-span calc above and
        // lofimap's tile math — fine at these sub-1000 km distances.
        float dLatKm = (ac.lat - _homeMarkLat) * KM_PER_DEG;
        float dLonKm = (ac.lon - _homeMarkLon) * KM_PER_DEG *
                       cosf(_homeMarkLat * (float)M_PI / 180.0f);
        float homeKm = sqrtf(dLatKm * dLatKm + dLonKm * dLonKm);
        // Same "age at fetch + elapsed since" reckoning effectivePos() uses,
        // so this agrees with how stale the plotted position actually is.
        float ageS = ac.posAgeS + (float)(millis() - _fetchMs) / 1000.0f;
        if (metric) snprintf(buf, sizeof(buf), "%.0f km  %.0fs old", homeKm, ageS);
        else        snprintf(buf, sizeof(buf), "%.0f mi  %.0fs old", homeKm * 0.621371f, ageS);
        _g->drawString(buf, CX, lineY);
    } else {
        // Identity page — registration, full aircraft description, and
        // operator/airline, straight from airplanes.live's DB lookup. New
        // data, not shown on the other two pages, so blank/unknown rather
        // than derived: shows "n/a" rather than falling back to a value
        // already visible elsewhere.

        // Row 2: registration (tail number)
        snprintf(buf, sizeof(buf), "Reg  %s", ac.reg[0] ? ac.reg : "n/a");
        _g->drawString(buf, CX, lineY);   lineY += step;

        // Row 3: aircraft description (e.g. "Airbus A320-214")
        _g->drawString(ac.desc[0] ? ac.desc : "Type n/a", CX, lineY);   lineY += step;

        // Row 4: operator/airline
        _g->drawString(ac.ownOp[0] ? ac.ownOp : "Operator n/a", CX, lineY);
    }

    // Disclosure chevron, pinned to the pill's edge (clear of the centred
    // text regardless of string width): right-pointing "there's more" on
    // every page but the last, left-pointing "back to the start" there.
    // Inset 16px rather than hugging the edge — the pill sits low on a round
    // screen, and the visible chord narrows fast there, so an edge-hugging
    // chevron reads as clipped against the bezel.
    bool lastPage = (page == DETAIL_PAGES - 1);
    int dir = lastPage ? -1 : 1;
    int ax  = lastPage ? DETAIL_X + 16 : DETAIL_X + DETAIL_W - 16;
    int ay  = DETAIL_Y + DETAIL_H / 2;
    _g->fillTriangle(ax - dir * 4, ay - 5, ax - dir * 4, ay + 5, ax + dir * 4, ay, COL_SEL);
}

void RadarDisplay::drawFollowButton() {
    _g->fillRoundRect(FBTN_X, FBTN_Y, FBTN_W, FBTN_H, 5, COL_OVERLAY);
    _g->drawRoundRect(FBTN_X, FBTN_Y, FBTN_W, FBTN_H, 5, COL_SEL);
    _g->setTextDatum(MC_DATUM);
    _g->setTextSize(1);
    _g->setTextColor(COL_SEL, COL_OVERLAY);
    _g->drawString("FOLLOW", CX, FBTN_Y + FBTN_H / 2);
}

void RadarDisplay::drawUnfollowBar() {
    _g->fillRoundRect(UFBTN_X, UFBTN_Y, UFBTN_W, UFBTN_H, 6, COL_OVERLAY);
    _g->drawRoundRect(UFBTN_X, UFBTN_Y, UFBTN_W, UFBTN_H, 6, COL_HOME);
    _g->setTextDatum(MC_DATUM);
    _g->setTextSize(1);
    _g->setTextColor(COL_HOME, COL_OVERLAY);
    char buf[24];
    snprintf(buf, sizeof(buf), "UNFOLLOW %s", _followLabel);
    _g->drawString(buf, CX, UFBTN_Y + UFBTN_H / 2);
}

void RadarDisplay::drawOthersButton() {
    _g->fillRoundRect(OBTN_X, OBTN_Y, OBTN_W, OBTN_H, 5, COL_OVERLAY);
    _g->drawRoundRect(OBTN_X, OBTN_Y, OBTN_W, OBTN_H, 5, COL_SEL);
    _g->setTextDatum(MC_DATUM);
    _g->setTextSize(1);
    _g->setTextColor(COL_SEL, COL_OVERLAY);
    _g->drawString(_followHideOthers ? "SHOW OTHERS" : "HIDE OTHERS",
                   OBTN_X + OBTN_W / 2, OBTN_Y + OBTN_H / 2);
}

void RadarDisplay::drawReticleGlyph(int x, int y, uint16_t col) {
    _g->drawCircle(x, y, 9, col);
    _g->drawLine(x - 12, y, x - 5, y, col);
    _g->drawLine(x + 5, y, x + 12, y, col);
    _g->drawLine(x, y - 12, x, y - 5, col);
    _g->drawLine(x, y + 5, x, y + 12, col);
}

bool RadarDisplay::hitSetHomeIcon(int tx, int ty) const {
    int dx = tx - HOME_BTN_X, dy = ty - HOME_BTN_Y;
    return (dx * dx + dy * dy) <= (18 * 18);
}

// Small house: pitched roof over a body, with a door. Drawn in the home colour
// so it reads as "this is about home" at a glance, against the same dark halo
// the other marks use for contrast over the map.
void RadarDisplay::drawSetHomeIcon() {
    const int x = HOME_BTN_X, y = HOME_BTN_Y;
    _g->fillCircle(x, y, 13, COL_HALO);
    _g->fillTriangle(x - 9, y - 1, x, y - 10, x + 9, y - 1, COL_HOME);   // roof
    _g->drawRect(x - 6, y - 1, 13, 10, COL_HOME);                        // body
    _g->drawRect(x - 2, y + 4, 5, 5, COL_HOME);                          // door
}

bool RadarDisplay::hitRecenterIcon(int tx, int ty) const {
    int dx = tx - RCTR_X, dy = ty - RCTR_Y;
    return (dx * dx + dy * dy) <= (20 * 20);   // generous, like the poll icon
}

// "No signal" pill, sat just above the UNFOLLOW bar and clear of the centre
// reticle. Counts up so it's obvious the view is coasting and roughly how long
// it has been doing so.
void RadarDisplay::drawSignalLostNotice() {
    const int w = 132, h = 18, x = CX - w / 2, y = 172;
    _g->fillRoundRect(x, y, w, h, 4, COL_OVERLAY);
    _g->drawRoundRect(x, y, w, h, 4, COL_HOME);
    _g->setTextDatum(MC_DATUM);
    _g->setTextSize(1);
    _g->setTextColor(COL_HOME, COL_OVERLAY);
    char buf[32];
    unsigned long m = _followLostSecs / 60, sec = _followLostSecs % 60;
    snprintf(buf, sizeof(buf), "NO SIGNAL  %lu:%02lu", m, sec);
    _g->drawString(buf, CX, y + h / 2);
}

void RadarDisplay::drawRecenterIcon() {
    // Dark halo behind it for contrast against the map, same reasoning as the
    // aircraft marks — an unfilled reticle alone can vanish over light tiles.
    _g->fillCircle(RCTR_X, RCTR_Y, 13, COL_HALO);
    drawReticleGlyph(RCTR_X, RCTR_Y, COL_SEL);
}

void RadarDisplay::flashEmergencyRing() {
    // 3 px thick ring just inside the round screen bezel (radius 115–117)
    auto& d = M5Dial.Display;
    const int FLASHES = 10;
    for (int flash = 0; flash < FLASHES; flash++) {
        for (int r = 115; r <= 117; r++)
            d.drawCircle(CX, CY, r, COL_HOME);   // COL_HOME = red
        M5Dial.Speaker.tone(2200, 300);   // fills the ring's full 300ms on-screen — was 150, left the back half silent
        delay(300);
        for (int r = 115; r <= 117; r++)
            d.drawCircle(CX, CY, r, COL_BG);
        if (flash < FLASHES - 1) delay(180);
    }
}

void RadarDisplay::drawPollIcon(bool fetching) {
    if (!showPollIcon()) return;   // Settings → Poll sweep = Off
    auto& d = *_g;

    // A rotating sweep, not a countdown.
    //
    // This used to drain an arc towards the next scheduled poll, and go solid
    // for the whole time a request was in flight. That reads as broken, because
    // a fetch is a large and *variable* slice of the interval — a wide-zoom poll
    // can take seconds — so the ring sat motionless mid-cycle, and if a fetch ran
    // longer than the interval the arc pinned at empty and stopped moving
    // altogether. It was reporting the truth, but the truth is not something a
    // countdown can express.
    //
    // A sweep driven by millis() instead of by the schedule can't freeze: it
    // turns whenever the device is alive, so a stopped sweep now means something
    // genuinely wedged rather than merely a slow poll. Colour still carries the
    // feed's health, which is the part actually worth glancing at.
    //
    // Opaque disc behind the ring first. drawArc only paints between ICON_R0 and
    // ICON_R, leaving the middle transparent — so without this, whatever the last
    // full composite left underneath stayed visible inside the icon and made it
    // look like it had debris in it. Same dark-halo treatment the home and
    // recentre icons already use, so it reads as one solid element over the map.
    d.fillCircle(ICON_X, ICON_Y, ICON_R, COL_HALO);
    d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, 0, 360, COL_RING);   // dim track

    uint16_t col = apiFailed(apiStatus().state) ? COL_HOME      // feed unhealthy
                 : fetching                     ? COL_SEL       // request in flight
                                                : COL_RING_LBL; // idle, healthy

    // ~1 revolution per 1.4 s. The icon is a handful of pixels painted straight
    // to the display, so it's ticked far faster than the radar composite (see
    // POLL_ICON_TICK_MS in main.cpp) and the motion stays smooth.
    const int len   = 80;                                   // sweep length, degrees
    const int start = (int)((millis() / 4UL) % 360UL);
    int end = start + len;
    if (end <= 360) {
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, start, end, col);
    } else {
        // Wrapped past 12 o'clock — draw it as two pieces.
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, start, 360, col);
        d.drawArc(ICON_X, ICON_Y, ICON_R, ICON_R0, 0, end - 360, col);
    }
}

bool RadarDisplay::hitPollIcon(int tx, int ty) const {
    // Nothing drawn there when the sweep is off, so don't swallow the tap —
    // it should fall through to ordinary aircraft selection. The status panel
    // is still reachable from Settings → API status.
    if (!showPollIcon()) return false;
    // Generous target — the icon itself is tiny and sits near the bottom bezel.
    int dx = tx - ICON_X, dy = ty - ICON_Y;
    return (dx * dx + dy * dy) <= (22 * 22);
}

bool RadarDisplay::hitFollowButton(int tx, int ty) const {
    return tx >= FBTN_X && tx <= FBTN_X + FBTN_W &&
           ty >= FBTN_Y && ty <= FBTN_Y + FBTN_H;
}

bool RadarDisplay::hitDetailPill(int tx, int ty) const {
    return tx >= DETAIL_X && tx <= DETAIL_X + DETAIL_W &&
           ty >= DETAIL_Y && ty <= DETAIL_Y + DETAIL_H;
}

// A few px of slop beyond the drawn edge on both — short buttons close to the
// round bezel (OTHERS is only 22 px tall) are otherwise an easy miss for a
// normal-sized fingertip, on top of the down-position/last-position mismatch
// fixed in main.cpp's follow touch handler.
static constexpr int BTN_SLOP = 6;

bool RadarDisplay::hitUnfollowButton(int tx, int ty) const {
    return tx >= UFBTN_X - BTN_SLOP && tx <= UFBTN_X + UFBTN_W + BTN_SLOP &&
           ty >= UFBTN_Y - BTN_SLOP && ty <= UFBTN_Y + UFBTN_H + BTN_SLOP;
}

bool RadarDisplay::hitOthersButton(int tx, int ty) const {
    return tx >= OBTN_X - BTN_SLOP && tx <= OBTN_X + OBTN_W + BTN_SLOP &&
           ty >= OBTN_Y - BTN_SLOP && ty <= OBTN_Y + OBTN_H + BTN_SLOP;
}

void RadarDisplay::drawApiStatusOverlay() {
    auto& d = *_g;
    const ApiStatus& s = apiStatus();

    d.fillRoundRect(28, 66, 184, 124, 8, COL_OVERLAY);
    d.drawRoundRect(28, 66, 184, 124, 8, COL_SEL);

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
    d.drawString("API STATUS", CX, 84);

    d.setTextSize(2);
    d.setTextColor(col, COL_OVERLAY);
    d.drawString(label, CX, 110);

    char buf[48];
    d.setTextSize(1);
    d.setTextColor(COL_STATUS, COL_OVERLAY);

    // Reason / aircraft count from the last attempt
    d.drawString(s.detail[0] ? s.detail : "-", CX, 136);

    // HTTP code + payload size
    if (s.httpCode)
        snprintf(buf, sizeof(buf), "HTTP %d   %dB", s.httpCode, s.bytes);
    else
        snprintf(buf, sizeof(buf), "no request yet");
    d.drawString(buf, CX, 154);

    // Age of last attempt + effective poll interval, so the chosen cadence is
    // visible (e.g. "12s ago  every 8s").
    if (s.lastMs) {
        unsigned long age = (millis() - s.lastMs) / 1000UL;
        snprintf(buf, sizeof(buf), "%lus ago  every %lus",
                 age, refreshIntervalMs() / 1000UL);
        d.drawString(buf, CX, 172);
    }
}
