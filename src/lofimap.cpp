#include "lofimap.h"

// The blob is linked in by `board_build.embed_files = assets/lofimap.bin`
// (see platformio.ini), which exposes these symbols around it.
extern const uint8_t lofimap_start[] asm("_binary_lofimap_bin_start");
extern const uint8_t lofimap_end[]   asm("_binary_lofimap_bin_end");

namespace lofi {

// Byte-assembly reads so records can sit at any offset — the blob is tightly
// packed, so multi-byte fields land at odd addresses and a direct int16* deref
// would fault on the Xtensa core (unaligned load).
static inline int16_t  rd16(const uint8_t* p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }
static inline uint16_t rdu16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rdu32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t* _lines     = nullptr;
static const uint8_t* _cities    = nullptr;
static const uint8_t* _airports  = nullptr;
static uint32_t       _nLines    = 0;
static uint32_t       _nCities   = 0;
static uint32_t       _nAirports = 0;
static bool           _ready     = false;
static float          _degPerU   = 180.0f / 32767.0f;

bool begin() {
    const uint8_t* p = lofimap_start;
    if (lofimap_end - lofimap_start < 16) return false;
    if (!(p[0] == 'L' && p[1] == 'F' && p[2] == 'M')) return false;
    bool v2 = (p[3] == '2');   // v2 adds the airports section

    int16_t full = rd16(p + 6);
    if (full > 0) _degPerU = 180.0f / (float)full;
    _nLines    = rdu32(p + 8);
    _nCities   = rdu32(p + 12);
    _nAirports = v2 ? rdu32(p + 16) : 0;
    _lines     = p + (v2 ? 20 : 16);

    // Walk the variable-length line records to locate the cities section.
    const uint8_t* q = _lines;
    for (uint32_t i = 0; i < _nLines; ++i) {
        uint16_t n = rdu16(q + 9);           // layer(1) + bbox(8) → nPts at +9
        q += 1 + 8 + 2 + (uint32_t)n * 4;
    }
    _cities = q;

    // Walk the city records (6-byte header + name) to locate airports.
    for (uint32_t i = 0; i < _nCities; ++i)
        q += 6 + q[5];                       // nameLen at +5
    _airports = q;

    _ready = (q <= lofimap_end);
    return _ready;
}

bool           ready()         { return _ready; }
uint32_t       lineCount()     { return _nLines; }
uint32_t       cityCount()     { return _nCities; }
uint32_t       airportCount()  { return _nAirports; }
const uint8_t* linesBegin()    { return _lines; }
const uint8_t* citiesBegin()   { return _cities; }
const uint8_t* airportsBegin() { return _airports; }
float          degPerUnit()    { return _degPerU; }

const uint8_t* readLine(const uint8_t* p, Line& out) {
    out.layer  = p[0];
    out.minLon = rd16(p + 1); out.minLat = rd16(p + 3);
    out.maxLon = rd16(p + 5); out.maxLat = rd16(p + 7);
    out.nPts   = rdu16(p + 9);
    out.pts    = p + 11;
    return out.pts + (uint32_t)out.nPts * 4;
}

const uint8_t* readCity(const uint8_t* p, City& out) {
    out.lon     = rd16(p);
    out.lat     = rd16(p + 2);
    out.rank    = p[4];
    out.nameLen = p[5];
    out.name    = (const char*)(p + 6);
    return p + 6 + out.nameLen;
}

void linePoint(const Line& l, uint16_t k, float& lonDeg, float& latDeg) {
    const uint8_t* pp = l.pts + (uint32_t)k * 4;
    lonDeg = rd16(pp)     * _degPerU;
    latDeg = rd16(pp + 2) * _degPerU;
}

float nearestCity(float lat, float lon, char* out, size_t len) {
    if (out && len) out[0] = '\0';
    if (!_ready || _nCities == 0) return -1.0f;

    float cosLat = cosf(lat * 0.0174532925f);   // longitude compression at this latitude
    const uint8_t* c = _cities;
    float       bestD2   = 1e30f;
    const char* bestName = nullptr;
    uint8_t     bestLen  = 0;
    for (uint32_t i = 0; i < _nCities; ++i) {
        City cc;
        c = readCity(c, cc);
        float dlat = cc.lat * _degPerU - lat;
        float dlon = (cc.lon * _degPerU - lon) * cosLat;
        float d2   = dlat * dlat + dlon * dlon;
        if (d2 < bestD2) { bestD2 = d2; bestName = cc.name; bestLen = cc.nameLen; }
    }
    if (bestName && out && len) {
        size_t n = bestLen < len - 1 ? bestLen : len - 1;
        memcpy(out, bestName, n);
        out[n] = '\0';
    }
    return sqrtf(bestD2) * 111.0f;   // degrees -> approx km
}

}  // namespace lofi
