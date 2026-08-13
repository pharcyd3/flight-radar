# Emergency Alerts

Frank's Flight Radar watches every aircraft's transponder squawk code for the three internationally recognised emergency codes:

| Code | Meaning |
|---|---|
| **7500** | Unlawful interference (hijack) |
| **7600** | Radio failure |
| **7700** | General emergency |

## Visual highlighting

Any aircraft squawking one of these codes is drawn differently from normal traffic, regardless of your other display settings:

- Its dot and heading line are always **red** (not affected by colour theme)
- An extra **red ring** is drawn around the dot so it stands out even at a glance
- Its **callsign label is always shown**, overriding the Flight Labels setting

## Audible alert & auto-follow

If **Settings → Buzz on Emergency** is **On** (the default), the first time an emergency squawk is detected:

- The device **automatically enters follow mode** on that aircraft — the same lock-on behaviour as tapping FOLLOW yourself. If you're already following it, nothing changes; if you're following something else (or nothing), the view snaps straight to the emergency aircraft.
- Its follow reticle is drawn **red** (rather than the usual theme colour) for as long as it's the aircraft being tracked, and its **current squawk code is printed underneath it** alongside altitude/speed.
- The screen bezel flashes a red ring ten times
- The device beeps ten times (2.2&nbsp;kHz tones)

This happens once per aircraft per emergency, with a **60-second cooldown** — the same aircraft won't re-trigger the audible/flash/re-follow alert again within 60 seconds, so a single ongoing emergency doesn't repeatedly interrupt you. New updates to its position still redraw it in red on every refresh regardless. Follow it as long as you like, or tap **UNFOLLOW** at any time — it won't re-lock you back on unless a fresh alert fires.

Turning **Buzz on Emergency** off disables the audible alert, ring flash, and auto-follow entirely, but the visual highlighting on the radar (red dot, ring, forced label) always stays on — there's no way to hide an emergency squawk from the display itself.
