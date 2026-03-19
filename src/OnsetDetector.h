#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>

/**
 * OnsetDetector — finds transient onset positions in an audio buffer.
 *
 * Algorithm: energy-based spectral flux (no FFT required).
 *   1. Compute short-term RMS energy in overlapping windows
 *   2. Compute positive energy differences (flux) between consecutive windows
 *   3. Find peaks above an adaptive threshold
 *   4. Return positions normalised to [0, 1]
 *
 * Also estimates BPM from inter-onset interval autocorrelation.
 *
 * This runs OFFLINE on the message thread after a file is loaded.
 * Never call from the audio thread — it allocates memory.
 *
 * Good for: drums, percussive one-shots, rhythmic material.
 * Less good for: smooth pads (few onsets), complex polyphony.
 *
 * Future upgrade: replace energy flux with FFT-based spectral flux for
 * better accuracy on melodic content. The Result struct stays the same.
 */
class OnsetDetector
{
public:
    struct Result
    {
        std::vector<float> positions;    // onset positions, normalised [0, 1]
        float              estimatedBPM; // best-guess BPM from onset intervals (20–300)
        int                count;        // shorthand for positions.size()
    };

    /**
     * Analyse a buffer and return all detected onsets.
     *
     * buf              — the audio data (any number of channels)
     * sampleRate       — file's sample rate
     * sensitivity      — 0.0 (very few onsets) → 1.0 (very many), default 0.5
     *                    multiplies the adaptive threshold: higher = fewer detections
     */
    static Result analyse (const juce::AudioBuffer<float>& buf,
                           double sampleRate,
                           float sensitivity = 0.5f)
    {
        Result result;
        result.estimatedBPM = 120.0f;
        result.count = 0;

        int numSamples  = buf.getNumSamples();
        int numChannels = buf.getNumChannels();
        if (numSamples < kWindowSize || numChannels == 0) return result;

        // ── 1. Compute short-term energy per hop window ─────────────────────
        std::vector<float> energy;
        energy.reserve ((size_t)(numSamples / kHopSize + 1));

        for (int pos = 0; pos + kWindowSize <= numSamples; pos += kHopSize)
        {
            float e = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = buf.getReadPointer (ch);
                for (int i = 0; i < kWindowSize; ++i)
                    e += data[pos + i] * data[pos + i];
            }
            energy.push_back (e / (float)(kWindowSize * numChannels));
        }

        if (energy.size() < 3) return result;

        // ── 2. Spectral flux: positive energy differences ───────────────────
        std::vector<float> flux;
        flux.reserve (energy.size() - 1);
        for (size_t i = 1; i < energy.size(); ++i)
            flux.push_back (std::max (0.0f, energy[i] - energy[i - 1]));

        // ── 3. Adaptive threshold ────────────────────────────────────────────
        // Use a local mean in a window around each frame × sensitivity multiplier.
        // sensitivity=0.5 → thresholdMult≈2.0; sensitivity=0 → 1.0; sens=1.0 → 3.0
        float thresholdMult = 1.0f + sensitivity * 4.0f;

        // Global mean as simple baseline
        float globalMean = std::accumulate (flux.begin(), flux.end(), 0.0f)
                           / (float) flux.size();
        float threshold = globalMean * thresholdMult;

        // ── 4. Peak-picking ──────────────────────────────────────────────────
        // A peak must be: > threshold AND > both neighbours AND > min spacing
        static constexpr int kMinSpacingHops = 4;  // ~23ms at 44100/256
        int lastPeakHop = -kMinSpacingHops - 1;

        for (int i = 1; i < (int) flux.size() - 1; ++i)
        {
            bool aboveThreshold = flux[(size_t) i] > threshold;
            bool localMax       = flux[(size_t) i] > flux[(size_t)(i - 1)]
                               && flux[(size_t) i] > flux[(size_t)(i + 1)];
            bool spaced         = (i - lastPeakHop) >= kMinSpacingHops;

            if (aboveThreshold && localMax && spaced)
            {
                // Convert hop index to normalised position
                int samplePos = i * kHopSize;
                float normPos = (float) samplePos / (float) numSamples;
                result.positions.push_back (normPos);
                lastPeakHop = i;
            }
        }

        // ── 5. Estimate BPM from inter-onset intervals ───────────────────────
        if (result.positions.size() >= 2)
        {
            std::vector<float> intervals;
            for (size_t i = 1; i < result.positions.size(); ++i)
            {
                float intervalSec = (result.positions[i] - result.positions[i - 1])
                                  * (float) numSamples / (float) sampleRate;
                if (intervalSec > 0.1f && intervalSec < 4.0f)  // plausible range
                    intervals.push_back (intervalSec);
            }

            if (!intervals.empty())
            {
                // Median interval (more robust than mean)
                std::sort (intervals.begin(), intervals.end());
                float medianInterval = intervals[intervals.size() / 2];
                float rawBPM = 60.0f / medianInterval;

                // Octave-fold into 60–180 BPM range
                while (rawBPM < 60.0f)  rawBPM *= 2.0f;
                while (rawBPM > 180.0f) rawBPM *= 0.5f;

                result.estimatedBPM = std::clamp (rawBPM, 20.0f, 300.0f);
            }
        }

        result.count = (int) result.positions.size();
        return result;
    }

    /**
     * Find the nearest onset position to a given normalised position.
     * Returns the snapped position, or the original if no onsets within tolerance.
     *
     * tolerance — max distance (normalised) to snap, default 0.02 (2% of file length)
     */
    static float snapToNearestOnset (float position,
                                     const std::vector<float>& onsets,
                                     float tolerance = 0.02f)
    {
        float bestDist = tolerance;
        float bestPos  = position;
        for (float onset : onsets)
        {
            float d = std::abs (onset - position);
            if (d < bestDist)
            {
                bestDist = d;
                bestPos  = onset;
            }
        }
        return bestPos;
    }

private:
    static constexpr int kWindowSize = 512;   // samples per energy window
    static constexpr int kHopSize    = 256;   // samples between windows
};
