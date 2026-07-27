// LittleFS.h must be included before map.h (which pulls in M5Dial->M5GFX):
// M5GFX only compiles in its LittleFS-aware drawPngFile() overload if
// LittleFS's include guard is already defined by the time it's processed.
#include <LittleFS.h>
#include "map.h"
#include "config.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>
#include <vector>

MapLayer mapLayer;

static const int    TARGET_PX = 240;
static const size_t MAP_BYTES = 240 * 240 * 2;   // RGB565 sprite buffer size
static const uint16_t GAP_FILL = 0x2104;         // dark grey where a tile is missing

// ── Web-Mercator tile maths (mirrors the emulator) ───────────────────────────

static void deg2tileF(double lat, double lon, int z, double& xt, double& yt) {
    double latr = lat * M_PI / 180.0;
    double n    = pow(2.0, z);
    xt = (lon + 180.0) / 360.0 * n;
    yt = (1.0 - log(tan(latr) + 1.0 / cos(latr)) / M_PI) / 2.0 * n;
}

static int chooseZoom(double lat, double radiusKm, int targetPx, double& mppOut) {
    double mppTarget = (radiusKm * 1000.0 * 2.0) / targetPx;
    for (int z = 1; z < 20; z++) {
        double mpp = 156543.03392 * cos(lat * M_PI / 180.0) / pow(2.0, z);
        if (mpp <= mppTarget) { mppOut = mpp; return z; }
    }
    mppOut = 156543.03392 * cos(lat * M_PI / 180.0) / pow(2.0, 19);
    return 19;
}

// ── Tile fetch ────────────────────────────────────────────────────────────────
// Streams straight to a scratch file on flash in small fixed-size chunks —
// never buffers a whole PNG in RAM. The heap has to hold the 240x240 map
// sprite (~115 KB) plus TLS session state at the same time, so there's no
// reliable contiguous block left for a ~10-50 KB tile; an earlier version
// that buffered tiles in a std::vector threw bad_alloc (exceptions are
// disabled, so it hard-aborted) as soon as the allocator ran out of room.

static const char* TILE_TMP_PATH = "/tile.tmp";

// client/http are created once per compose() call and passed in by reference
// so consecutive tiles (all from the same host, tile.openstreetmap.org) reuse
// one TLS connection instead of paying a full handshake per tile. A fresh
// WiFiClientSecure+HTTPClient per tile was the reason a multi-tile compose
// could take tens of seconds and appear to hang — every tile re-did the
// (relatively slow, on an ESP32) TLS handshake from scratch.
static bool fetchTileToFile(WiFiClientSecure& client, HTTPClient& http,
                            int z, int x, int y, const char* path) {
    char url[96];
    snprintf(url, sizeof(url),
             "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);

    http.begin(client, url);
    // OSM tile-usage policy requires an identifying User-Agent.
    http.addHeader("User-Agent", PRODUCT_UA);
    http.setTimeout(9000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Map] tile %d/%d/%d HTTP %d\n", z, x, y, code);
        http.end();
        return false;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.println("[Map] tile scratch file open failed");
        http.end();
        return false;
    }

    static const size_t MAX_TILE = 96 * 1024;   // real OSM tiles are < ~50 KB
    Stream* stream = http.getStreamPtr();
    uint8_t buf[512];
    size_t  total   = 0;
    unsigned long deadline = millis() + 9000;
    while (http.connected() && millis() < deadline) {
        size_t avail = stream->available();
        if (avail) {
            int r = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
            total += r;
            if (total > MAX_TILE) {   // runaway response — bail
                Serial.printf("[Map] tile %d/%d/%d oversized, dropping\n", z, x, y);
                f.close();
                LittleFS.remove(path);
                http.end();
                return false;
            }
            f.write(buf, r);
        } else {
            delay(1);
        }
        if (http.getSize() > 0 && total >= (size_t)http.getSize()) break;
    }
    f.close();
    http.end();
    return total > 100;   // a valid PNG is well over 100 bytes
}

// ── Compose / cache ───────────────────────────────────────────────────────────

