#pragma once
#include <cmath>

/**
 * MasterClock — a single phasor that drives all voice channels.
 *
 * One full cycle = beatsPerCycle beats.
 *   beatsPerCycle = 4  →  1 cycle per bar (4/4).  DEFAULT.
 *   beatsPerCycle = 1  →  1 cycle per beat (legacy/fast mode).
 *   beatsPerCycle = 2  →  1 cycle per half-bar.
 *   beatsPerCycle = 8  →  1 cycle per 2 bars.
 *
 * At 120 BPM with beatsPerCycle=4:
 *   one full 0→1 cycle = 2 seconds  (comfortable, bar-level timing)
 *
 * All voices derive their timing from this single phase value.
 * Audio thread only. No locking, no allocation.
 */
class MasterClock
{
public:
    double phase         = 0.0;     // current phasor: 0.0 → 1.0
    double bpm           = 120.0;
    double sampleRate    = 44100.0;
    double beatsPerCycle = 4.0;     // 4 = 1 bar in 4/4  (default)

    void prepare (double sr) { sampleRate = sr; }
    void setBPM  (double b)  { bpm = b; }
    void reset   ()          { phase = 0.0; }

    /** Increment per sample = bpm / (60 × sr × beatsPerCycle). */
    double tickOneSample()
    {
        phase += bpm / (60.0 * sampleRate * beatsPerCycle);
        if (phase >= 1.0) phase -= 1.0;
        return phase;
    }

    double incrementPerBlock (int numSamples) const
    {
        return bpm / (60.0 * sampleRate * beatsPerCycle) * numSamples;
    }

    double phaseAfter (int numSamples) const
    {
        double p = phase + incrementPerBlock (numSamples);
        return p - std::floor (p);
    }
};
