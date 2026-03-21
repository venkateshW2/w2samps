#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MasterClock.h"
#include "VoiceChannel.h"
#include "TimelineEnv.h"
#include "FluCoMaAnalyser.h"

/**
 * W2SamplerProcessor — Phase 1 (phasor-based, 3 voices).
 *
 * Global param: bpm
 * Per-voice (33 params × 3 = 99): rate, phaseOffset, warp, reverse, quantiseAmt,
 *   phaseSource, seqSteps, seqHits, seqRotation, sampleAdv, rndFxChance,
 *   pitch, ADSR, filterFreq/Res, distDrive, reverbMix/Size/Freeze,
 *   preGain, gain, limitThresh,
 *   regionStart/End, loopMode, loopStart/End, loopSizeMs, loopSizeLock.
 *
 * Parameter IDs use v0_/v1_/v2_ prefixes — never rename (DAW project compatibility).
 */
class W2SamplerProcessor : public juce::AudioProcessor
{
public:
    //==========================================================================
    struct VoiceParamPtrs
    {
        // Phase source & rate
        juce::AudioParameterInt*   phaseSource  = nullptr; // 0=Master,1=LockV1,2=LockV2,3=LockV3
        juce::AudioParameterFloat* rate         = nullptr; // 0.125–8.0 (÷8…×8)

        // Phase transform
        juce::AudioParameterFloat* phaseOffset  = nullptr; // 0→1  (time-shift)
        juce::AudioParameterFloat* warp         = nullptr; // -1→+1 (curve)
        juce::AudioParameterBool*  reverse      = nullptr;
        juce::AudioParameterFloat* quantiseAmt  = nullptr; // 0=smooth, 1=hard steps

        // Euclidean sequencer
        juce::AudioParameterInt*   seqSteps     = nullptr; // 1–32
        juce::AudioParameterInt*   seqHits      = nullptr; // 0–32
        juce::AudioParameterInt*   seqRotation  = nullptr; // 0–31
        juce::AudioParameterInt*   sampleAdv    = nullptr; // 0=Hold,1=Seq,2=Rnd
        juce::AudioParameterFloat* rndFxChance  = nullptr; // 0–1 (FX rnd probability)

        // Pitch + ADSR
        juce::AudioParameterFloat* pitch        = nullptr; // -24→+24 semitones
        juce::AudioParameterFloat* attack       = nullptr;
        juce::AudioParameterFloat* decay        = nullptr;
        juce::AudioParameterFloat* sustain      = nullptr;
        juce::AudioParameterFloat* release      = nullptr;

        // Filter
        juce::AudioParameterFloat* filterFreq   = nullptr; // 20–20000 Hz
        juce::AudioParameterFloat* filterRes    = nullptr; // 0.5–10.0

        // Gain structure
        juce::AudioParameterFloat* preGain      = nullptr; // pre-amp before drive (0.25–4.0)
        juce::AudioParameterFloat* gain         = nullptr; // post-FX track level (0.0–2.0)
        juce::AudioParameterFloat* limitThresh  = nullptr; // per-voice limiter dB (-24–0, 0=bypass)

        // Distortion + Reverb
        juce::AudioParameterFloat* distDrive    = nullptr;
        juce::AudioParameterFloat* reverbMix    = nullptr;
        juce::AudioParameterFloat* reverbSize   = nullptr;
        juce::AudioParameterBool*  reverbFreeze = nullptr;

        // Region + Loop
        juce::AudioParameterFloat* regionStart  = nullptr;
        juce::AudioParameterFloat* regionEnd    = nullptr;
        juce::AudioParameterInt*   loopMode     = nullptr; // 0=Off,1=Fixed,2=Rnd,3=Seq
        juce::AudioParameterFloat* loopStart    = nullptr;
        juce::AudioParameterFloat* loopEnd      = nullptr;
        juce::AudioParameterFloat* loopSizeMs   = nullptr; // 5–5000 ms
        juce::AudioParameterBool*  loopSizeLock = nullptr;
        juce::AudioParameterFloat* smoothMs     = nullptr; // 0–200 ms param smoothing
        juce::AudioParameterBool*  bungeeMode   = nullptr; // true = Bungee pitch-only

