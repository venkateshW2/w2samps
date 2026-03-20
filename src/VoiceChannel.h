#pragma once
#include "SampleLibrary.h"
#include "GranularVoice.h"
#include "EuclideanSequencer.h"
#include "PhaseTransform.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

/**
 * VoiceChannel — one complete voice lane: sample library + granular voice + sequencer.
 *
 * Signal flow per block:
 *   inputPhase  (master clock, or another voice's transformed phase)
 *        ↓
 *   PhaseTransform  (rate × offset × warp × reverse × quantise)
 *        ↓
 *   transformedPhase  [0,1)
 *        ↓
 *   Step crossing detection  →  EuclideanSequencer.getStepValue()
 *        ↓ (hit?)
 *   Sample advance (Hold / Sequential / Random)
 *   FX randomize request (if rndFxChance triggers — sets atomic flag)
 *        ↓
 *   GranularVoice.trigger()  →  GranularVoice.renderBlock()  →  stereo output
 *
 * Rate vs. Steps:
 *   rate           — how fast the voice phasor moves (×1 = one cycle per master beat,
 *                    ×2 = two cycles per beat / double-time, ÷2 = half-time)
 *   seqSteps       — how many euclidean slots per phasor cycle (more steps = faster grid)
 *   Together they give full polyrhythm control without a confusing N:D fraction UI.
 *
 * FX randomize on hit:
 *   When rndFxChance > 0 and an euclidean hit fires, the audio thread rolls the RNG.
 *   If it passes, wantsRandomizeFX_ is set. The message thread (editor timer) reads
 *   this with takeRandomizeFXRequest() and calls proc.randomizeVoiceParams(v).
 *   This keeps AudioParameter writes on the message thread where they belong.
 */
class VoiceChannel
{
public:
    //==========================================================================
    enum class PhaseSource   { Master = 0, LockV0 = 1, LockV1 = 2, LockV2 = 3 };
    enum class SampleAdvMode { Hold = 0, Sequential = 1, Random = 2 };

    struct Params
    {
        // ── Phase source ──────────────────────────────────────────────────────
        PhaseSource phaseSource = PhaseSource::Master;

        // ── Rate (replaces N/D fraction) ──────────────────────────────────────
        // Direct rateMultiplier for PhaseTransform.
        //   1.0  = one voice cycle per master beat
        //   2.0  = double-time (two cycles per beat)
        //   0.5  = half-time  (one cycle per two beats)
        // Use kRatePresets[] for the curated preset values (÷8…×8).
        float rate = 1.0f;

        // ── Phase transform ───────────────────────────────────────────────────
        float phaseOffset    = 0.0f;  // 0→1 : time-shift the pattern start
        float warp           = 0.0f;  // -1→+1 : curve the phasor (0=linear)
        bool  reverse        = false;
        float quantiseAmount = 0.0f;  // 0=smooth phasor, 1=hard staircase

        // ── Euclidean sequencer ───────────────────────────────────────────────
        int seqSteps    = 16;
        int seqHits     = 4;
        int seqRotation = 0;

        // ── Sample advance per hit ────────────────────────────────────────────
        SampleAdvMode sampleAdvance = SampleAdvMode::Hold;

        // ── FX randomize on hit ───────────────────────────────────────────────
        // Probability [0,1] that a hit triggers an FX param randomization.
        // 0 = never, 1 = every hit.
        float rndFxChance = 0.0f;

