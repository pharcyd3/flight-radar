#pragma once
#include <vector>
#include "aircraft.h"

// Background flight-data feed.
//
// fetchAircraftAdsbLive() is a synchronous HTTPS request that can block for
// many seconds on a slow or flaky network (TLS handshake + read timeouts, plus
// a retry). Running it inline in loop() meant M5Dial.update() — which samples
// the touchscreen — and the encoder poll simply did not run for that whole
// stretch: taps were silently dropped and a dial turn could take 15+ s to
// register, because the encoder debouncer needs consecutive polls to confirm a
// step and those polls were seconds apart.
//
// This runs the fetch on its own FreeRTOS task pinned to the radio core, so the
// UI loop keeps sampling input and animating at full rate no matter how badly
// the network is behaving. The UI never blocks on the network again; the worst
// a dead connection can do now is leave the poll icon red.
namespace feed {

// Starts the background task. Call once from setup(), after WiFi is up.
void begin();

// Asks for a fetch centred on (lat,lon) with the given radius. Returns false
// (and does nothing) if a fetch is already in flight.
bool request(float lat, float lon, float radiusKm);

// True while a request is in flight. Drives the poll icon's "in flight" look,
// and gates heap-hungry work (map composes) that must not overlap a TLS
// handshake on this no-PSRAM board.
bool busy();

// Non-blocking. Returns true once per completed fetch. On success the fresh
// aircraft set is swapped into `out` and ok is true; on failure `out` is left
// untouched (so a transient blip doesn't blank the radar) and ok is false.
bool takeResult(std::vector<Aircraft>& out, bool& ok);

// Unused stack words of the fetch task — surfaced by the INFO debug command so
// the mbedTLS handshake's headroom can be checked on real hardware rather than
// guessed at.
unsigned stackHeadroom();

}  // namespace feed
