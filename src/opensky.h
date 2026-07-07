#pragma once
#include "aircraft.h"
#include <vector>

// Fetches all aircraft within a bounding box derived from (centerLat, centerLon)
// expanded by radiusKm.  Returns true on success and populates `out`.
bool fetchAircraft(float centerLat, float centerLon, float radiusKm,
                   std::vector<Aircraft>& out);