        // Function generator modulation (4 per voice)
        static constexpr int kNumFg = 4;
        juce::AudioParameterFloat* fgRateVal[kNumFg] = {};  // 0.001–32.0 (mult when sync, Hz when free)
        juce::AudioParameterBool*  fgSync[kNumFg]    = {};  // true=sync to phasor, false=free Hz
        juce::AudioParameterInt*   fgDest[kNumFg]    = {};  // ModDest index
        juce::AudioParameterFloat* fgDepth[kNumFg]   = {};  // -1 → +1
        juce::AudioParameterFloat* fgMin[kNumFg]     = {};  // 0 → 1 (normalised range lo)
        juce::AudioParameterFloat* fgMax[kNumFg]     = {};  // 0 → 1 (normalised range hi)
    };

    //==========================================================================
    W2SamplerProcessor();
    ~W2SamplerProcessor() override;

    void prepareToPlay   (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock    (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override  { return "W2 Sampler"; }
    bool   acceptsMidi()  const override         { return true; }
    bool   producesMidi() const override         { return false; }
    bool   isMidiEffect() const override         { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int  getNumPrograms() override                              { return 1; }
    int  getCurrentProgram() override                           { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return "Default"; }
    void changeProgramName (int, const juce::String&) override  {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    // Public params
    juce::AudioParameterFloat* bpm        = nullptr;
    juce::AudioParameterInt*   clkDiv     = nullptr;  // beats per phasor cycle: 1,2,4,8
    juce::AudioParameterFloat* masterGain = nullptr;
    VoiceParamPtrs vp[3];

    //==========================================================================
    // Preset system (8 slots per voice)
    struct VoicePreset
    {
        bool valid = false;
        // FX params only (the 10 randomizable params + preGain + limitThresh)
        float pitch      = 0.0f;
        float attack     = 0.005f, decay = 0.1f, sustain = 0.8f, release_ = 0.2f;
        float filterFreq = 20000.0f, filterRes = 0.707f;
        float distDrive  = 0.0f;
        float reverbMix  = 0.0f, reverbSize = 0.5f;
        float preGain    = 1.0f;
        float gain       = 1.0f;
        float limitThresh = 0.0f;
    };
    VoicePreset presets_[3][8];

    void saveVoicePreset (int v, int slot);
    void loadVoicePreset (int v, int slot);

    //==========================================================================
    // Message-thread API
    void loadFolder      (int v, const juce::File& folder);
    void loadSingleFile  (int v, const juce::File& file);
    /** Load file and inject pre-computed FluCoMa analysis (onset positions, key, BPM etc.)
     *  from SoundBrowser. Skips redundant re-analysis. Message thread only. */
    void loadSingleFileWithAnalysis (int v, const juce::File& file, const FluCoMaResult& analysis);
    /** Load a saved playlist into voice v. Decodes all files; injects FluCoMa analysis. */
    void loadPlaylist (int v, const juce::String& playlistName);
    void prevSample      (int v);
    void nextSample      (int v);
    void randomSample    (int v);

    /** Re-run onset detection on all samples for voice v with new sensitivity. Message thread only. */
    void reanalyseOnsets (int v, float sensitivity);

    /** Randomize FX params for voice v.
     *  locked[10]: if non-null, locked[i]==true means param i is skipped.
     *  Order: pitch(0) attack(1) decay(2) sustain(3) release(4)
     *         filterFreq(5) filterRes(6) distDrive(7) reverbMix(8) reverbSize(9).
     */
    void randomizeVoiceParams (int v, const bool* locked = nullptr);

    /** Reset all FX params for voice v to default values. */
    void resetVoiceFX (int v);

    /** Returns true (and clears the flag) if voice v had a hit-triggered FX randomize request.
     *  Call from the editor's timer so the editor can pass its own lock mask. */
    bool takeRandomizeFXRequest (int v);

    void setPlaying (bool p);
    bool getPlaying() const { return isPlaying_.load(); }

    float getClockPhase() const { return clockPhaseUI_.load (std::memory_order_relaxed); }
    void  requestClick()        { pendingClick_.store (true, std::memory_order_relaxed); }

    // ── Preview playback (SoundBrowser) ──────────────────────────────────────
    /** Takes a shared_ptr — no copy, both browser and processor share the same buffer. */
    void  startPreview  (std::shared_ptr<juce::AudioBuffer<float>> buf, double srcRate);
    void  stopPreview   () { previewPos_.store (-1, std::memory_order_relaxed); }
    bool  isPreviewPlaying() const { return previewPos_.load (std::memory_order_relaxed) >= 0; }
    void  setPreviewLevel (float lv) { previewLevel_.store (lv, std::memory_order_relaxed); }
    /** 0..1 normalised position within the preview buffer, or -1 if stopped. */
    float getPreviewProgress() const
    {
        int pos = previewPos_.load (std::memory_order_relaxed);
        int len = previewLen_.load (std::memory_order_relaxed);
        return (pos >= 0 && len > 0) ? (float)pos / (float)len : -1.f;
    }

    float getOutputPeakL() const { return outputPeakL_.load (std::memory_order_relaxed); }
    float getOutputPeakR() const { return outputPeakR_.load (std::memory_order_relaxed); }
    void  decayOutputPeaks()
    {
        outputPeakL_.store (outputPeakL_.load (std::memory_order_relaxed) * 0.97f, std::memory_order_relaxed);
        outputPeakR_.store (outputPeakR_.load (std::memory_order_relaxed) * 0.97f, std::memory_order_relaxed);
    }
    float getShortTermLufs() const { return shortTermLufs_.load (std::memory_order_relaxed); }

    // Mute / solo (non-serialised live state)
    void setVoiceMute (int v, bool muted);
    void setVoiceSolo (int v);          // call with -1 to clear all solos
    bool getVoiceMuted (int v) const    { return v>=0&&v<3 ? voiceMuted_[v].load() : false; }
    int  getSoloVoice()        const    { return soloVoice_.load(); }

    const VoiceChannel& getVoice (int v) const { return voices_[v]; }

    /** Access a voice's FuncGen from the message thread (editor draws + edits). */
    FuncGen& getVoiceFuncGen (int v, int fg) { return voices_[v].getFuncGen (fg); }

    //==========================================================================
    // Timeline envelopes (up to 8 macro-scale multi-dest envelopes)
    static constexpr int kMaxTimelines = 8;

    TimelineEnv& getTimeline (int i)       { return timelines_[i]; }
    const TimelineEnv& getTimeline (int i) const { return timelines_[i]; }

private:
    void fillVoiceParams  (int v, VoiceChannel::Params& out) const;
    double selectInputPhase (int v, double masterPhase) const;
    void registerVoiceParams (int v, const juce::String& prefix);

    MasterClock  clock_;
    VoiceChannel voices_[3];
    TimelineEnv  timelines_[kMaxTimelines];
    juce::AudioFormatManager formatManager_;
    double sampleRate_ = 44100.0;
    std::atomic<bool>  isPlaying_    { false };
    std::atomic<bool>  voiceMuted_[3];
    std::atomic<int>   soloVoice_    { -1 };
    std::atomic<float> outputPeakL_    { 0.0f };
    std::atomic<float> outputPeakR_    { 0.0f };
    std::atomic<float> clockPhaseUI_   { 0.0f };
    std::atomic<bool>  pendingClick_   { false };
    int                clickSamplesLeft_ = 0;

    // Preview playback (SoundBrowser) — shared_ptr avoids copying the buffer
    std::shared_ptr<juce::AudioBuffer<float>> previewBufPtr_;
    juce::CriticalSection    previewLock_;
    double                   previewSrcRate_ = 44100.0;
    std::atomic<int>         previewPos_    { -1 };
    std::atomic<int>         previewLen_    { 0 };
    std::atomic<float>       previewLevel_  { 0.7f };

    // K-weighting filter state (BS.1770, two stages × stereo)
    struct Biquad {
        float b0=1,b1=0,b2=0, a1=0,a2=0, z1=0,z2=0;
        float process (float x) noexcept {
            float y = b0*x + z1;
            z1 = b1*x - a1*y + z2;
            z2 = b2*x - a2*y;
            return y;
        }
        void reset() { z1 = z2 = 0.0f; }
    };
    Biquad kw1_[2], kw2_[2];   // stage1 and stage2 per channel
    float  lufsBlockBuf_[64] = {};  // circular buffer of block mean-squares
    int    lufsBlockPos_      = 0;
    float  lufsBlockFill_     = 0;  // how many blocks are filled (up to 64)
    std::atomic<float> shortTermLufs_ { -70.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerProcessor)
};