bool MapLayer::compose(float lat, float lon, float r) {
    // Make sure the PNG decoder is allocated (it's freed between precache rounds
    // to give OpenSky polls headroom). If it can't be re-primed on this heap the
    // tiles can't be decoded, so bail — the radar falls back to a solid bg.
    if (!ensureDecoder()) return false;

    double mpp;
    int    z = chooseZoom(lat, r, TARGET_PX, mpp);

    double xtf, ytf;
    deg2tileF(lat, lon, z, xtf, ytf);
    double homeAbsX = xtf * 256.0;      // home position in absolute tile-pixels
    double homeAbsY = ytf * 256.0;

    double pxNeeded = (r * 1000.0 * 2.0) / mpp;   // >= TARGET_PX (we downscale)
    float  s        = (float)(TARGET_PX / pxNeeded);
    double n        = pow(2.0, z);

    // Tiles overlapping the sprite: map the sprite's top-left/bottom-right back
    // into absolute tile-pixels, then to tile indices.
    double absL = homeAbsX + (0   - TARGET_PX / 2) / s;
    double absR = homeAbsX + (TARGET_PX - TARGET_PX / 2) / s;
    double absT = homeAbsY + (0   - TARGET_PX / 2) / s;
    double absB = homeAbsY + (TARGET_PX - TARGET_PX / 2) / s;
    int txmin = (int)floor(absL / 256.0), txmax = (int)floor(absR / 256.0);
    int tymin = (int)floor(absT / 256.0), tymax = (int)floor(absB / 256.0);

    // Safety cap: chooseZoom() is derived from a fixed 240px target so the
    // tile count should stay small (typically 4-9) at any radius, but clamp
    // defensively so a degenerate case can't turn into dozens of sequential
    // fetches — worst case ~9s (a single tile's timeout) per fetch attempt.
    if (txmax - txmin > 5) txmax = txmin + 5;
    if (tymax - tymin > 5) tymax = tymin + 5;

    _spr.fillScreen(GAP_FILL);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    HTTPClient http;
    http.setReuse(true);   // keep the TLS session open across tiles (same host)

    int ok = 0, fail = 0;
    for (int tx = txmin; tx <= txmax; tx++) {
        for (int ty = tymin; ty <= tymax; ty++) {
            int wtx = ((tx % (int)n) + (int)n) % (int)n;   // wrap at date line
            int wty = ((ty % (int)n) + (int)n) % (int)n;

            if (!fetchTileToFile(client, http, z, wtx, wty, TILE_TMP_PATH)) { fail++; continue; }

            int sx = (int)lround((tx * 256.0 - homeAbsX) * s + TARGET_PX / 2);
            int sy = (int)lround((ty * 256.0 - homeAbsY) * s + TARGET_PX / 2);
            // drawPngFile's return value was previously ignored, so a decode
            // failure (e.g. unsupported PNG variant) still counted as "ok" —
            // the tile downloaded fine but nothing was actually drawn, leaving
            // the sprite as a flat GAP_FILL background with no visible error.
            bool drew = _spr.drawPngFile(LittleFS, TILE_TMP_PATH, sx, sy, 0, 0, 0, 0, s, s);
            if (drew) ok++;
            else {
                fail++;
                Serial.printf("[Map] drawPngFile failed for tile %d/%d/%d\n", z, wtx, wty);
            }
        }
    }

    LittleFS.remove(TILE_TMP_PATH);

    // NOTE: we deliberately do NOT release the PNG decoder here (nor in the
    // OpenSky poll — see opensky.cpp). The decoder's scratch buffer is a single
    // ~45 KB contiguous allocation that pngle keeps and reuses once created.
    // On this no-PSRAM board the largest contiguous free block once WiFi/TLS
    // are up is only ~31 KB, so if the buffer is ever freed it can NEVER be
    // re-allocated — that was the "maps only load at one zoom" bug. It's
    // allocated once at boot (begin() priming, while a 45 KB block still
    // exists) and kept resident for the life of the program.

    Serial.printf("[Map] z=%d s=%.2f tiles ok=%d fail=%d heap=%u\n",
                  z, s, ok, fail, ESP.getFreeHeap());
    return ok > 0;
}

static String cachePath(float lat, float lon, float r) {
    char p[48];
    snprintf(p, sizeof(p), "/m%.3f_%.3f_%d.565", lat, lon, (int)r);
    return String(p);
}

