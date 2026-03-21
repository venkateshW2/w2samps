/**
 * FluCoMaAnalyser implementation — all heavy FluCoMa includes live here only.
 * This avoids duplicate symbol link errors that arise when flucoma headers
 * (which define non-inline free functions) are included from multiple TUs.
 */

#include "FluCoMaAnalyser.h"

// ── flucoma-core ──────────────────────────────────────────────────────────────
#include <flucoma/algorithms/public/STFT.hpp>
#include <flucoma/algorithms/public/OnsetSegmentation.hpp>
#include <flucoma/algorithms/public/MelBands.hpp>
#include <flucoma/algorithms/public/DCT.hpp>
#include <flucoma/algorithms/public/ChromaFilterBank.hpp>
#include <flucoma/algorithms/public/SpectralShape.hpp>
#include <flucoma/algorithms/public/YINFFT.hpp>
#include <flucoma/data/TensorTypes.hpp>
#include <flucoma/data/FluidMemory.hpp>

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>

// ─────────────────────────────────────────────────────────────────────────────
// Key detection (Krumhansl-Kessler profiles)
// ─────────────────────────────────────────────────────────────────────────────
static const float kMajorProfile[12] = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };

static const float kMinorProfile[12] = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

static float chromaCorr (const float* chroma, const float* profile, int shift)
{
    float cMean = 0.f, pMean = 0.f;
    for (int i = 0; i < 12; ++i) { cMean += chroma[i]; pMean += profile[i]; }
    cMean /= 12.f; pMean /= 12.f;
    float num = 0.f, dc2 = 0.f, dp2 = 0.f;
    for (int i = 0; i < 12; ++i)
    {
        float c = chroma[i] - cMean;
        float p = profile[(i - shift + 120) % 12] - pMean;
        num += c * p; dc2 += c * c; dp2 += p * p;
    }
    float denom = std::sqrt (dc2 * dp2);
    return denom > 1e-6f ? num / denom : 0.f;
}

static void estimateKey (FluCoMaResult& r)
{
    static const char* noteNames[] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

    float bestCorr  = -2.f;
    int   bestKey   = 0;
    bool  bestMajor = true;
    const float* ch = r.chromaMean.data();

    for (int s = 0; s < 12; ++s)
    {
        float majC = chromaCorr (ch, kMajorProfile, s);
        float minC = chromaCorr (ch, kMinorProfile, s);
        if (majC > bestCorr) { bestCorr = majC; bestKey = s;      bestMajor = true;  }
        if (minC > bestCorr) { bestCorr = minC; bestKey = s + 12; bestMajor = false; }
    }
    r.key           = bestKey;
    r.keyConfidence = (bestCorr + 1.0f) * 0.5f;
    int noteIdx     = bestKey % 12;
    r.keyName       = juce::String (noteNames[noteIdx])
                    + " " + (bestMajor ? "major" : "minor");
}

static void estimateBpm (FluCoMaResult& r, double sampleRate, int srcLen)
{
    if (r.onsetPositions.size() < 2) { r.estimatedBpm = 0.f; return; }
    std::vector<float> iois;
    float durSec = (float)srcLen / (float)sampleRate;
    for (int i = 1; i < (int)r.onsetPositions.size(); ++i)
    {
        float s = (r.onsetPositions[(size_t)i] - r.onsetPositions[(size_t)i-1]) * durSec;
        if (s > 0.05f && s < 2.0f) iois.push_back (s);
    }
    if (iois.empty()) { r.estimatedBpm = 0.f; return; }
    std::sort (iois.begin(), iois.end());
    float bpm = 60.f / iois[iois.size() / 2];
    while (bpm <  60.f) bpm *= 2.f;
    while (bpm > 240.f) bpm /= 2.f;
    r.estimatedBpm = bpm;
}

