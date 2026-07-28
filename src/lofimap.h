#pragma once
#include <Arduino.h>

// Read-only access to the embedded lo-fi vector map blob (Natural Earth 1:50m,
// packed by tools/build_lofimap.py and linked into flash rodata). The renderer
// (RadarDisplay::drawLoFiMap) iterates lines + cities straight from flash — no
// heap copy — culling each feature by its stored bounding box.
namespace lofi {

// Layer ids, matching the packer.
enum { LAYER_COAST = 0, LAYER_BORDER = 1, LAYER_WATER = 2 };

struct Line {
    uint8_t  layer;
    int16_t  minLon, minLat, maxLon, maxLat;   // quantised bbox for culling
    uint16_t nPts;
    const uint8_t* pts;   // nPts × (int16 lon, int16 lat), byte-packed
};

struct City {
    int16_t     lon, lat;
    uint8_t     rank;      // 0 = most prominent
    uint8_t     nameLen;
    const char* name;      // NOT null-terminated; length nameLen (ASCII)
};

bool begin();              // parse the header; false if the blob is missing/bad
bool ready();

uint32_t lineCount();
uint32_t cityCount();
uint32_t airportCount();
const uint8_t* linesBegin();
const uint8_t* citiesBegin();
const uint8_t* airportsBegin();   // same record format as cities; name = IATA code

// Decode one record at p; fills out; returns a pointer to the next record.
// readCity also decodes airport records (identical layout).
const uint8_t* readLine(const uint8_t* p, Line& out);
const uint8_t* readCity(const uint8_t* p, City& out);

// Quantised int16 lon/lat of a line's point k, converted to degrees.
void linePoint(const Line& l, uint16_t k, float& lonDeg, float& latDeg);

// Multiply a stored int16 coordinate by this to get degrees.
float degPerUnit();

// Copies the name of the nearest embedded city to (lat,lon) into `out` (ASCII,
// truncated to len) and returns its approximate distance in km, or -1 if the
// data isn't available. Scans the whole city set — fine for occasional use.
float nearestCity(float lat, float lon, char* out, size_t len);

}  // namespace lofi
