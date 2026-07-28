#pragma once
#include <M5Dial.h>
#include "config.h"

// Turns M5Dial's noisy rotary into reliable, single-step "one click = one
// step" events.
//
// The hardware reports ENC_TICKS_PER_DETENT raw quadrature ticks per physical
// click, and its raw count has been observed to jitter by a tick with no
// physical input — including reverting a tick several hundred ms after a real
// click, which naive "did the detent number change?" handling reads as a
// second, phantom click in the opposite direction.
//
// This rejects that with HYSTERESIS rather than a settling timer: `_anchor`
// holds the raw count at the last committed detent, and a step is only emitted
// once the raw count has moved a *full detent* away from it. A ±1-tick blip
// never reaches the threshold, so it cannot produce a step at all.
//
// The previous implementation instead withheld every step until the reading
// held steady for a fixed window (a full second for zoom). That rejected the
// same noise, but paid the entire window in latency on every genuine click,
// and was the dominant reason zooming felt laggy. Hysteresis is both instant
// and strictly more selective: sub-detent noise isn't out-waited, it simply
// never accumulates into a step.
class EncoderDebouncer {
public:
    void begin() {
        _anchor     = M5Dial.Encoder.read();
        _lastStepMs = 0;
    }

    // Returns true at most once per confirmed detent, writing the net step
    // count (magnitude can exceed 1 on a fast spin) to *outDelta.
    bool poll(int* outDelta) {
        const long raw  = M5Dial.Encoder.read();
        const long diff = raw - _anchor;

        // Inside the dead band — nothing has moved a full click yet.
        if (diff > -ENC_TICKS_PER_DETENT && diff < ENC_TICKS_PER_DETENT)
            return false;

        // Truncate toward zero, so a partially-turned next detent stays
        // pending in the anchor instead of being rounded into this step.
        const long steps = (diff > 0) ?  ( diff / ENC_TICKS_PER_DETENT)
                                      : -((-diff) / ENC_TICKS_PER_DETENT);

        // Rate floor: a burst of steps faster than a finger can physically
        // click is noise, not a spin.
        const unsigned long now = millis();
        if (now - _lastStepMs < ENC_MIN_STEP_MS) return false;

        _anchor    += steps * ENC_TICKS_PER_DETENT;
        _lastStepMs = now;
        *outDelta   = (int)steps;
        return true;
    }

private:
    long          _anchor     = 0;
    unsigned long _lastStepMs = 0;
};