bool MapLayer::loadCache(float lat, float lon, float r) {
    File f = LittleFS.open(cachePath(lat, lon, r), "r");
    if (!f) return false;
    if (f.size() != MAP_BYTES) {
        Serial.printf("[Map] cache size mismatch: file=%u expected=%u\n",
                      (unsigned)f.size(), (unsigned)MAP_BYTES);
        f.close();
        return false;
    }
    size_t n = f.read((uint8_t*)_spr.getBuffer(), MAP_BYTES);
    f.close();
    return n == MAP_BYTES;
}

// Cache is content-addressed by (lat,lon,radius), so it's fine to accumulate
// entries indefinitely — this just keeps the 1.5 MB spiffs partition from
// filling up over time (e.g. lots of zoom levels visited at lots of saved
// locations). Evicts the least-recently-written .565 file(s) until there's
// enough headroom for the new one.
static void evictOldMapsIfNeeded(size_t needed) {
    const size_t HEADROOM = 32 * 1024;
    for (int guard = 0; guard < 16; guard++) {
        size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
        if (free >= needed + HEADROOM) return;

        String oldest;
        time_t oldestT = 0;
        File root = LittleFS.open("/");
        if (!root) return;
        for (File e = root.openNextFile(); e; e = root.openNextFile()) {
            String name = e.name();
            if (name.endsWith(".565")) {
                time_t t = e.getLastWrite();
                if (oldest.isEmpty() || t < oldestT) { oldest = name; oldestT = t; }
            }
        }
        root.close();
        if (oldest.isEmpty()) return;   // nothing left to evict
        if (!oldest.startsWith("/")) oldest = "/" + oldest;
        Serial.printf("[Map] evicting %s to free space\n", oldest.c_str());
        LittleFS.remove(oldest);
    }
}

void MapLayer::saveCache(float lat, float lon, float r) {
    evictOldMapsIfNeeded(MAP_BYTES);
    File f = LittleFS.open(cachePath(lat, lon, r), "w");
    if (!f) { Serial.println("[Map] cache write failed"); return; }
    f.write((const uint8_t*)_spr.getBuffer(), MAP_BYTES);
    f.close();
}

// ── Public ────────────────────────────────────────────────────────────────────

// pngle's decompression context is ~44 KB (dominated by a 32 KB LZ window) and
// is cached/reused across drawPngFile() calls once allocated successfully —
// but on a fragmented heap that first 44 KB contiguous allocation can fail,
// and it then retries (and fails) on every single tile forever. Decoding one
// trivial 1x1 PNG here, as early in boot as possible (before WiFi/TLS/LittleFS
// have had a chance to fragment the heap), forces that allocation to happen
// while conditions are best, so it succeeds once and is reused for real tiles.
static const uint8_t PRIME_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00,
    0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0xf7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

void MapLayer::begin() {
    if (!LittleFS.begin(/*formatOnFail=*/true))
        Serial.println("[Map] LittleFS mount failed — tiles won't cache");

    _spr.setColorDepth(16);
    _spr.setPsram(false);
    if (_spr.createSprite(TARGET_PX, TARGET_PX) == nullptr) {
        Serial.println("[Map] sprite alloc failed — map disabled");
        _ready = false;
        return;
    }
    _ready = true;
    Serial.printf("[Map] ready, free heap %u, bufferLength=%u (expected %u)\n",
                  ESP.getFreeHeap(), _spr.bufferLength(), (unsigned)MAP_BYTES);

    unsigned before = ESP.getFreeHeap();
    bool primed = _spr.drawPng(PRIME_PNG, sizeof(PRIME_PNG), 0, 0);
    _decoderReady = primed;
    Serial.printf("[Map] png decoder priming: %s (heap %u -> %u)\n",
                  primed ? "ok" : "FAILED", before, ESP.getFreeHeap());
}

// (Re)allocate the pngle scratch buffer if it's been released. Priming decodes a
// trivial 1x1 PNG, which forces the ~45 KB allocation. Runs from compose(),
// which only happens when the loop is idle (no OpenSky TLS active), so the
// contiguous block is available even though a poll couldn't spare it.
bool MapLayer::ensureDecoder() {
    if (_decoderReady) return true;
    unsigned before = ESP.getFreeHeap();
    _decoderReady = _spr.drawPng(PRIME_PNG, sizeof(PRIME_PNG), 0, 0);
    Serial.printf("[Map] png decoder re-prime: %s (heap %u -> %u)\n",
                  _decoderReady ? "ok" : "FAILED", before, ESP.getFreeHeap());
    return _decoderReady;
}

