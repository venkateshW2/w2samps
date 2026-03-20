#pragma once
#include "SampleLibrary.h"
#include "GranularVoice.h"
#include "EuclideanSequencer.h"
#include "PhaseTransform.h"
#include "FuncGen.h"
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

        // ── Function generator modulation ────────────────────────────────────
        // 4 FuncGens per voice.
        // fgSync: true = rate is a multiplier vs master phasor delta
        //         false = rate is Hz (free-running, BPM-independent)
        // fgRateVal: 0.001–32.0 (multiplier when sync, Hz when free)
        static constexpr int kNumFuncGens = 4;
        float fgRateVal[kNumFuncGens] = { 1.f, 1.f, 1.f, 1.f };   // sync mult or free Hz
        bool  fgSync[kNumFuncGens]    = { true, true, true, true }; // sync to phasor or free Hz
        int   fgDest[kNumFuncGens]    = { 0, 0, 0, 0 };            // ModDest index
        float fgDepth[kNumFuncGens]   = { 0.f, 0.f, 0.f, 0.f };   // -1 → +1
        float fgMin[kNumFuncGens]     = { 0.f, 0.f, 0.f, 0.f };   // normalised range lo
        float fgMax[kNumFuncGens]     = { 1.f, 1.f, 1.f, 1.f };   // normalised range hi
    };

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_       = sampleRate;
        lastInputPhase_   = 0.0;
        voicePhaseAccum_  = 0.0;
        lastTransformedPhase_ = 0.0;
        for (int i = 0; i < Params::kNumFuncGens; ++i)
        {
            fgPhaseAccum_[i] = 0.0;
            fgPhaseOut_[i].store (0.f, std::memory_order_relaxed);
        }
        for (int d = 0; d < kNumModDests; ++d)
            destModNorm_[d].store (-1.f, std::memory_order_relaxed);
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

        // ── Accumulate FuncGen phases (independent of voice rate) ────────────
        for (int i = 0; i < Params::kNumFuncGens; ++i)
        {
            double rate = (double) params.fgRateVal[i];
            if (params.fgSync[i])
                fgPhaseAccum_[i] += inputDelta * rate;          // sync: × master delta
            else
                fgPhaseAccum_[i] += rate * (double) numSamples / sampleRate_;  // free Hz
            while (fgPhaseAccum_[i] >= 1.0) fgPhaseAccum_[i] -= 1.0;
            fgPhaseOut_[i].store ((float) fgPhaseAccum_[i], std::memory_order_relaxed);
        }

        // ── Build mutable granular params (loop anchor may be overridden) ────────
        GranularVoice::Params gran = params.granular;

        // ── Apply FuncGen modulation to gran params ───────────────────────────
        // Clear mod output per dest first (last FG wins if multiple target same dest)
        for (int d = 0; d < (int) ModDest::kCount; ++d)
            destModNorm_[d].store (-1.f, std::memory_order_relaxed);  // -1 = inactive

        for (int i = 0; i < Params::kNumFuncGens; ++i)
        {
            auto dest = (ModDest) params.fgDest[i];
            if (dest == ModDest::None || params.fgDepth[i] == 0.0f) continue;

            float raw   = funcGens_[i].evaluate ((float) fgPhaseAccum_[i]);
            float norm  = params.fgMin[i] + raw * (params.fgMax[i] - params.fgMin[i]);
            float depth = params.fgDepth[i];
            applyModDest (gran, params.granular, dest, norm, depth);
            destModNorm_[(int) dest].store (norm, std::memory_order_relaxed);
        }

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

    // FuncGen access (editor draws + edits curves on message thread)
    FuncGen&       getFuncGen (int i)       { return funcGens_[(size_t) i]; }
    const FuncGen& getFuncGen (int i) const { return funcGens_[(size_t) i]; }

    /** Current FuncGen phase [0,1] — written by audio thread, safe to read from timer. */
    float getFgPhase (int i) const
    {
        return fgPhaseOut_[(size_t) i].load (std::memory_order_relaxed);
    }

    /** Current modulated norm [0,1] for a destination, or -1 if no FG targets it. */
    float getDestModNorm (ModDest d) const
    {
        return destModNorm_[(int) d].load (std::memory_order_relaxed);
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

    /** Apply one FuncGen modulation slot to gran params.
     *  norm   — normalised [0,1] curve output, already scaled by fgMin/fgMax.
     *  depth  — blend [-1, +1]: how much norm shifts the destination.
     *  Base   — the original (unmodulated) gran params for blending. */
    static void applyModDest (GranularVoice::Params& gran,
                               const GranularVoice::Params& base,
                               ModDest dest, float norm, float depth) noexcept
    {
        auto bl = [] (float a, float b, float t) { return a + (b - a) * t; };
        auto cl = [] (float v, float lo, float hi)
                  { return v < lo ? lo : (v > hi ? hi : v); };

        switch (dest)
        {
            case ModDest::Pitch:
            {
                float target = bl (-24.f, 24.f, norm);
                gran.pitchSemitones = cl (base.pitchSemitones + (target - base.pitchSemitones) * depth,
                                         -24.f, 24.f);
                break;
            }
            case ModDest::Attack:
            {
                float target = 0.001f + norm * 1.999f;
                gran.attackSec = cl (base.attackSec + (target - base.attackSec) * depth,
                                     0.001f, 2.f);
                break;
            }
            case ModDest::Decay:
            {
                float target = 0.001f + norm * 1.999f;
                gran.decaySec = cl (base.decaySec + (target - base.decaySec) * depth,
                                    0.001f, 2.f);
                break;
            }
            case ModDest::Sustain:
            {
                gran.sustain = cl (base.sustain + (norm - base.sustain) * depth, 0.f, 1.f);
                break;
            }
            case ModDest::Release:
            {
                float target = 0.001f + norm * 3.999f;
                gran.releaseSec = cl (base.releaseSec + (target - base.releaseSec) * depth,
                                      0.001f, 4.f);
                break;
            }
            case ModDest::FilterFreq:
            {
                // Log-scale for musically even sweeps
                float logLo = std::log (20.f), logHi = std::log (20000.f);
                float target = std::exp (logLo + norm * (logHi - logLo));
                gran.filterFreqHz = cl (base.filterFreqHz + (target - base.filterFreqHz) * depth,
                                        20.f, 20000.f);
                break;
            }
            case ModDest::FilterQ:
            {
                float target = 0.5f + norm * 9.5f;
                gran.filterRes = cl (base.filterRes + (target - base.filterRes) * depth,
                                     0.5f, 10.f);
                break;
            }
            case ModDest::Drive:
            {
                gran.distDrive = cl (base.distDrive + (norm - base.distDrive) * depth, 0.f, 1.f);
                break;
            }
            case ModDest::ReverbMix:
            {
                gran.reverbMix = cl (base.reverbMix + (norm - base.reverbMix) * depth, 0.f, 1.f);
                break;
            }
            case ModDest::ReverbSize:
            {
                gran.reverbSize = cl (base.reverbSize + (norm - base.reverbSize) * depth, 0.f, 1.f);
                break;
            }
            case ModDest::LoopSizeMs:
            {
                float target = 5.f + norm * 4995.f;
                gran.loopSizeMs = cl (base.loopSizeMs + (target - base.loopSizeMs) * depth,
                                      5.f, 5000.f);
                break;
            }
            default: break;
        }
    }

    SampleLibrary      library_;
    GranularVoice      voice_;
    EuclideanSequencer sequencer_;
    FuncGen            funcGens_[Params::kNumFuncGens];

    double   sampleRate_           = 44100.0;
    double   lastInputPhase_       = 0.0;
    double   voicePhaseAccum_      = 0.0;
    double   lastTransformedPhase_ = 0.0;
    double   fgPhaseAccum_[Params::kNumFuncGens] = {};

    // UI-readable FuncGen state (audio thread writes, timer reads)
    std::atomic<float> fgPhaseOut_[Params::kNumFuncGens];   // playhead phase per FG
    std::atomic<float> destModNorm_[kNumModDests];           // active mod norm per dest (-1=off)
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
