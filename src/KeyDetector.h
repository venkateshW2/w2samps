#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <algorithm>

/**
 * KeyDetector — offline key detection via chromagram + Krumhansl-Kessler profiles.
 *
 * Algorithm:
 *   1. Mix buffer to mono, sub-sample to ~11025 Hz for efficiency
 *   2. Goertzel algorithm at each of 12 pitch classes × 5 octaves
 *   3. Build normalised chromagram (12-element vector)
 *   4. Pearson correlation against major and minor KK profiles for all 12 roots
 *   5. Return best-matching key and confidence
 *
 * Runs offline on the message thread — never call from the audio thread.
 */
class KeyDetector
{
public:
    struct Result
    {
        int          keyIndex   = -1;    // 0-11 = C..B major, 12-23 = C..B minor; -1 = unknown
        juce::String keyName;            // e.g. "C maj" or "A min"
        float        confidence = 0.0f;  // Pearson r, 0-1
    };

    static Result analyse (const juce::AudioBuffer<float>& buf, double sampleRate)
    {
        Result result;
        int numSamples  = buf.getNumSamples();
        int numChannels = buf.getNumChannels();
        if (numSamples < 4096 || numChannels == 0) return result;

        // Sub-sample to ~11025 Hz (every 4th sample at 44100)
        int step = std::max (1, (int)(sampleRate / 11025.0 + 0.5));
        float effSR = (float)(sampleRate / step);

        // Mix to mono with sub-sampling (limit analysis to first 8 seconds for speed)
        int maxSamples = std::min (numSamples, (int)(sampleRate * 8.0));
        std::vector<float> mono;
        mono.reserve ((size_t)(maxSamples / step + 1));
        for (int i = 0; i < maxSamples; i += step)
        {
            float v = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                v += buf.getSample (c, i);
            mono.push_back (v / (float) numChannels);
        }

        int n = (int) mono.size();
        if (n < 64) return result;

        // ── Chromagram via Goertzel at 5 octaves × 12 pitch classes ──────────
        // Reference: C2 = 65.41 Hz, C3=130.81, C4=261.63, C5=523.25, C6=1046.5
        static const float kC2 = 65.4064f;
        std::array<float, 12> chroma {};

        for (int pc = 0; pc < 12; ++pc)
        {
            float energy = 0.0f;
            for (int oct = 0; oct < 6; ++oct)   // C2..C7
            {
                float freq = kC2 * std::pow (2.0f, (float) oct + (float) pc / 12.0f);
                if (freq > effSR * 0.45f) break;  // below Nyquist
                energy += goertzel (mono.data(), n, freq, effSR);
            }
            chroma[(size_t) pc] = energy;
        }

        // Normalize
        float maxE = *std::max_element (chroma.begin(), chroma.end());
        if (maxE < 1e-10f) return result;
        for (auto& e : chroma) e /= maxE;

        // ── Krumhansl-Kessler profiles ────────────────────────────────────────
        static const float kMajor[12] = {6.35f,2.23f,3.48f,2.33f,4.38f,4.09f,
                                          2.52f,5.19f,2.39f,3.66f,2.29f,2.88f};
        static const float kMinor[12] = {6.33f,2.68f,3.52f,5.38f,2.60f,3.53f,
                                          2.54f,4.75f,3.98f,2.69f,3.34f,3.17f};

        // ── Correlate against all 24 keys ─────────────────────────────────────
        int   bestKey  = 0;
        float bestCorr = -2.0f;

        for (int root = 0; root < 12; ++root)
        {
            float majC = pearson (chroma, kMajor, root);
            float minC = pearson (chroma, kMinor, root);
            if (majC > bestCorr) { bestCorr = majC; bestKey = root; }
            if (minC > bestCorr) { bestCorr = minC; bestKey = root + 12; }
        }

        static const char* kNoteNames[12] = {"C","C#","D","D#","E","F",
                                              "F#","G","G#","A","A#","B"};
        int  root    = bestKey % 12;
        bool isMajor = (bestKey < 12);

        result.keyIndex   = bestKey;
        result.keyName    = juce::String (kNoteNames[root])
                          + (isMajor ? " maj" : " min");
        result.confidence = juce::jlimit (0.0f, 1.0f, bestCorr);
        return result;
    }

private:
    static float goertzel (const float* x, int n, float freq, float sr)
    {
        float omega = 2.0f * 3.14159265f * freq / sr;
        float coeff = 2.0f * std::cos (omega);
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            s0 = x[i] + coeff * s1 - s2;
            s2 = s1;  s1 = s0;
        }
        return s1 * s1 + s2 * s2 - coeff * s1 * s2;
    }

    static float pearson (const std::array<float, 12>& x,
                          const float* y, int shift)
    {
        float mx = 0.0f, my = 0.0f;
        for (int i = 0; i < 12; ++i) { mx += x[(size_t)i]; my += y[i]; }
        mx /= 12.0f;  my /= 12.0f;
        float num = 0.0f, dx2 = 0.0f, dy2 = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            float xi = x[(size_t)((i + shift) % 12)] - mx;
            float yi = y[i] - my;
            num  += xi * yi;
            dx2  += xi * xi;
            dy2  += yi * yi;
        }
        float denom = std::sqrt (dx2 * dy2);
        return denom > 0.0f ? num / denom : 0.0f;
    }
};
