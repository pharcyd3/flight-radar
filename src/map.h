#pragma once
#include <M5Dial.h>

// OpenStreetMap raster-tile map underlay, composited into a 240x240 RGB565
// sprite and cached to LittleFS. Ported from the emulator's map_tile code
// (fetch_map_tile / _choose_zoom / _deg2tile_f). On the device we can't stitch
// a full tile grid in RAM (no PSRAM), so each tile PNG is decoded straight into
// the sprite at its scaled offset via M5GFX drawPng.
class MapLayer {
public:
    void begin();                 // mount LittleFS + allocate the sprite

    // Compose the map for (lat,lon,radiusKm) if it isn't already current. Loads
    // from the LittleFS cache when possible, else fetches OSM tiles (blocking)
    // and caches the result. Cheap no-op when already current — safe per frame.
    void ensure(float lat, float lon, float radiusKm);

    // Push the current map to the display as the background. Returns false when
    // no map is available (caller should clear the screen itself).
    bool blitTo();

    // ── Off-screen scene compositing ────────────────────────────────────────────
    // The radar composites its whole frame (map + rings + aircraft + overlays)
    // into this one sprite and pushes it in a single transfer, so nothing is ever
    // seen half-drawn — the flicker-free path that also makes per-frame
    // interpolation animation possible. sprite() is the render target; the radar
    // draws into it directly.
    LGFX_Sprite* sprite() { return &_spr; }
    bool ready() const    { return _ready; }

    // Prepare the sprite to hold the pristine map background for (lat,lon,radiusKm)
    // ready for overlays to be drawn on top. Composes/loads if the location or
    // zoom changed; otherwise, if last frame's overlays dirtied the sprite,
    // restores the clean map from the (instant) LittleFS cache. Returns true if a
    // map is available — false means the caller should fill a solid background
    // into the sprite itself (no map / map underlay disabled).
    bool beginScene(float lat, float lon, float radiusKm);

    // Push the fully-composited sprite to the display in one transfer and mark it
    // dirty, so the next beginScene() knows to restore the clean background first.
    void pushScene();

    // Marks the sprite dirty without pushing — used when the radar fills the
    // sprite with a solid (map-off) background, so a later map-on frame restores.
    void markSceneDirty() { _sceneDirty = true; }

    // Fetch + cache a map for (lat,lon,radiusKm) if it isn't already cached,
    // without disturbing what's currently on screen — used to pre-cache saved
    // favourite locations and the other zoom levels so switching is instant.
    // Safe to call from a blocking portal/settings flow; the next ensure() call
    // for the *actual* current home corrects the sprite regardless of what this
    // last touched. Returns true if it actually composed (and therefore dirtied
    // the shared sprite / dropped the live-view state, so the caller should
    // repaint the current view); false if the tile was already cached.
    bool precache(float lat, float lon, float radiusKm);

    // Compose + cache *every* zoom level for (lat,lon) up front, behind a
    // "Loading maps N/M" progress screen — used on a location change so that all
    // zoom levels (and a later lo-fi→full switch) are ready with no gaps once the
    // loading screen clears. Already-cached levels are skipped (fast no-ops).
    void precacheAll(float lat, float lon);

    // Wipes every cached map from flash. Not needed for correctness (the cache
    // is content-addressed by lat/lon/radius so stale entries are just inert),
    // but exposed for a manual "clear map cache" action if ever wanted.
    void invalidate();

    // Deletes cached maps for every location EXCEPT (lat,lon). Keeps the cache
    // bounded to the current home's handful of zoom levels so it can't fill the
    // flash partition — a full partition made saving one level evict another,
    // thrashing endless recomposes (constant TLS at low heap → wedge/freeze risk).
    void pruneExcept(float lat, float lon);

    // Releases the ~45 KB PNG-decoder scratch buffer. Call once all zoom levels
    // are cached (composes become decoder-free cache hits), so the flight-data
    // poll regains that 45 KB — the difference between a big 200 km JSON response
    // fitting or the next TLS handshake being starved of contiguous heap. A
    // later compose (uncached level / new location) re-primes it on demand via
    // ensureDecoder(). No-op if already released.
    void releaseDecoder();

private:
    LGFX_Sprite _spr{&M5Dial.Display};
    bool  _ready        = false;
    bool  _haveMap      = false;
    bool  _decoderReady = false;   // is the pngle scratch buffer currently allocated?
    bool  _sceneDirty   = false;   // has the sprite been overdrawn since the last clean map load?
    bool  _composeComplete = false;  // did the last compose() fetch every tile (safe to cache)?
    float _curLat = 999.0f, _curLon = 999.0f, _curR = -1.0f;

    bool loadCache(float lat, float lon, float r);
    void saveCache(float lat, float lon, float r);
    bool compose(float lat, float lon, float r);   // fetch + decode tiles
    bool ensureDecoder();                          // (re)prime the pngle buffer if freed
};

extern MapLayer mapLayer;
