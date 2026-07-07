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

    // Fetch + cache a map for (lat,lon,radiusKm) if it isn't already cached,
    // without disturbing what's currently on screen — used to pre-cache saved
    // favourite locations so switching to one is instant. Safe to call from a
    // blocking portal/settings flow; the next ensure() call for the *actual*
    // current home corrects the sprite regardless of what this last touched.
    void precache(float lat, float lon, float radiusKm);

    // Wipes every cached map from flash. Not needed for correctness (the cache
    // is content-addressed by lat/lon/radius so stale entries are just inert),
    // but exposed for a manual "clear map cache" action if ever wanted.
    void invalidate();

private:
    LGFX_Sprite _spr{&M5Dial.Display};
    bool  _ready   = false;
    bool  _haveMap = false;
    float _curLat = 999.0f, _curLon = 999.0f, _curR = -1.0f;

    bool loadCache(float lat, float lon, float r);
    void saveCache(float lat, float lon, float r);
    bool compose(float lat, float lon, float r);   // fetch + decode tiles
};

extern MapLayer mapLayer;