static void buildDescriptor (FluCoMaResult& r)
{
    size_t i = 0;
    for (int c = 0; c < 13; ++c) r.descriptor[i++] = r.mfccMean[(size_t)c];
    for (int c = 0; c < 13; ++c) r.descriptor[i++] = r.mfccStd[(size_t)c];
    for (int c = 0; c < 12; ++c) r.descriptor[i++] = r.chromaMean[(size_t)c];
    for (int c = 0; c < 7;  ++c) r.descriptor[i++] = r.shapeMean[(size_t)c];
    r.descriptor[i++] = r.pitchHz / 4000.f;
    r.descriptor[i++] = r.pitchConfidence;
    r.descriptor[i++] = r.estimatedBpm / 240.f;
    r.descriptor[i++] = (float)r.key / 23.f;
    r.descriptor[i++] = r.keyConfidence;
    // i == 50
}

// ─────────────────────────────────────────────────────────────────────────────
FluCoMaResult FluCoMaAnalyser::analyse (const juce::AudioBuffer<float>& buffer,
                                        double sampleRate)
{
    FluCoMaResult result;
    if (buffer.getNumSamples() < windowSize) return result;

    const int srcLen = buffer.getNumSamples();
    const int nBins  = fftSize / 2 + 1;

    // Mix to mono
    std::vector<double> mono ((size_t)srcLen, 0.0);
    int nCh = buffer.getNumChannels();
    for (int ch = 0; ch < nCh; ++ch)
    {
        const float* ptr = buffer.getReadPointer (ch);
        for (int s = 0; s < srcLen; ++s) mono[(size_t)s] += ptr[s];
    }
    if (nCh > 1) for (auto& x : mono) x /= nCh;

    using fidx = fluid::index;
    using namespace fluid::algorithm;

    // Algorithm objects
    STFT stft { (fidx)windowSize, (fidx)fftSize, (fidx)hopSize };

    OnsetSegmentation onset { (fidx)windowSize, (fidx)onsetFilterSize };
    onset.init ((fidx)windowSize, (fidx)fftSize, (fidx)onsetFilterSize);

    MelBands melBands { (fidx)nMelBands, (fidx)fftSize };
    melBands.init (20.0, sampleRate / 2.0,
                   (fidx)nMelBands, (fidx)nBins, sampleRate, (fidx)windowSize);

    DCT dct { (fidx)nMelBands, (fidx)nMfccCoeff };
    dct.init ((fidx)nMelBands, (fidx)nMfccCoeff);

    ChromaFilterBank chroma { (fidx)nChromaBins, (fidx)fftSize,
                               fluid::FluidDefaultAllocator() };
    chroma.init ((fidx)nChromaBins, (fidx)nBins, 440.0, sampleRate,
                 fluid::FluidDefaultAllocator());

    SpectralShape shape { fluid::FluidDefaultAllocator() };

    YINFFT yin { (fidx)nBins };

    // Accumulators
    std::vector<std::array<float,13>> mfccFrames;
    std::vector<std::array<float,12>> chromaFrames;
    std::vector<std::array<float,7>>  shapeFrames;
    std::vector<float>                pitchHzFrames, pitchConfFrames;

    fluid::FluidTensor<double, 1> window    (windowSize);
    fluid::FluidTensor<double, 1> mag       (nBins);
    fluid::FluidTensor<std::complex<double>, 1> frame (nBins);
    fluid::FluidTensor<double, 1> mels      (nMelBands);
    fluid::FluidTensor<double, 1> mfccs     (nMfccCoeff);
    fluid::FluidTensor<double, 1> chromaOut (nChromaBins);
    fluid::FluidTensor<double, 1> shapeOut  (7);
    fluid::FluidTensor<double, 1> pitchOut  (2);

    int totalFrames = (srcLen - windowSize) / hopSize + 1;
    mfccFrames.reserve   ((size_t)totalFrames);
    chromaFrames.reserve ((size_t)totalFrames);
    shapeFrames.reserve  ((size_t)totalFrames);
    pitchHzFrames.reserve   ((size_t)totalFrames);
    pitchConfFrames.reserve ((size_t)totalFrames);

    for (int fr = 0; fr < totalFrames; ++fr)
    {
        int startSample = fr * hopSize;
        for (int s = 0; s < windowSize; ++s)
            window ((fidx)s) = mono[(size_t)(startSample + s)];

        stft.processFrame (window, frame);
        STFT::magnitude (frame, mag);

        // Onset
        double det = onset.processFrame (
            window, (fidx)onsetFunction, (fidx)onsetFilterSize,
            onsetThreshold, (fidx)onsetDebounce, (fidx)1);
        if (det > 0.0 && fr > 0)
            result.onsetPositions.push_back ((float)startSample / (float)(srcLen - 1));

        // MFCC
        melBands.processFrame (mag, mels, false, false, true, fluid::FluidDefaultAllocator());
        dct.processFrame (mels, mfccs);
        {
            std::array<float,13> row = {};
            for (int c = 0; c < nMfccCoeff; ++c) row[(size_t)c] = (float)mfccs ((fidx)c);
            mfccFrames.push_back (row);
        }

        // Chroma
        chroma.processFrame (mag, chromaOut, 0.0, -1.0, 1);
        {
            std::array<float,12> row = {};
            for (int c = 0; c < nChromaBins; ++c) row[(size_t)c] = (float)chromaOut ((fidx)c);
            chromaFrames.push_back (row);
        }

        // SpectralShape
        shape.processFrame (mag, shapeOut, sampleRate, 0.0, -1.0,
                            0.95, false, false, fluid::FluidDefaultAllocator());
        {
            std::array<float,7> row = {};
            for (int c = 0; c < 7; ++c) row[(size_t)c] = (float)shapeOut ((fidx)c);
            shapeFrames.push_back (row);
        }

        // Pitch
        yin.processFrame (mag, pitchOut, minPitchHz, maxPitchHz, sampleRate);
        pitchHzFrames.push_back   ((float)pitchOut ((fidx)0));
        pitchConfFrames.push_back ((float)pitchOut ((fidx)1));
    }

    // Aggregate MFCC
    int nFr = (int)mfccFrames.size();
    if (nFr > 0)
    {
        for (int c = 0; c < nMfccCoeff; ++c)
        {
            float sum = 0.f;
            for (const auto& row : mfccFrames) sum += row[(size_t)c];
            float mean = sum / nFr;
            result.mfccMean[(size_t)c] = mean;
            float var = 0.f;
            for (const auto& row : mfccFrames) { float d = row[(size_t)c] - mean; var += d*d; }
            result.mfccStd[(size_t)c] = std::sqrt (var / nFr);
        }
    }

    // Aggregate Chroma
    if (!chromaFrames.empty())
        for (int c = 0; c < nChromaBins; ++c)
        {
            float sum = 0.f;
            for (const auto& row : chromaFrames) sum += row[(size_t)c];
            result.chromaMean[(size_t)c] = sum / (int)chromaFrames.size();
        }

    // Aggregate SpectralShape
    if (!shapeFrames.empty())
        for (int c = 0; c < 7; ++c)
        {
            float sum = 0.f;
            for (const auto& row : shapeFrames) sum += row[(size_t)c];
            result.shapeMean[(size_t)c] = sum / (int)shapeFrames.size();
        }

    // Aggregate Pitch (confidence-weighted)
    {
        float wSum = 0.f, phSum = 0.f;
        for (int i = 0; i < (int)pitchHzFrames.size(); ++i)
        {
            float conf = pitchConfFrames[(size_t)i];
            if (conf > 0.5f) { phSum += pitchHzFrames[(size_t)i] * conf; wSum += conf; }
        }
        result.pitchHz         = wSum > 0.f ? phSum / wSum : 0.f;
        result.pitchConfidence = wSum > 0.f ? wSum / (float)pitchHzFrames.size() : 0.f;
    }

    estimateKey (result);
    estimateBpm (result, sampleRate, srcLen);
    buildDescriptor (result);

    return result;
}
