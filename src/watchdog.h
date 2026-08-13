#pragma once

// Software watchdog: a low-priority task on the other core resets the device if
// the main loop stops making progress for too long. This turns a wedged blocking
// call (e.g. a stuck TLS handshake during a map compose or flight-data poll —
// which otherwise freezes the device with no crash log) into an automatic
// recovery.
//
// Call watchdogBegin() once from setup(), and watchdogFeed() from the main loop
// and from any long-running blocking operation (map compose, precache, fetch) so
// legitimate slow work isn't mistaken for a hang.
void watchdogBegin();
void watchdogFeed();

// For a blocking call we can't instrument with watchdogFeed() ourselves —
// namely WiFiManager's captive portal (autoConnect()/startConfigPortal()),
// which loops internally for minutes waiting on user input. Without this,
// that internal loop looks identical to a genuine hang and the device
// reboots every ~60s, tearing down the AP mid-setup. Safe to blind the
// watchdog for exactly that span because WiFiManager has its own
// setConfigPortalTimeout() bounding it independently; resume immediately
// after so normal operation stays protected the rest of the time.
void watchdogSuspend();
void watchdogResume();