        // ── GranularVoice DSP chain ───────────────────────────────────────────
        GranularVoice::Params granular;
    };

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_       = sampleRate;
        lastInputPhase_   = 0.0;
        voicePhaseAccum_  = 0.0;
        lastTransformedPhase_ = 0.0;
        voice_.prepare (sampleRate, maxBlockSize);
    }

    void loadFolder (const juce::File& folder, juce::AudioFormatManager& fmgr)
    {
        library_.loadFolder (folder, fmgr);
        loadCurrentSampleIntoVoice();
    }

    //==========================================================================
    // Audio thread

    /**
     * Process one block.
     *
     * inputPhase     — phasor [0,1) for this voice (master clock or locked voice)
     * params         — all voice parameters
     * output         — stereo buffer (voice ADDs to it)
     * startSample    — block offset in output
     * numSamples     — block length
     */
    void processBlock (double inputPhase,
                       const Params& params,
                       juce::AudioBuffer<float>& output,
                       int startSample,
                       int numSamples)
    {
        // ── Accumulate voice phase via delta × rate ───────────────────────────
        // Rate must be applied to the INCREMENT, not the instantaneous master
        // phase value.  Multiplying master phase directly caps the range to
        // [0, rate) when rate < 1, so e.g. /2 only ever crosses steps 0–7.
        // By accumulating delta × rate we always sweep the full 0→1 range —
        // /2 just takes twice as many master cycles to get there.
        double inputDelta = inputPhase - lastInputPhase_;
        if (inputDelta < 0.0) inputDelta += 1.0;   // handle master clock wrap
        lastInputPhase_ = inputPhase;

        voicePhaseAccum_ += inputDelta * (double) params.rate;
        while (voicePhaseAccum_ >= 1.0) voicePhaseAccum_ -= 1.0;

        // ── Apply remaining phase transforms (offset / warp / reverse / quant)
        // Rate is already baked in above — pass rateMultiplier = 1.
        PhaseTransform pt;
        pt.rateMultiplier = 1.0f;
        pt.phaseOffset    = params.phaseOffset;
        pt.warp           = params.warp;
        pt.reverse        = params.reverse;
        pt.quantiseAmount = params.quantiseAmount;
        pt.stepsForQuant  = params.seqSteps;

        double newPhase = pt.apply (voicePhaseAccum_);

        // ── Rebuild euclidean pattern only when changed ───────────────────────
        if (params.seqSteps    != lastSeqSteps_    ||
            params.seqHits     != lastSeqHits_     ||
            params.seqRotation != lastSeqRotation_)
        {
            sequencer_.set (params.seqSteps, params.seqHits, params.seqRotation);
            lastSeqSteps_    = params.seqSteps;
            lastSeqHits_     = params.seqHits;
            lastSeqRotation_ = params.seqRotation;
        }

        // ── Build mutable granular params (loop anchor may be overridden) ────────
        GranularVoice::Params gran = params.granular;

        // ── Find step crossings in [lastPhase, newPhase) ──────────────────────
        int firedSteps[32];
        int numFired = PhaseTransform::findStepCrossings (
            lastTransformedPhase_, newPhase,
            params.seqSteps, 1.0,
            firedSteps, 32);

        // ── Trigger on euclidean hits ─────────────────────────────────────────
        for (int i = 0; i < numFired; ++i)
        {
            if (sequencer_.getStepValue (firedSteps[i]))
            {
                // ── Loop window movement on each trigger ──────────────────────
                // Sequential/Random modes: advance the loop anchor.
                // Fixed/Off: always use the waveform-set loopStart value.
                {
                    auto loopMode = gran.loopMode;
                    int  bufLen   = library_.current()
                                      ? library_.current()->buffer.getNumSamples() : 0;

                    if (bufLen > 0 && sampleRate_ > 0.0 &&
                        (loopMode == GranularVoice::LoopMode::OnsetSeq ||
                         loopMode == GranularVoice::LoopMode::OnsetRnd))
                    {
                        float loopFrac = (float)(gran.loopSizeMs / 1000.0 * sampleRate_ / bufLen);
                        loopFrac = juce::jlimit (0.001f, 1.0f, loopFrac);
                        float rgnSt = gran.regionStart;
                        float rgnEn = gran.regionEnd;

                        auto* entry = library_.current();
                        bool  found = false;

                        if (entry && entry->onsetsAnalysed && entry->onsets.count > 0)
                        {
                            const auto& ov = entry->onsets.positions;
                            int nOnsets = (int) ov.size();

                            if (loopMode == GranularVoice::LoopMode::OnsetSeq)
                            {
                                ++onsetIdx_;
                                for (int tries = 0; tries < nOnsets; ++tries)
                                {
                                    int idx = ((onsetIdx_ + tries) % nOnsets + nOnsets) % nOnsets;
                                    float pos = ov[(size_t) idx];
                                    if (pos >= rgnSt && pos + loopFrac <= rgnEn)
                                    {
                                        seqLoopAnchorNorm_ = pos;
                                        onsetIdx_ = idx;
                                        found = true;
                                        break;
                                    }
                                }
                            }
                            else // OnsetRnd
                            {
                                audioRng_ = audioRng_ * 1664525u + 1013904223u;
                                int startIdx = (int)((audioRng_ >> 16) % (uint32_t) nOnsets);
                                for (int tries = 0; tries < nOnsets; ++tries)
                                {
                                    int idx = (startIdx + tries) % nOnsets;
                                    float pos = ov[(size_t) idx];
                                    if (pos >= rgnSt && pos + loopFrac <= rgnEn)
                                    {
                                        seqLoopAnchorNorm_ = pos;
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (!found)
                        {
                            // No valid onset — fall back to random position in region
                            audioRng_ = audioRng_ * 1664525u + 1013904223u;
                            float frac = (float)(audioRng_ >> 16) / 65535.0f;
                            float maxAnch = rgnEn - loopFrac;
                            if (maxAnch < rgnSt) maxAnch = rgnSt;
                            seqLoopAnchorNorm_ = rgnSt + frac * (maxAnch - rgnSt);
                        }
                        seqLoopEndNorm_ = seqLoopAnchorNorm_ + loopFrac;
                    }
                    else if (bufLen > 0 && sampleRate_ > 0.0 &&
                        (loopMode == GranularVoice::LoopMode::Sequential ||
                         loopMode == GranularVoice::LoopMode::Random))
                    {
                        // In Rnd/Seq, always use loopSizeMs for grain size.
                        // The handles define the playback region boundary, not the grain size.
                        float loopFrac = (float)(gran.loopSizeMs / 1000.0 * sampleRate_ / bufLen);
                        loopFrac = juce::jlimit (0.001f, 1.0f, loopFrac);

                        float rgnSt    = gran.regionStart;
                        float rgnEn    = gran.regionEnd;
                        float maxAnch  = rgnEn - loopFrac;
                        if (maxAnch < rgnSt) maxAnch = rgnSt;

                        if (loopMode == GranularVoice::LoopMode::Sequential)
                        {
                            seqLoopAnchorNorm_ += loopFrac;
                            if (seqLoopAnchorNorm_ > maxAnch)
                                seqLoopAnchorNorm_ = rgnSt;
                        }
                        else // Random
                        {
                            audioRng_ = audioRng_ * 1664525u + 1013904223u;
                            float frac = (float)(audioRng_ >> 16) / 65535.0f;
                            seqLoopAnchorNorm_ = rgnSt + frac * (maxAnch - rgnSt);
                        }
                        seqLoopEndNorm_ = seqLoopAnchorNorm_ + loopFrac;
                    }
                    else
                    {
                        // Fixed/Off: anchor and end follow the waveform handles
                        seqLoopAnchorNorm_ = gran.loopStart;
                        seqLoopEndNorm_    = gran.loopEnd;
                    }
                    gran.loopStart = seqLoopAnchorNorm_;
                    gran.loopEnd   = seqLoopEndNorm_;
                }

                // ── Sample file advance ───────────────────────────────────────
                switch (params.sampleAdvance)
                {
                    case SampleAdvMode::Sequential:
                        library_.advanceNext();
                        swapCurrentSampleIntoVoice();
                        break;
                    case SampleAdvMode::Random:
                        library_.advanceRandom (audioRng_);
                        swapCurrentSampleIntoVoice();
                        break;
                    case SampleAdvMode::Hold:
                    default: break;
                }

                // ── FX randomize? ─────────────────────────────────────────────
                if (params.rndFxChance > 0.001f)
                {
                    audioRng_ = audioRng_ * 1664525u + 1013904223u;
                    float roll = (float)(audioRng_ >> 16) / 65535.0f;
                    if (roll < params.rndFxChance)
                        wantsRandomizeFX_.store (true, std::memory_order_relaxed);
                }

                voice_.trigger();
            }
        }

        // ── Update UI-readable state ──────────────────────────────────────────
        transformedPhase_.store (newPhase);
        if (params.seqSteps > 0)
            currentStep_.store ((int)(newPhase * params.seqSteps) % params.seqSteps);
        lastTransformedPhase_ = newPhase;

        // ── Keep gran.loopStart/End in sync for rendering ────────────────────────
        gran.loopStart = seqLoopAnchorNorm_;
        gran.loopEnd   = seqLoopEndNorm_;
        // Expose for waveform cursor
        seqLoopAnchorDisplay_.store (seqLoopAnchorNorm_, std::memory_order_relaxed);

        // ── Render (skip if muted) ────────────────────────────────────────────
        if (!muted_.load (std::memory_order_relaxed))
            voice_.renderBlock (output, startSample, numSamples,
                                sampleRate_, gran, audioRng_);
    }

    //==========================================================================
    // Accessors (readable from UI thread after prepare())

    double getTransformedPhase()   const { return transformedPhase_.load(); }
    int    getCurrentStep()        const { return currentStep_.load(); }
    float  getPlayPositionNorm()   const { return voice_.getPlayPositionNorm(); }
    float  getSeqLoopAnchorNorm()  const { return seqLoopAnchorDisplay_.load (std::memory_order_relaxed); }
    const EuclideanSequencer& getSequencer() const { return sequencer_; }
    const SampleLibrary&      getLibrary()   const { return library_; }
    SampleLibrary&            getLibrary()         { return library_; }

    void setMuted (bool m) { muted_.store (m, std::memory_order_relaxed); }
    bool getMuted()  const { return muted_.load (std::memory_order_relaxed); }

    /** Returns true once per hit-triggered randomize request, then resets. */
    bool takeRandomizeFXRequest()
    {
        return wantsRandomizeFX_.exchange (false);
    }

    //==========================================================================
    // Message-thread navigation

    void prevSample()   { library_.prev();        loadCurrentSampleIntoVoice(); }
    void nextSample()   { library_.next();         loadCurrentSampleIntoVoice(); }
    void randomSample() { library_.pickRandom();   loadCurrentSampleIntoVoice(); }

private:
    void loadCurrentSampleIntoVoice()
    {
        auto* e = library_.current();
        if (e)
        {
            voice_.loadBuffer (&e->buffer, e->sampleRate);
            seqLoopAnchorNorm_ = 0.0f;
            seqLoopEndNorm_    = 0.25f;
            onsetIdx_          = -1;
        }
    }

    void swapCurrentSampleIntoVoice()
    {
        auto* e = library_.current();
        if (e)
        {
            voice_.swapBuffer (&e->buffer, e->sampleRate);
            seqLoopAnchorNorm_ = 0.0f;
            seqLoopEndNorm_    = 1.0f;
            onsetIdx_          = -1;
        }
    }

    SampleLibrary      library_;
    GranularVoice      voice_;
    EuclideanSequencer sequencer_;

    double   sampleRate_           = 44100.0;
    double   lastInputPhase_       = 0.0;   // for computing master phase delta
    double   voicePhaseAccum_      = 0.0;   // accumulated voice phase (rate × delta)
    double   lastTransformedPhase_ = 0.0;
    int      lastSeqSteps_         = -1;
    int      lastSeqHits_          = -1;
    int      lastSeqRotation_      = -1;
    uint32_t audioRng_             = 0xDEADBEEFu;

    // Loop position sequencer state (audio thread)
    float seqLoopAnchorNorm_ = 0.0f;
    float seqLoopEndNorm_    = 1.0f;  // loopEnd kept in sync with anchor
    int   onsetIdx_          = -1;    // current onset index for OnsetSeq mode

    // UI-readable (written by audio thread, read by UI timer)
    std::atomic<double> transformedPhase_     { 0.0 };
    std::atomic<int>    currentStep_          { 0 };
    std::atomic<bool>   muted_                { false };
    std::atomic<float>  seqLoopAnchorDisplay_ { 0.0f };

    // Hit-triggered FX randomize request (audio → message thread handoff)
    std::atomic<bool>   wantsRandomizeFX_ { false };
};
