#pragma once
#include <M5Dial.h>
#include "config.h"

// Debounces M5Dial's rotary into reliable, single-step "one click = one
// step" events.
//
// Two layers of noise on this hardware, neither of which the naive
// "did encoderDetent() change?" check handles:
//
//  1. Plain electrical/mechanical bounce right when the value changes —
//     the usual reason any encoder needs debouncing.
//  2. DELAYED bounce: the raw count has been observed to revert to its
//     previous value several hundred ms after a real click, with no
//     further physical input — e.g. raw 0 -> -1 on a click, then -1 -> 0
//     on its own ~500ms later. Since real clicks have also been observed
//     to produce as little as 1 raw tick, the same magnitude as the noise,
//     a real click and a noise blip are indistinguishable by size — only
//     by timing.
//
// This withholds judgement rather than committing optimistically: a new
// reading must hold steady for `stableMs` (see begin()) before it's treated
// as a real step. An earlier version instead committed immediately and only
// *watched* for a delayed bounce-back afterward, undoing the commit if one
// arrived — which sounds equivalent but isn't: that still shows the wrong
// value on screen for however long the bounce takes to arrive, then flips
// it back, which is visibly the exact same "zooms in then bounces back out"
// symptom this whole class exists to prevent. Waiting to commit means the
// wrong value is never shown in the first place — the cost is a fixed
// `stableMs` delay on every step, real or not.
//
// `stableMs` is a per-instance setting (not a single global) because the
// right trade-off differs by use: zooming needs the longer, safer window
// since a wrong zoom flipping is jarring, while moving a menu cursor is
// low-stakes (trivially corrected by continuing to rotate) and feels
// noticeably laggy at the same window, so it uses a shorter one.
class EncoderDebouncer {
public:
    void begin(unsigned long stableMs) {
        _stableMs     = stableMs;
        _stable       = encoderDetent(M5Dial.Encoder.read());
        _hasCandidate = false;
    }

    // Returns true at most once per confirmed step, writing the net detent
    // delta (can be >1 in magnitude if several raw changes settled during
    // the same stability window) to *outDelta.
    bool poll(int* outDelta) {
        long detent = encoderDetent(M5Dial.Encoder.read());
        unsigned long now = millis();

        if (detent == _stable) {
            _hasCandidate = false;   // back at the last confirmed value
            return false;
        }

        if (!_hasCandidate || detent != _candidate) {
            // First sight of this value, or it changed again mid-wait —
            // (re)start the stability clock against the latest reading.
            _candidate        = detent;
            _candidateSinceMs = now;
            _hasCandidate     = true;
            return false;
        }

        if (now - _candidateSinceMs < _stableMs) return false;

        *outDelta     = (int)(_candidate - _stable);
        _stable       = _candidate;
        _hasCandidate = false;
        return true;
    }

private:
    unsigned long _stableMs         = ENC_STABLE_MS_ZOOM;   // safety default if begin() is ever skipped
    long          _stable           = 0;
    long          _candidate        = 0;
    unsigned long _candidateSinceMs = 0;
    bool          _hasCandidate     = false;
};
