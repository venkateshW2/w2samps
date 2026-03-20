#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

/**
 * GranularVoice — region-based looping sample voice with full DSP chain.
 *
 * DSP chain (per renderBlock):
 *   Sample read (linear interp, pitch ratio, region clamp, loop logic)
 *     → ADSR envelope
 *     → Anti-click trigger fade  (5ms linear ramp on each trigger)
 *     → Pre-amp gain             (params.preGain — into drive)
 *     → tanh distortion          (normalised)
 *     → StateVariableTPT LP filter
 *     → Reverb
 *     → Per-voice Limiter        (params.limitThreshDb; 0 = bypassed)
 *     → Track level × velocity  (params.gain — post-FX output level)
 *
 * ALL loop/region values stored as normalised [0,1] fractions of the buffer.
 * loopSizeMs is in milliseconds; conversion happens at render time.
 *
 * Thread safety:
 *   prepare()     — before audio starts
 *   loadBuffer()  — message thread only
 *   swapBuffer()  — audio thread only (pointer swap, no alloc)
 *   trigger()     — audio thread
 *   renderBlock() — audio thread
 */
class GranularVoice
{
public:
    enum class LoopMode { Off = 0, Fixed = 1, Random = 2, Sequential = 3, OnsetSeq = 4, OnsetRnd = 5 };

    struct Params
    {
        // Pitch
        float pitchSemitones = 0.0f;

        // ADSR
        float attackSec  = 0.005f;
        float decaySec   = 0.1f;
        float sustain    = 0.8f;
        float releaseSec = 0.2f;

        // Filter
        float filterFreqHz = 20000.0f;
        float filterRes    = 0.707f;

        // Distortion + reverb
        float distDrive    = 0.0f;
        float reverbMix    = 0.0f;
        float reverbSize   = 0.5f;
        bool  reverbFreeze = false;

        // Gain structure
        float preGain       = 1.0f;   // pre-amp: applied BEFORE drive  (0.25–4.0)
        float gain          = 1.0f;   // track level: post-FX output    (0.0–2.0)
        float limitThreshDb = 0.0f;   // per-voice limiter threshold dB (0 = bypass)

        // Region (normalised 0–1)
        float regionStart = 0.0f;
        float regionEnd   = 1.0f;

