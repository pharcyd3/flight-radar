#include "aircraftfeed.h"
#include "adsblive.h"
#include "config.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace feed {
namespace {

// mbedTLS's handshake is the stack-hungry part of the fetch. Measured peak use
// against real traffic (via stackHeadroom(), reported by the INFO debug
// command) is ~5.1 KB, so 8 KB leaves ~3 KB of margin — matching the Arduino
// loop task this fetch used to run on. Anything unused here is heap taken from
// the contiguous block the TLS handshake itself needs.
constexpr uint32_t   TASK_STACK = 8192;
// Core 0 already hosts the WiFi/LWIP stack; the Arduino loop() runs on core 1.
// Pinning the fetch to core 0 keeps the network work off the UI core entirely.
constexpr BaseType_t TASK_CORE  = 0;
// Same priority as the Arduino loop task — this is ordinary application work,
// well below the WiFi driver/lwIP tasks it shares core 0 with.
constexpr UBaseType_t TASK_PRIO = 1;

TaskHandle_t      _task  = nullptr;
SemaphoreHandle_t _goSem = nullptr;   // UI -> task: "start a fetch"
SemaphoreHandle_t _mtx   = nullptr;   // guards _result / _resultOk / _haveResult

// The single hand-off buffer, swapped with the UI's vector rather than copied.
//
// Ownership alternates strictly, so no lock is needed around the parse itself:
// between request() and completion the UI never touches this (takeResult() only
// swaps once _haveResult is set, which the task sets last), and after a swap
// the task gets the UI's old buffer to refill. Keeping it to one buffer instead
// of a separate scratch matters — each is MAX_AIRCRAFT × sizeof(Aircraft) (~10 KB),
// and this no-PSRAM heap must keep a contiguous ~40 KB block free for each TLS
// handshake. It's reserve()d once at startup and only ever swapped, so its
// allocation never moves and can't fragment that block later.
std::vector<Aircraft> _result;

volatile bool _busy       = false;
volatile bool _haveResult = false;
volatile bool _resultOk   = false;

// Request parameters. Written by the UI thread only while !_busy, and read by
// the task only after it takes _goSem (which the UI gives after writing them),
// so that ordering alone makes them safe without a lock.
float _reqLat = 0.0f, _reqLon = 0.0f, _reqRadiusKm = 0.0f;

void taskMain(void*) {
    for (;;) {
        xSemaphoreTake(_goSem, portMAX_DELAY);

        // Parses straight into the hand-off buffer — safe unlocked, see the
        // ownership note on _result.
        bool ok = fetchAircraftAdsbLive(_reqLat, _reqLon, _reqRadiusKm, _result);

        xSemaphoreTake(_mtx, portMAX_DELAY);
        // On failure _result was cleared by the fetch before it gave up, and
        // takeResult() will decline to swap it in. That's deliberate: the UI
        // keeps showing (and dead-reckoning) the last good set through a
        // transient network blip rather than blanking the radar, with the poll
        // icon red to say the feed is unhealthy.
        _resultOk   = ok;
        _haveResult = true;
        xSemaphoreGive(_mtx);

        _busy = false;
    }
}

}  // namespace

void begin() {
    if (_task) return;
    _goSem = xSemaphoreCreateBinary();
    _mtx   = xSemaphoreCreateMutex();
    _result.reserve(MAX_AIRCRAFT);
    xTaskCreatePinnedToCore(taskMain, "acfeed", TASK_STACK, nullptr,
                            TASK_PRIO, &_task, TASK_CORE);
}

bool request(float lat, float lon, float radiusKm) {
    if (!_task || _busy) return false;
    _reqLat = lat; _reqLon = lon; _reqRadiusKm = radiusKm;
    _busy = true;
    xSemaphoreGive(_goSem);
    return true;
}

bool busy() { return _busy; }

bool takeResult(std::vector<Aircraft>& out, bool& ok) {
    if (!_haveResult) return false;
    xSemaphoreTake(_mtx, portMAX_DELAY);
    ok = _resultOk;
    if (ok) out.swap(_result);
    _haveResult = false;
    xSemaphoreGive(_mtx);
    return true;
}

unsigned stackHeadroom() {
    return _task ? uxTaskGetStackHighWaterMark(_task) : 0;
}

}  // namespace feed
