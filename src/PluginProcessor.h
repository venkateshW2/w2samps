#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MasterClock.h"
#include "VoiceChannel.h"

/**
 * W2SamplerProcessor — Phase 1 (phasor-based, 3 voices).
 *
 * Global param: bpm
 * Per-voice (30 params × 3 = 90): rate, phaseOffset, warp, reverse, quantiseAmt,
 *   phaseSource, seqSteps, seqHits, seqRotation, sampleAdv, rndFxChance,
 *   pitch, ADSR, filterFreq/Res, distDrive, reverbMix/Size/Freeze, gain,
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

        // Distortion + Reverb
        juce::AudioParameterFloat* distDrive    = nullptr;
        juce::AudioParameterFloat* reverbMix    = nullptr;
        juce::AudioParameterFloat* reverbSize   = nullptr;
        juce::AudioParameterBool*  reverbFreeze = nullptr;
        juce::AudioParameterFloat* gain         = nullptr;

        // Region + Loop
        juce::AudioParameterFloat* regionStart  = nullptr;
        juce::AudioParameterFloat* regionEnd    = nullptr;
        juce::AudioParameterInt*   loopMode     = nullptr; // 0=Off,1=Fixed,2=Rnd,3=Seq
        juce::AudioParameterFloat* loopStart    = nullptr;
        juce::AudioParameterFloat* loopEnd      = nullptr;
        juce::AudioParameterFloat* loopSizeMs   = nullptr; // 5–5000 ms
        juce::AudioParameterBool*  loopSizeLock = nullptr;
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
    juce::AudioParameterFloat* bpm    = nullptr;
    juce::AudioParameterInt*   clkDiv = nullptr;  // beats per phasor cycle: 1,2,4,8
    VoiceParamPtrs vp[3];

    //==========================================================================
    // Message-thread API
    void loadFolder      (int v, const juce::File& folder);
    void prevSample      (int v);
    void nextSample      (int v);
    void randomSample    (int v);
    void randomizeVoiceParams (int v);

    /** Check if any voice requested FX randomize on the last block; fire if so.
     *  Call from the editor's 20fps timer. Thread-safe (atomic exchange). */
    void checkAndFireRandomizations();

    void setPlaying (bool p);
    bool getPlaying() const { return isPlaying_.load(); }

    // Mute / solo (non-serialised live state)
    void setVoiceMute (int v, bool muted);
    void setVoiceSolo (int v);          // call with -1 to clear all solos
    bool getVoiceMuted (int v) const    { return v>=0&&v<3 ? voiceMuted_[v].load() : false; }
    int  getSoloVoice()        const    { return soloVoice_.load(); }

    const VoiceChannel& getVoice (int v) const { return voices_[v]; }

private:
    void fillVoiceParams  (int v, VoiceChannel::Params& out) const;
    double selectInputPhase (int v, double masterPhase) const;
    void registerVoiceParams (int v, const juce::String& prefix);

    MasterClock  clock_;
    VoiceChannel voices_[3];
    juce::AudioFormatManager formatManager_;
    double sampleRate_ = 44100.0;
    std::atomic<bool> isPlaying_    { false };
    std::atomic<bool> voiceMuted_[3];
    std::atomic<int>  soloVoice_    { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerProcessor)
};
