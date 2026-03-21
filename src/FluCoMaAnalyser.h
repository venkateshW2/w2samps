#pragma once
/**
 * FluCoMaAnalyser — offline audio analysis using flucoma-core algorithms.
 *
 * Implementation lives in FluCoMaAnalyser.cpp (keeps heavy FluCoMa includes
 * in a single translation unit, avoiding duplicate symbol link errors).
 *
 * Call analyse() on the MESSAGE THREAD only. Never call from processBlock.
 */

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <vector>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
struct FluCoMaResult
{
    // Onsets
    std::vector<float> onsetPositions;   // normalised 0–1
    float              estimatedBpm = 0.f;

    // MFCC (13 coeff, mean + std)
    std::array<float, 13> mfccMean  = {};
    std::array<float, 13> mfccStd   = {};

    // Chroma (12 bins, mean)
    std::array<float, 12> chromaMean = {};

    // SpectralShape (7 descriptors, mean)
    // [0]=centroid [1]=spread [2]=skewness [3]=kurtosis [4]=rolloff [5]=flatness [6]=crest
    std::array<float, 7>  shapeMean  = {};

    // Pitch
    float pitchHz         = 0.f;
    float pitchConfidence = 0.f;

    // Key (0–11 = major C..B, 12–23 = minor C..B)
    int          key          = 0;
    float        keyConfidence = 0.f;
    juce::String keyName;    // e.g. "C major", "F# minor"

    // Flat descriptor vector (50 floats) for KMeans / UMAP
    // mfccMean[13] + mfccStd[13] + chromaMean[12] + shapeMean[7] +
    // pitchHz(norm) + pitchConf + bpm(norm) + key(norm) + keyConf = 50
    std::array<float, 50> descriptor = {};
};

// ─────────────────────────────────────────────────────────────────────────────
class FluCoMaAnalyser
{
public:
    // Analysis parameters (adjustable before calling analyse())
    int    windowSize      = 1024;
    int    fftSize         = 1024;   // must equal windowSize for OnsetSegmentation compatibility
    int    hopSize         = 512;
    int    nMelBands       = 40;
    int    nMfccCoeff      = 13;
    int    nChromaBins     = 12;
    int    onsetFunction   = 9;    // complex domain — best general-purpose
    int    onsetFilterSize = 5;
    double onsetThreshold  = 0.35;
    int    onsetDebounce   = 2;
    double minPitchHz      = 50.0;
    double maxPitchHz      = 4000.0;

    /** Analyse a buffer (any channel count, mixed to mono internally).
     *  All heavy work here — call on message thread only. */
    FluCoMaResult analyse (const juce::AudioBuffer<float>& buffer,
                           double sampleRate);
};
