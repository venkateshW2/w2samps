#pragma once
#include <cmath>
#include <algorithm>

/**
 * PhaseTransform — the 5-stage pipeline that each voice applies to the master phasor.
 *
 * Pipeline (applied in order):
 *
 *   masterPhase (0→1 per beat)
 *        ↓
 *   ① Rate multiply       phase × rateMultiplier
 *        ↓                  >1 = faster, <1 = slower, 2.0 = double speed
 *   ② Phase offset         fmod(phase + offset, 1.0)
 *        ↓                  shifts pattern start in time
 *   ③ Warp / curve         pow(phase, exp(warp × 2))
 *        ↓                  0=linear, +1=rushes early, -1=drags early
 *        ↓                  also animatable via DAW automation or LFO
 *   ④ Reverse              if on: use (1.0 - phase)
 *        ↓                  pattern runs backwards
 *   ⑤ Step quantise        floor(phase × stepsPerLoop) / stepsPerLoop
 *                           turns the smooth ramp into a staircase
 *                           (step crossings = trigger points)
 *
 * Step-quantise amount (0→1):
 *   0.0 = fully smooth ramp (no stepping)
 *   1.0 = hard steps (classic sequencer)
 *   0.5 = slewed steps (organic feel, like a portamento clock)
 *
 * Warp animation notes:
 *   Since warp reads from an AudioParameter each block, it CAN be animated.
 *   A slow LFO or DAW automation curve on warp creates shifting groove.
 *   A hard jump in warp may skip or double-fire a step — this is accepted
 *   behaviour and can be musically interesting.
 *
 * Inter-voice phase source:
 *   The INPUT to this pipeline is normally the raw master phase.
 *   But it can also be another voice's TRANSFORMED phase (for locking/ratio).
 *   The caller selects the input; this struct only does the transform.
 */
struct PhaseTransform
{
    float rateMultiplier = 1.0f;   // 0.125 (÷8) → 8.0 (×8)
    float phaseOffset    = 0.0f;   // 0.0 → 1.0
    float warp           = 0.0f;   // -1.0 → +1.0
    bool  reverse        = false;  // reverse direction
    float quantiseAmount = 1.0f;   // 0=smooth, 1=hard steps
    int   stepsForQuant  = 16;     // number of steps for quantisation

    /**
     * Apply all five stages to an input phase value.
     * Returns the transformed phase, clamped to [0, 1).
     *
     * NOTE: this is called per-BLOCK (at the block boundary) for efficiency.
     * For per-sample accuracy in future, call this once per sample inside
     * the voice's per-sample loop.
     */
    double apply (double inputPhase) const
    {
        // ① Rate multiply — scale how fast this voice moves through a beat
        double p = inputPhase * (double) rateMultiplier;
        p = p - std::floor (p);   // wrap to [0,1)

        // ② Phase offset — shift the pattern's start position
        p = p + (double) phaseOffset;
        p = p - std::floor (p);

        // ③ Warp — non-linear time mapping
        //    pow(p, exp(warp*2)):
        //      warp= 0 → exp(0)=1   → pow(p,1)=p      (linear)
        //      warp=+1 → exp(2)≈7.4 → pow(p,7.4)      (rushes early, drags late)
        //      warp=-1 → exp(-2)≈0.14→ pow(p,0.14)    (drags early, rushes late)
        //    Input is already in [0,1) so this is safe for all warp values.
        if (std::abs (warp) > 0.001f)
        {
            double exponent = std::exp ((double) warp * 2.0);
            p = std::pow (p, exponent);
        }

        // ④ Reverse — play the pattern backwards
        if (reverse)
            p = 1.0 - p;

        // ⑤ Step quantise — blend between smooth ramp and hard staircase
        //    amount=0 → pure ramp
        //    amount=1 → floor(p*N)/N (staircase)
        if (quantiseAmount > 0.001f && stepsForQuant > 0)
        {
            double n       = (double) stepsForQuant;
            double stepped = std::floor (p * n) / n;
            p = p * (1.0 - (double) quantiseAmount)
              + stepped * (double) quantiseAmount;
        }

        // Clamp to [0,1) — rounding errors can push it to exactly 1.0
        return std::max (0.0, std::min (p, 0.9999999));
    }

