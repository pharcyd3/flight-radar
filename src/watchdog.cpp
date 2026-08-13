#include "watchdog.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>

static volatile uint32_t   _heartbeat = 0;
static volatile bool       _suspended = false;
static const uint32_t      CHECK_MS   = 3000;    // how often the monitor task checks
static const uint32_t      STALL_MS   = 60000;   // reset after this long with no progress

void watchdogFeed() { _heartbeat++; }

void watchdogSuspend() { _suspended = true; }
void watchdogResume()  { _heartbeat++; _suspended = false; }

// Runs on the other core, so it keeps checking even while the loop task is blocked
// inside a wedged network call. If the heartbeat hasn't advanced for STALL_MS, the
// main loop is genuinely stuck — reboot to recover.
static void watchdogTask(void*) {
    uint32_t last    = _heartbeat;
    uint32_t stalled = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        if (_suspended) {
            last    = _heartbeat;
            stalled = 0;
            continue;
        }
        if (_heartbeat != last) {
            last = _heartbeat;
            stalled = 0;
        } else {
            stalled += CHECK_MS;
            if (stalled >= STALL_MS) {
                Serial.println("[WDT] main loop stalled — restarting");
                Serial.flush();
                esp_restart();
            }
        }
    }
}

void watchdogBegin() {
    // Pin to core 0 (the loop task runs on core 1), low priority.
    xTaskCreatePinnedToCore(watchdogTask, "wdt", 2560, nullptr, 1, nullptr, 0);
}
