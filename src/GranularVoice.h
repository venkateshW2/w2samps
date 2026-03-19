#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

/**
 * GranularVoice — region-based looping sample voice with full DSP chain.
 *
 * Extends the previous W2SamplerVoice with:
 *   - Play region (regionStart → regionEnd within the sample buffer)
 *   - Four loop modes (Off / Fixed / Random / Sequential)
 *   - Loop size lock in milliseconds
 *   - Same DSP chain (ADSR → distortion → LP filter → reverb)
 *
 * One-shot behaviour = LoopMode::Off (default).
 * "Granular" = LoopMode::Random with a short loopSizeMs (5–200ms).
 *
 * ALL values stored as normalised [0,1] fractions of the buffer length,
 * except loopSizeMs which is in milliseconds. Conversion happens at render time.
 *
 * Thread safety: same as W2SamplerVoice.
 *   prepare()     — before audio starts
 *   loadBuffer()  — message thread only
 *   swapBuffer()  — audio thread only (pointer swap, no alloc)
 *   trigger()     — audio thread
 *   renderBlock() — audio thread
 */
class GranularVoice
{
public:
    enum class LoopMode { Off = 0, Fixed = 1, Random = 2, Sequential = 3 };

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

        // Region (normalised 0–1)
        float regionStart = 0.0f;
        float regionEnd   = 1.0f;

        // Loop
        LoopMode loopMode    = LoopMode::Off;
        float    loopStart   = 0.0f;   // normalised; ignored if loopSizeLock=true
        float    loopEnd     = 1.0f;   // normalised; ignored if loopSizeLock=true
        float    loopSizeMs  = 100.0f; // used when loopSizeLock=true
        bool     loopSizeLock = false; // if true, loopEnd = loopStart + loopSizeMs

        // Output
        float gain = 1.0f;
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

        position_  = 0.0;
        playing_   = false;
        exhausted_ = false;
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
        // Will be positioned at regionStart on first renderBlock call
        // We flag this with a "needs reset" bool so that position is
        // set correctly even if trigger() is called mid-block
        needsPositionReset_ = true;
        adsr_.noteOn();
    }

    /** Silence immediately and enter ADSR release. */
    void stop()
    {
        adsr_.noteOff();
    }

    bool isPlaying() const { return playing_; }

    /** UI thread: normalised [0,1] position within the full sample buffer.
     *  Atomic — safe to read from message thread while audio thread renders. */
    float getPlayPositionNorm() const { return playPosNorm_.load (std::memory_order_relaxed); }

    //==========================================================================
    void renderBlock (juce::AudioBuffer<float>& output,
                      int startSample, int numSamples,
                      double playbackRate,
                      const Params& params,
                      uint32_t& /*rngState*/)  // reserved for future per-grain randomization
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
        reverb_.setParameters (rp);   // audio-thread only — always safe here

        // ── Resolve region + loop boundaries (samples) ───────────────────────
        int   srcLen   = (buffer_ != nullptr) ? buffer_->getNumSamples() : 0;
        if (srcLen == 0) { playing_ = false; return; }

        double rgnStart = (double) params.regionStart * srcLen;
        double rgnEnd   = (double) params.regionEnd   * srcLen;
        if (rgnEnd   <= rgnStart) rgnEnd   = rgnStart + 1.0;
        rgnEnd   = std::min (rgnEnd,   (double)(srcLen - 1));

        // Loop bounds — loopStart/loopEnd are absolute [0,1] fractions of the
        // full buffer (same coordinate space as the waveform display).
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

        // Clamp loop window to region
        lStart = std::max (lStart, rgnStart);
        lEnd   = std::min (lEnd,   rgnEnd);
        if (lEnd <= lStart) lEnd = lStart + 1.0;

        // Position the playhead on first trigger — always start at the loop window
        if (needsPositionReset_)
        {
            position_           = lStart;   // play FROM loop start, not file start
            loopAnchorSamples_  = lStart;
            needsPositionReset_ = false;
        }

        // ── Pitch + resample ratio ────────────────────────────────────────────
        double pitchRatio    = std::pow (2.0, (double) params.pitchSemitones / 12.0);
        double effectiveStep = (sourceRate_ / playbackRate) * pitchRatio;

        // ── Update UI-readable playback position ──────────────────────────────
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
                // ── Loop logic: handle reaching the loop end ─────────────────
                bool pastLoopEnd = (position_ >= lEnd);
                bool pastRgnEnd  = (position_ >= rgnEnd);

                // One-shot: stop at loop end; looping: wrap within current window.
                // Window position movement (Seq/Rnd) is handled by VoiceChannel
                // on each euclidean trigger — not here.
                if (pastLoopEnd && params.loopMode != LoopMode::Off)
                {
                    // All looping modes: seamlessly wrap within the current window
                    double span = lEnd - loopAnchorSamples_;
                    if (span < 1.0) span = 1.0;
                    position_ = loopAnchorSamples_
                              + std::fmod (position_ - loopAnchorSamples_, span);
                }
                else if (pastLoopEnd || pastRgnEnd)
                {
                    // Off mode or past entire region → release
                    exhausted_ = true;
                    adsr_.noteOff();
                }
            }

            if (!exhausted_ && position_ < (double)(srcLen - 1))
            {
                // Linear interpolation read
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
            // else: silence (exhausted but ADSR still releasing — reverb tail)
        }

        // ── ADSR envelope ─────────────────────────────────────────────────────
        adsr_.applyEnvelopeToBuffer (tempBuf_, 0, numSamples);

        if (exhausted_ && !adsr_.isActive())
        {
            playing_ = false;
            tempBuf_.clear();
            return;
        }

        // ── Distortion ────────────────────────────────────────────────────────
        if (params.distDrive > 0.001f)
        {
            float preGain  = 1.0f + params.distDrive * 9.0f;
            float postGain = 1.0f / std::tanh (preGain);
            for (int ch = 0; ch < dstChans; ++ch)
            {
                float* d = tempBuf_.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    d[i] = std::tanh (d[i] * preGain) * postGain;
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

        // ── Mix into output ───────────────────────────────────────────────────
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
    double loopAnchorSamples_ = 0.0;   // current loop start in samples (updated by loop modes)
    float  velocity_         = 1.0f;
    bool   playing_          = false;
    bool   exhausted_        = false;
    bool   needsPositionReset_ = false;

    juce::ADSR                               adsr_;
    juce::dsp::StateVariableTPTFilter<float> filter_;
    juce::Reverb                             reverb_;
    juce::AudioBuffer<float>                 tempBuf_;
};