    /**
     * Given a transformed phase and loop parameters, return the current step index.
     * This is what the sequencer reads to know "which step is now active."
     *
     * stepsPerLoop  — number of steps in one full cycle
     * loopBeats     — how many master-clock beats = one full voice cycle
     *                 (= loopNumerator / loopDenominator)
     */
    static int currentStep (double transformedPhase, int stepsPerLoop, double loopBeats)
    {
        if (stepsPerLoop <= 0 || loopBeats <= 0.0) return 0;
        double pos = transformedPhase * stepsPerLoop / loopBeats;
        return (int) pos % stepsPerLoop;
    }

    /**
     * Count how many step-boundary crossings occurred between phaseStart and phaseEnd.
     * Fills `firedSteps` with the step indices that fired (in order).
     * Handles wrap-around (phaseEnd < phaseStart means the beat wrapped).
     *
     * Returns the number of steps that fired.
     *
     * Used inside processBlock to fire the euclidean pattern correctly.
     */
    static int findStepCrossings (double phaseStart, double phaseEnd,
                                   int stepsPerLoop, double loopBeats,
                                   int* firedSteps, int maxFired)
    {
        if (stepsPerLoop <= 0 || loopBeats <= 0.0) return 0;

        double n = (double) stepsPerLoop / loopBeats;

        // Convert phase to "step position" (fractional step index in [0, stepsPerLoop))
        double posStart = std::fmod (phaseStart * n, (double) stepsPerLoop);
        double posEnd   = std::fmod (phaseEnd   * n, (double) stepsPerLoop);

        int stepStart = (int) posStart;
        int stepEnd   = (int) posEnd;

        int count = 0;

        if (phaseEnd >= phaseStart)
        {
            // No wrap: simple case
            // Fire every step from (stepStart+1) to stepEnd inclusive
            int s = stepStart;
            double pos = posStart;
            while (count < maxFired)
            {
                // Next step boundary
                int nextStep = (s + 1) % stepsPerLoop;
                double nextPos = (double)(s + 1);
                if (nextPos >= (double) stepsPerLoop) nextPos -= (double) stepsPerLoop;

                if (posEnd < (double)(s + 1) - 1e-9 && phaseEnd < phaseStart + 1.0 / n * 1.5)
                    break;  // didn't cross the next boundary

                firedSteps[count++] = nextStep;
                s = nextStep;
                pos = nextPos;

                if (s == stepEnd && pos >= posEnd - 1e-9) break;
                if (count >= maxFired) break;
            }
        }
        else
        {
            // Phase wrapped: two segments
            // Segment 1: posStart → stepsPerLoop
            // Segment 2: 0 → posEnd
            for (int s = stepStart + 1; s < stepsPerLoop && count < maxFired; ++s)
                firedSteps[count++] = s % stepsPerLoop;
            for (int s = 0; s <= stepEnd && count < maxFired; ++s)
                firedSteps[count++] = s;
        }

        // Deduplicate and cap
        if (count == 0 && stepEnd != stepStart)
        {
            // Simple fallback: one crossing
            firedSteps[0] = stepEnd;
            count = 1;
        }

        return count;
    }
};


/**
 * Musical rate multiplier presets.
 * Stored as {display name, float value} pairs.
 * Used by the UI to populate the curated rate button strip.
 */
struct RatePreset { const char* name; float value; };
static constexpr RatePreset kRatePresets[] = {
    { "/8",  0.125f },
    { "/4",  0.25f  },
    { "/2",  0.5f   },
    { "x1",  1.0f   },
    { "x2",  2.0f   },
    { "x4",  4.0f   },
    { "x8",  8.0f   },
};
static constexpr int kNumRatePresets = 7;

/** Find the index of the closest preset to a given rate value. Returns -1 if none within tolerance. */
inline int findRatePresetIndex (float rate)
{
    for (int i = 0; i < kNumRatePresets; ++i)
        if (std::abs (kRatePresets[i].value - rate) < 0.01f)
            return i;
    return -1;
}

/**
 * Musical inter-voice ratio presets (as N:D fractions).
 * Used by the UI's curated ratio button strip.
 */
struct RatioPreset { const char* name; int n; int d; };
static constexpr RatioPreset kRatioPresets[] = {
    { "1:1", 1, 1 },
    { "2:1", 2, 1 },
    { "1:2", 1, 2 },
    { "3:2", 3, 2 },
    { "2:3", 2, 3 },
    { "4:3", 4, 3 },
    { "3:4", 3, 4 },
    { "5:4", 5, 4 },
    { "4:5", 4, 5 },
    { "7:4", 7, 4 },
    { "7:8", 7, 8 },
    { "5:3", 5, 3 },
    { "3:5", 3, 5 },
};
static constexpr int kNumRatioPresets = 13;
