#pragma once

// Software watchdog: a low-priority task on the other core resets the device if
// the main loop stops making progress for too long. This turns a wedged blocking
// call (e.g. a stuck TLS handshake during a map compose or OpenSky poll — which
// otherwise freezes the device with no crash log) into an automatic recovery.
//
// Call watchdogBegin() once from setup(), and watchdogFeed() from the main loop
// and from any long-running blocking operation (map compose, precache, fetch) so
// legitimate slow work isn't mistaken for a hang.
void watchdogBegin();
void watchdogFeed();
