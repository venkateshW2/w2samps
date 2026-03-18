#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

/**
 * W2SamplerVoice — one-shot sample player with a full DSP chain.
 *
 * DSP signal chain (per audio block):
 *
 *   Sample read (linear interp, pitch-adjusted ratio)
 *        ↓
 *   ADSR envelope (juce::ADSR — shapes amplitude over time)
 *        ↓
 *   Distortion (tanh soft-clip with pre-gain drive)
 *        ↓
 *   Low-pass filter (juce::dsp::StateVariableTPTFilter)
 *        ↓
 *   Reverb (juce::Reverb — with freeze mode)
 *        ↓
 *   Gain × Velocity → mix into output
 *
 * Thread safety:
 *   prepare()    — message thread or audio thread before playback starts
 *   loadBuffer() — message thread only (no audio running)
 *   trigger()    — audio thread (called from processBlock on a hit)
 *   renderBlock()— audio thread
 *   All DSP objects (filter, reverb, ADSR) are touched only from the audio thread.
 */
class W2SamplerVoice
{
public:
    /**
     * Parameters passed to renderBlock() every audio block.
     * Built by PluginProcessor from AudioParameter values (all on audio thread).
     * Kept as a plain struct to avoid individual atomic setters per param.
     */
    struct Params
    {
        float pitchSemitones = 0.0f;      // -24 to +24 semitones
        float attackSec      = 0.005f;    // ADSR attack  (0.001 – 2.0 sec)
        float decaySec       = 0.1f;      // ADSR decay   (0.001 – 2.0 sec)
        float sustain        = 0.8f;      // ADSR sustain (0.0 – 1.0)
        float releaseSec     = 0.2f;      // ADSR release (0.001 – 4.0 sec)
        float filterFreqHz   = 20000.0f;  // LP cutoff    (20 – 20000 Hz)
        float filterRes      = 0.707f;    // Q value      (0.5 – 10.0)
        float distDrive      = 0.0f;      // distortion   (0.0 – 1.0)
        float reverbMix      = 0.0f;      // reverb wet   (0.0 – 1.0)
        float reverbSize     = 0.5f;      // room size    (0.0 – 1.0)
        bool  reverbFreeze   = false;     // freeze the reverb tail
        float gain           = 1.0f;      // output gain  (0.0 – 2.0)
    };

    //==========================================================================
    /**
     * Prepare DSP objects for a given sample rate and block size.
     * Call from prepareToPlay(). Safe to call multiple times.
     */
    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_ = sampleRate;

        // Pre-allocate temp buffer so renderBlock never allocates
        tempBuf_.setSize (2, maxBlockSize, false, true, false);

