#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "EuclideanSequencer.h"
#include "SamplerVoice.h"
#include "SampleLibrary.h"

/**
 * W2SamplerProcessor — the audio brain of the plugin.
 *
 * Owns all audio parameters, the euclidean sequencer, the sample voice (DSP),
 * and the sample library (loaded files). Coordinates the audio thread (processBlock)
 * and the message thread (UI callbacks, file loading).
 *
 * Two threads interact with this class:
 *   Audio thread  → processBlock() — HIGH PRIORITY, no alloc, no I/O, no locking
 *   Message thread → editor callbacks, loadSample/loadFolder, parameter UI writes
 *
 * Parameters use juce::AudioParameter* (atomic internally) — safe to read on audio
 * thread and write from message thread.
 *
 * Sample advance modes (sampleAdvanceMode parameter):
 *   0 = Hold     — same sample every hit (classic sampler)
 *   1 = Sequential — advance to next sample each hit (walks through folder)
 *   2 = Random   — pick random sample each hit (chaotic/generative)
 */
class W2SamplerProcessor : public juce::AudioProcessor
{
public:
    W2SamplerProcessor();
    ~W2SamplerProcessor() override;

    // ── JUCE AudioProcessor interface ────────────────────────────────────────
    void prepareToPlay   (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock    (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // ── Editor ────────────────────────────────────────────────────────────────
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ── Plugin metadata ───────────────────────────────────────────────────────
    const juce::String getName() const override  { return "W2 Sampler"; }
    bool   acceptsMidi()  const override         { return true; }
    bool   producesMidi() const override         { return false; }
    bool   isMidiEffect() const override         { return false; }
    double getTailLengthSeconds() const override { return 4.0; }  // reverb tail

    // ── Programs ──────────────────────────────────────────────────────────────
    int  getNumPrograms() override                              { return 1; }
    int  getCurrentProgram() override                           { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return "Default"; }
    void changeProgramName (int, const juce::String&) override  {}

    // ── State I/O ─────────────────────────────────────────────────────────────
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ── Parameters (public so editor can read/write directly) ─────────────────

    // Sequencer
    juce::AudioParameterInt*   seqSteps    = nullptr;  // 1–32 steps
    juce::AudioParameterInt*   seqHits     = nullptr;  // 0–32 hits
    juce::AudioParameterInt*   seqRotation = nullptr;  // 0–31 rotation
    juce::AudioParameterFloat* seqRate     = nullptr;  // 0.25–4.0 steps/beat
    juce::AudioParameterFloat* sampleGain  = nullptr;  // 0–2 output gain

    // Voice / pitch
    juce::AudioParameterFloat* pitch       = nullptr;  // -24 to +24 semitones

    // ADSR envelope
    juce::AudioParameterFloat* attack      = nullptr;  // 0.001–2.0 sec
    juce::AudioParameterFloat* decay       = nullptr;  // 0.001–2.0 sec
    juce::AudioParameterFloat* sustain     = nullptr;  // 0–1
    juce::AudioParameterFloat* release     = nullptr;  // 0.001–4.0 sec

    // Filter
    juce::AudioParameterFloat* filterFreq  = nullptr;  // 20–20000 Hz
    juce::AudioParameterFloat* filterRes   = nullptr;  // 0.5–10.0 Q

    // Distortion + Reverb
    juce::AudioParameterFloat* distDrive   = nullptr;  // 0–1
    juce::AudioParameterFloat* reverbMix   = nullptr;  // 0–1 wet mix
    juce::AudioParameterFloat* reverbSize  = nullptr;  // 0–1 room size
    juce::AudioParameterBool*  reverbFreeze = nullptr; // freeze mode

    // Sample advance mode (0=Hold, 1=Sequential, 2=Random)
    juce::AudioParameterInt*   sampleAdvanceMode = nullptr;

    // ── Public methods (message thread) ───────────────────────────────────────

    /** Load and decode all audio files from a folder into the SampleLibrary. */
    void loadFolder  (const juce::File& folder);

    /** Navigate the library (message thread). Each call also loads the sample. */
    void prevSample();
    void nextSample();
    void randomSample();

    /** Randomize all voice/FX params (not sequencer, not gain). */
    void randomizeVoiceParams();

    /** Transport control — stop resets clock and pattern. */
    void setPlaying (bool shouldPlay);
    bool getPlaying() const;

    // ── UI accessors (safe from message thread after library is loaded) ───────
    bool               hasSample()          const;
    int                getLibraryCount()    const { return library_.getCount(); }
    int                getLibraryIndex()    const { return library_.currentIndex.load(); }
    juce::String       getCurrentSampleName() const { return library_.currentName(); }

    /** Returns pointer to the current sample's buffer for waveform drawing.
     *  Message thread only. May return nullptr if no sample is loaded. */
    const juce::AudioBuffer<float>* getCurrentSampleBuffer() const;

    /** Euclidean pattern snapshot for UI grid drawing. */
    std::vector<bool> getCurrentPattern() const;
    int               getCurrentStep()    const;

private:
    // ── DSP ───────────────────────────────────────────────────────────────────
    EuclideanSequencer sequencer;
    W2SamplerVoice     voice;
    SampleLibrary      library_;
    juce::AudioFormatManager formatManager_;

    // ── Audio engine state ────────────────────────────────────────────────────
    double sampleRate_     = 44100.0;
    double samplesPerStep_ = 0.0;
    double stepPhase_      = 0.0;
    std::atomic<bool> isPlaying_ { false };

    // ── Param diff caches (detect changes so we only rebuild when needed) ──────
    int   lastSteps_    = -1, lastHits_ = -1, lastRotation_ = -1;
    float lastRate_     = -1.0f;
    float lastPitch_    = -9999.0f;
    float lastAttack_   = -1.0f, lastDecay_ = -1.0f, lastSustain_ = -1.0f, lastRelease_ = -1.0f;
    float lastFilterFreq_ = -1.0f, lastFilterRes_ = -1.0f;
    float lastDistDrive_  = -1.0f;
    float lastReverbMix_  = -1.0f, lastReverbSize_ = -1.0f;
    bool  lastReverbFreeze_ = false;

    // ── Audio-thread RNG state for random sample selection ────────────────────
    // Simple LCG — audio thread only (no sharing, no atomics needed)
    uint32_t audioRng_ = 0xDEADBEEF;

    // ── Private helpers ────────────────────────────────────────────────────────
    void rebuildSequencerIfNeeded();
    void rebuildVoiceParamsIfNeeded (W2SamplerVoice::Params& params);
    void advanceClock (int numSamples);
    void loadCurrentSample();   // loads library_.current() into voice

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerProcessor)
};