        // Loop
        LoopMode loopMode    = LoopMode::Off;
        float    loopStart   = 0.0f;   // normalised; ignored if loopSizeLock=true
        float    loopEnd     = 1.0f;   // normalised; ignored if loopSizeLock=true
        float    loopSizeMs  = 100.0f; // used when loopSizeLock=true
        bool     loopSizeLock = false; // if true, loopEnd = loopStart + loopSizeMs
    };

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_ = sampleRate;
        tempBuf_.setSize (2, maxBlockSize, false, true, false);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) maxBlockSize;
        spec.numChannels      = 2;

        filter_.prepare (spec);
        filter_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        filter_.setCutoffFrequency (20000.0f);
        filter_.setResonance (0.707f);

        adsr_.setSampleRate (sampleRate);
        adsr_.setParameters ({ 0.005f, 0.1f, 0.8f, 0.2f });

        reverb_.reset();
        reverb_.setSampleRate (sampleRate);
        juce::Reverb::Parameters rp;
        rp.wetLevel = 0.0f; rp.dryLevel = 1.0f;
        rp.roomSize = 0.5f; rp.damping  = 0.5f;
        rp.width = 1.0f;    rp.freezeMode = 0.0f;
        reverb_.setParameters (rp);

        limiter_.prepare (spec);
        limiter_.setThreshold (-1.0f);
        limiter_.setRelease   (100.0f);

        position_             = 0.0;
        playing_              = false;
        exhausted_            = false;
        triggerFadeRemaining_ = 0;
    }

    /** Message thread: set or replace the source buffer. Full state reset. */
    void loadBuffer (const juce::AudioBuffer<float>* buf, double sourceSampleRate)
    {
        buffer_     = buf;
        sourceRate_ = sourceSampleRate;
        playing_    = false;
        exhausted_  = false;
        position_   = 0.0;
        loopAnchorSamples_ = 0.0;
    }

    /** Audio thread: swap buffer pointer without resetting playback state. */
    void swapBuffer (const juce::AudioBuffer<float>* buf, double sourceSampleRate)
    {
        buffer_     = buf;
        sourceRate_ = sourceSampleRate;
    }

    /** Audio thread: start playback from regionStart. */
    void trigger (float velocity = 1.0f)
    {
        if (buffer_ == nullptr) return;
        velocity_  = velocity;
        exhausted_ = false;
        playing_   = true;
        needsPositionReset_ = true;
        triggerFadeRemaining_ = kTriggerFadeSamples;
        adsr_.noteOn();
    }

    /** Silence immediately and enter ADSR release. */
    void stop()
    {
        adsr_.noteOff();
    }

    bool isPlaying() const { return playing_; }

    /** UI thread: normalised [0,1] position within the full sample buffer. */
    float getPlayPositionNorm() const { return playPosNorm_.load (std::memory_order_relaxed); }

    //==========================================================================
    void renderBlock (juce::AudioBuffer<float>& output,
                      int startSample, int numSamples,
                      double playbackRate,
                      const Params& params,
                      uint32_t& /*rngState*/)
    {
        if (!playing_) return;
        numSamples = std::min (numSamples, tempBuf_.getNumSamples());

        // ── Update DSP params ────────────────────────────────────────────────
        adsr_.setParameters ({
            params.attackSec, params.decaySec,
            params.sustain,   params.releaseSec
        });
        filter_.setCutoffFrequency (params.filterFreqHz);
        filter_.setResonance (std::max (0.5f, params.filterRes));

        juce::Reverb::Parameters rp;
        rp.roomSize   = params.reverbSize;
        rp.damping    = 0.5f;
        rp.wetLevel   = params.reverbMix * 0.5f;
        rp.dryLevel   = 1.0f;
        rp.width      = 1.0f;
        rp.freezeMode = params.reverbFreeze ? 1.0f : 0.0f;
        reverb_.setParameters (rp);

        // ── Resolve region + loop boundaries (samples) ───────────────────────
        int   srcLen   = (buffer_ != nullptr) ? buffer_->getNumSamples() : 0;
        if (srcLen == 0) { playing_ = false; return; }

        double rgnStart = (double) params.regionStart * srcLen;
        double rgnEnd   = (double) params.regionEnd   * srcLen;
        if (rgnEnd   <= rgnStart) rgnEnd   = rgnStart + 1.0;
        rgnEnd   = std::min (rgnEnd,   (double)(srcLen - 1));

        double loopSizeSamples = (double) params.loopSizeMs * sourceRate_ / 1000.0;
        double lStart, lEnd;

        if (params.loopSizeLock)
        {
            lStart = (double) params.loopStart * srcLen;
            lEnd   = lStart + loopSizeSamples;
        }
        else
        {
            lStart = (double) params.loopStart * srcLen;
            lEnd   = (double) params.loopEnd   * srcLen;
            if (lEnd <= lStart) lEnd = lStart + 1.0;
        }

        lStart = std::max (lStart, rgnStart);
        lEnd   = std::min (lEnd,   rgnEnd);
        if (lEnd <= lStart) lEnd = lStart + 1.0;

        if (needsPositionReset_)
        {
            position_           = lStart;
            loopAnchorSamples_  = lStart;
            needsPositionReset_ = false;
        }

        // ── Pitch + resample ratio ────────────────────────────────────────────
        double pitchRatio    = std::pow (2.0, (double) params.pitchSemitones / 12.0);
        double effectiveStep = (sourceRate_ / playbackRate) * pitchRatio;

        if (srcLen > 0)
            playPosNorm_.store ((float)(position_ / srcLen), std::memory_order_relaxed);

        // ── Fill tempBuf_ with sample data ────────────────────────────────────
        tempBuf_.clear (0, numSamples);
        int srcChans  = buffer_->getNumChannels();
        int dstChans  = std::min (tempBuf_.getNumChannels(), 2);

        for (int i = 0; i < numSamples; ++i)
        {
            if (!exhausted_)
            {
                bool pastLoopEnd = (position_ >= lEnd);
                bool pastRgnEnd  = (position_ >= rgnEnd);

                // OnsetSeq/OnsetRnd: one-shot per trigger — do NOT loop internally.
                // The sequencer in VoiceChannel advances the anchor on each hit.
                bool canLoop = params.loopMode != LoopMode::Off
                            && params.loopMode != LoopMode::OnsetSeq
                            && params.loopMode != LoopMode::OnsetRnd;
                if (pastLoopEnd && canLoop)
                {
                    double span = lEnd - loopAnchorSamples_;
                    if (span < 1.0) span = 1.0;
                    position_ = loopAnchorSamples_
                              + std::fmod (position_ - loopAnchorSamples_, span);
                }
                else if (pastLoopEnd || pastRgnEnd)
                {
                    exhausted_ = true;
                    adsr_.noteOff();
                }
            }

            if (!exhausted_ && position_ < (double)(srcLen - 1))
            {
                int   a    = (int) position_;
                int   b    = std::min (a + 1, srcLen - 1);
                float frac = (float)(position_ - a);

                for (int ch = 0; ch < dstChans; ++ch)
                {
                    int sc = ch % srcChans;
                    float s = buffer_->getSample (sc, a) * (1.0f - frac)
                            + buffer_->getSample (sc, b) * frac;
                    tempBuf_.addSample (ch, i, s);
                }
                position_ += effectiveStep;
            }
        }

        // ── ADSR envelope ─────────────────────────────────────────────────────
        adsr_.applyEnvelopeToBuffer (tempBuf_, 0, numSamples);

        if (exhausted_ && !adsr_.isActive())
        {
            playing_ = false;
            tempBuf_.clear();
            return;
        }

        // ── Anti-click: linear fade-in on each trigger (~5ms) ─────────────────
        if (triggerFadeRemaining_ > 0)
        {
            int sampleOffset = kTriggerFadeSamples - triggerFadeRemaining_;
            int n = std::min (numSamples, triggerFadeRemaining_);
            for (int i = 0; i < n; ++i)
            {
                float t = (float)(sampleOffset + i) / (float)kTriggerFadeSamples;
                for (int ch = 0; ch < dstChans; ++ch)
                    tempBuf_.setSample (ch, i, tempBuf_.getSample (ch, i) * t);
            }
            triggerFadeRemaining_ -= n;
        }

        // ── Pre-amp gain (before drive) ───────────────────────────────────────
        if (std::abs (params.preGain - 1.0f) > 0.001f)
        {
            for (int ch = 0; ch < dstChans; ++ch)
                juce::FloatVectorOperations::multiply (
                    tempBuf_.getWritePointer (ch), params.preGain, numSamples);
        }

        // ── Distortion ────────────────────────────────────────────────────────
        if (params.distDrive > 0.001f)
        {
            float preGainDrv  = 1.0f + params.distDrive * 9.0f;
            float postGainDrv = 1.0f / std::tanh (preGainDrv);
            for (int ch = 0; ch < dstChans; ++ch)
            {
                float* d = tempBuf_.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    d[i] = std::tanh (d[i] * preGainDrv) * postGainDrv;
            }
        }

        // ── Low-pass filter ───────────────────────────────────────────────────
        if (params.filterFreqHz < 19900.0f)
        {
            juce::dsp::AudioBlock<float> block (
                tempBuf_.getArrayOfWritePointers(),
                (size_t) dstChans, 0, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            filter_.process (ctx);
        }

        // ── Reverb ────────────────────────────────────────────────────────────
        if (params.reverbMix > 0.001f || params.reverbFreeze)
        {
            if (srcChans == 1)
                tempBuf_.copyFrom (1, 0, tempBuf_, 0, 0, numSamples);
            reverb_.processStereo (tempBuf_.getWritePointer (0),
                                   tempBuf_.getWritePointer (1), numSamples);
        }

        // ── Per-voice limiter ─────────────────────────────────────────────────
        if (params.limitThreshDb < -0.5f)
        {
            limiter_.setThreshold (params.limitThreshDb);
            juce::dsp::AudioBlock<float> block (
                tempBuf_.getArrayOfWritePointers(),
                (size_t) dstChans, 0, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            limiter_.process (ctx);
        }

        // ── Mix into output (params.gain = post-FX track level) ───────────────
        float finalGain = params.gain * velocity_;
        int   outChans  = output.getNumChannels();
        for (int ch = 0; ch < outChans; ++ch)
            output.addFrom (ch, startSample, tempBuf_, ch % dstChans, 0, numSamples, finalGain);
    }

private:
    // UI-readable (written audio thread, read UI timer)
    std::atomic<float> playPosNorm_ { 0.0f };

    const juce::AudioBuffer<float>* buffer_     = nullptr;
    double sourceRate_       = 44100.0;
    double sampleRate_       = 44100.0;
    double position_         = 0.0;
    double loopAnchorSamples_ = 0.0;
    float  velocity_         = 1.0f;
    bool   playing_          = false;
    bool   exhausted_        = false;
    bool   needsPositionReset_ = false;

    // Anti-click: linear fade-in over ~5ms on each trigger
    int triggerFadeRemaining_ = 0;
    static constexpr int kTriggerFadeSamples = 220;  // ~5ms @ 44.1kHz

    juce::ADSR                               adsr_;
    juce::dsp::StateVariableTPTFilter<float> filter_;
    juce::Reverb                             reverb_;
    juce::dsp::Limiter<float>                limiter_;
    juce::AudioBuffer<float>                 tempBuf_;
};