        // Prepare the low-pass filter (stereo)
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) maxBlockSize;
        spec.numChannels      = 2;
        filter_.prepare (spec);
        filter_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        filter_.setCutoffFrequency (20000.0f);  // start wide open
        filter_.setResonance (0.707f);           // Butterworth Q

        // Prepare ADSR
        adsr_.setSampleRate (sampleRate);
        adsr_.setParameters ({ 0.005f, 0.1f, 0.8f, 0.2f });  // sensible defaults

        // Prepare reverb
        reverb_.reset();
        reverb_.setSampleRate (sampleRate);
        juce::Reverb::Parameters rp;
        rp.roomSize   = 0.5f;
        rp.damping    = 0.5f;
        rp.wetLevel   = 0.0f;   // start dry
        rp.dryLevel   = 1.0f;
        rp.width      = 1.0f;
        rp.freezeMode = 0.0f;
        reverb_.setParameters (rp);

        // Reset playback state
        position_  = 0.0;
        playing_   = false;
        exhausted_ = false;
    }

    /**
     * Point the voice at a new audio buffer.
     * MESSAGE THREAD ONLY — must not be called while audio is running.
     * Does not reset the ADSR or reverb state.
     */
    void loadBuffer (const juce::AudioBuffer<float>* buf, double sourceSampleRate)
    {
        buffer_      = buf;
        sourceRate_  = sourceSampleRate;
        playing_     = false;
        exhausted_   = false;
        position_    = 0.0;
    }

    /** Same as above but for audio thread use — only swaps pointer + rate, no state reset. */
    void swapBuffer (const juce::AudioBuffer<float>* buf, double sourceSampleRate)
    {
        buffer_     = buf;
        sourceRate_ = sourceSampleRate;
        // DON'T reset position or playing — caller handles that via trigger()
    }

    /**
     * Start (or re-start) playback.
     * Called from the audio thread on a sequencer hit or MIDI note-on.
     */
    void trigger (float velocity = 1.0f)
    {
        if (buffer_ == nullptr) return;
        position_  = 0.0;
        velocity_  = velocity;
        exhausted_ = false;
        playing_   = true;
        adsr_.noteOn();  // start the envelope from the Attack phase
    }

    /** Silence immediately — also tells the ADSR to release. */
    void stop()
    {
        adsr_.noteOff();  // let the ADSR Release tail play out naturally
        // (playing_ becomes false when envelope finishes in renderBlock)
    }

    bool isPlaying() const { return playing_; }

    /**
     * Render one audio block into output (ADD mode — does not clear output first).
     *
     * output          — destination stereo buffer to mix into
     * startSample     — which sample index in output to start writing at
     * numSamples      — number of samples to render
     * playbackRate    — current engine sample rate (e.g. 48000)
     * params          — current parameter snapshot (built by processor)
     *
     * AUDIO THREAD ONLY.
     */
    void renderBlock (juce::AudioBuffer<float>& output,
                      int startSample, int numSamples,
                      double playbackRate,
                      const Params& params)
    {
        if (!playing_) return;

        // Clamp in case of an oversized block (should never happen in practice)
        numSamples = std::min (numSamples, tempBuf_.getNumSamples());

        // ── 1. Update DSP params (cheap — just sets a few floats) ────────────
        adsr_.setParameters ({
            params.attackSec,
            params.decaySec,
            params.sustain,
            params.releaseSec
        });

        filter_.setCutoffFrequency (params.filterFreqHz);
        filter_.setResonance (std::max (0.5f, params.filterRes));  // Q must be >= 0.5

        juce::Reverb::Parameters rp;
        rp.roomSize   = params.reverbSize;
        rp.damping    = 0.5f;
        rp.wetLevel   = params.reverbMix * 0.5f;   // scale: reverb can be loud
        rp.dryLevel   = 1.0f;
        rp.width      = 1.0f;
        rp.freezeMode = params.reverbFreeze ? 1.0f : 0.0f;
        reverb_.setParameters (rp);  // safe — only called from audio thread

        // ── 2. Fill tempBuf_ with raw sample data (with pitch + rate adjust) ─
        tempBuf_.clear (0, numSamples);

        // Pitch ratio: semitones → multiplier. Combined with resampling ratio.
        // pitchRatio  1.0 = no shift, 2.0 = +12 semitones (octave up), etc.
        double pitchRatio    = std::pow (2.0, (double) params.pitchSemitones / 12.0);
        double effectiveStep = (sourceRate_ / playbackRate) * pitchRatio;

        int srcLen     = (buffer_ != nullptr) ? buffer_->getNumSamples() : 0;
        int srcChans   = (buffer_ != nullptr) ? buffer_->getNumChannels() : 0;
        int dstChans   = std::min (tempBuf_.getNumChannels(), 2);

        for (int i = 0; i < numSamples; ++i)
        {
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
            else
            {
                // Sample buffer is exhausted — begin release phase
                if (!exhausted_)
                {
                    exhausted_ = true;
                    adsr_.noteOff();   // triggers release tail
                }
                // Continue processing (silence from sample) so reverb/ADSR can finish
            }
        }

        // ── 3. ADSR envelope ─────────────────────────────────────────────────
        // applyEnvelopeToBuffer multiplies each sample by the current envelope level
        adsr_.applyEnvelopeToBuffer (tempBuf_, 0, numSamples);

        // If envelope is completely silent and sample is done, stop the voice
        if (exhausted_ && !adsr_.isActive())
        {
            playing_ = false;
            // Clear whatever's in the temp buffer to prevent any stray signal
            tempBuf_.clear();
            return;
        }

        // ── 4. Distortion (tanh soft-clip) ───────────────────────────────────
        // drive 0 = bypass, drive 1 = heavy saturation
        if (params.distDrive > 0.001f)
        {
            float preGain  = 1.0f + params.distDrive * 9.0f;  // 1× to 10×
            float postGain = 1.0f / std::tanh (preGain);      // normalise output level

            for (int ch = 0; ch < dstChans; ++ch)
            {
                float* data = tempBuf_.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    data[i] = std::tanh (data[i] * preGain) * postGain;
            }
        }

        // ── 5. Low-pass filter ────────────────────────────────────────────────
        // Only process if filter is not fully open (saves CPU when bypassed)
        if (params.filterFreqHz < 19900.0f)
        {
            juce::dsp::AudioBlock<float> block (
                tempBuf_.getArrayOfWritePointers(), (size_t) dstChans, 0, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            filter_.process (ctx);
        }

        // ── 6. Reverb ─────────────────────────────────────────────────────────
        // processStereo works in-place; it mixes dry and wet based on parameters
        if (params.reverbMix > 0.001f || params.reverbFreeze)
        {
            // Ensure mono source is duplicated to both channels for reverb
            if (srcChans == 1)
                tempBuf_.copyFrom (1, 0, tempBuf_, 0, 0, numSamples);

            reverb_.processStereo (tempBuf_.getWritePointer (0),
                                   tempBuf_.getWritePointer (1),
                                   numSamples);
        }

        // ── 7. Mix into output buffer ─────────────────────────────────────────
        float finalGain = params.gain * velocity_;
        int   outChans  = output.getNumChannels();

        for (int ch = 0; ch < outChans; ++ch)
        {
            int srcCh = ch % dstChans;
            output.addFrom (ch, startSample, tempBuf_, srcCh, 0, numSamples, finalGain);
        }
    }

private:
    // ── Sample source ─────────────────────────────────────────────────────────
    const juce::AudioBuffer<float>* buffer_   = nullptr;  // ptr into SampleLibrary (not owned)
    double sourceRate_   = 44100.0;  // sample rate of the source file
    double position_     = 0.0;      // fractional read position in source buffer
    float  velocity_     = 1.0f;     // amplitude from trigger velocity (0-1)
    bool   playing_      = false;    // true while envelope is active
    bool   exhausted_    = false;    // true once sample buffer runs out

    // ── DSP state ─────────────────────────────────────────────────────────────
    double sampleRate_ = 44100.0;    // engine sample rate (set in prepare)

    juce::ADSR adsr_;                // amplitude envelope

    juce::dsp::StateVariableTPTFilter<float> filter_;  // low-pass filter
    // StateVariableTPT is a topology-preserving filter: stays stable even at
    // high resonance values and doesn't alias near Nyquist.

    juce::Reverb reverb_;            // plate-style reverb with freeze

    juce::AudioBuffer<float> tempBuf_;  // pre-allocated work buffer (prepare() sets size)
};