void MapLayer::releaseDecoder() {
    if (!_decoderReady) return;
    M5Dial.Display.releasePngMemory();
    _decoderReady = false;
    Serial.printf("[Map] png decoder released, free heap %u\n", ESP.getFreeHeap());
}

void MapLayer::ensure(float lat, float lon, float r) {
    if (!_ready) return;
    if (_haveMap &&
        fabsf(lat - _curLat) < 1e-6f &&
        fabsf(lon - _curLon) < 1e-6f &&
        fabsf(r   - _curR)   < 1e-3f)
        return;

    if (loadCache(lat, lon, r)) {
        _haveMap = true;
        Serial.printf("[Map] cache hit %.4f,%.4f @%.0fkm\n", lat, lon, r);
    } else {
        // Fetching several tiles over TLS takes a few seconds — show feedback.
        M5Dial.Display.fillScreen(0x0000);
        M5Dial.Display.setTextDatum(middle_center);
        M5Dial.Display.setTextColor(0x7BEF, 0x0000);
        M5Dial.Display.drawString("loading map...", 120, 120);

        if (compose(lat, lon, r)) {
            saveCache(lat, lon, r);
            _haveMap = true;
        } else {
            _haveMap = false;   // no network/tiles — radar falls back to solid bg
        }
    }
    _curLat = lat; _curLon = lon; _curR = r;
}

bool MapLayer::beginScene(float lat, float lon, float r) {
    if (!_ready) return false;

    bool current = _haveMap &&
                   fabsf(lat - _curLat) < 1e-6f &&
                   fabsf(lon - _curLon) < 1e-6f &&
                   fabsf(r   - _curR)   < 1e-3f;

    if (!current) {
        // Location/zoom changed (or no map yet): load from cache or compose.
        ensure(lat, lon, r);
    } else if (_sceneDirty) {
        // Same view, but last frame drew aircraft/overlays into the sprite —
        // reload the clean map underneath them. A cached .565 load is a straight
        // buffer read (no network/compose), effectively instant.
        loadCache(_curLat, _curLon, _curR);
    }

    _sceneDirty = false;
    return _haveMap;
}

void MapLayer::pushScene() {
    if (!_ready) return;
    _spr.pushSprite(&M5Dial.Display, 0, 0);
    _sceneDirty = true;   // overlays are now baked in — next frame must restore
}

bool MapLayer::blitTo() {
    static bool loggedOnce = false;
    if (!_ready || !_haveMap) {
        Serial.printf("[Map] blitTo skipped: ready=%d haveMap=%d\n", _ready, _haveMap);
        return false;
    }
    _spr.pushSprite(&M5Dial.Display, 0, 0);
    if (!loggedOnce) {
        Serial.println("[Map] blitTo pushed sprite to display");
        loggedOnce = true;
    }
    return true;
}

bool MapLayer::precache(float lat, float lon, float r) {
    if (!_ready) return false;
    File existing = LittleFS.open(cachePath(lat, lon, r), "r");
    if (existing) { existing.close(); return false; }   // already cached — no work

    if (compose(lat, lon, r)) saveCache(lat, lon, r);
    _haveMap = false;   // force the next ensure() to reload for the real view
    _curR    = -1.0f;
    // We composed into the shared sprite and dropped the live-view state, so the
    // caller should repaint the current view (now a fast cache hit) afterwards.
    return true;
}

void MapLayer::invalidate() {
    _haveMap = false;
    _curR    = -1.0f;
    if (!_ready) return;

    // Remove every cached map sprite so a new home location starts fresh.
    File root = LittleFS.open("/");
    if (!root) return;
    std::vector<String> victims;
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        String name = e.name();
        if (name.endsWith(".565")) {
            if (!name.startsWith("/")) name = "/" + name;
            victims.push_back(name);
        }
    }
    root.close();
    for (const String& v : victims) LittleFS.remove(v);
}
