#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

/**
 * W2SamplerEditor — 620 × 430 plugin UI.
 *
 * Layout top-to-bottom:
 *  ┌─ Title bar ────────────────────────────────────────────────────────────┐
 *  │  W2 Sampler    [sample name]     [▶ Play]  [Load File] [Load Folder]  │
 *  ├─ Waveform ─────────────────────────────────────────────────────────────┤
 *  │  ~~~~ sample waveform (drawn directly from AudioBuffer) ~~~~           │
 *  ├─ Library nav ──────────────────────────────────────────────────────────┤
 *  │  [◀ Prev] [Next ▶] [? Random]  [Mode: Hold/Seq/Rnd]  [Randomize FX]  │
 *  ├─ VOICE section ────────────────────────────────────────────────────────┤
 *  │  Pitch | Attack | Decay | Sustain | Release   (5-col sliders)         │
 *  │  FltFrq| FltRes | Drive | RvbMix | RvbSize   (5-col sliders)         │
 *  │  Gain [slider]   [Freeze]                                              │
 *  ├─ SEQUENCE section ─────────────────────────────────────────────────────┤
 *  │  Steps | Hits | Rotation | Rate              (4-col sliders)          │
 *  │  [▶ Play/■ Stop]   [? Randomize FX]                                   │
 *  ├─ Euclidean grid ───────────────────────────────────────────────────────┤
 *  │  ■ □ □ □ ■ □ □ □ ■ □ □ □ ■ □ □ □                                     │
 *  │  ● PLAYING  /  ○ STOPPED                                               │
 *  └────────────────────────────────────────────────────────────────────────┘
 */
class W2SamplerEditor : public juce::AudioProcessorEditor,
                        public juce::Timer
{
public:
    explicit W2SamplerEditor (W2SamplerProcessor&);
    ~W2SamplerEditor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;
    void timerCallback() override;

private:
    W2SamplerProcessor& proc;

    // ── File / library buttons ────────────────────────────────────────────────
    juce::TextButton loadFileButton   {"Load File"};
    juce::TextButton loadFolderButton {"Load Folder"};
    juce::TextButton prevButton       {L"\u25c4 Prev"};   // ◄
    juce::TextButton nextButton       {L"Next \u25ba"};   // ►
    juce::TextButton rndSampleButton  {"? Rnd Smpl"};
    juce::TextButton advanceModeButton{"Mode: Hold"};     // cycles modes
    juce::TextButton randomizeFXButton{"Randomize FX"};

    // ── Transport ─────────────────────────────────────────────────────────────
    juce::TextButton playButton       {L"\u25b6 Play"};

    // ── Voice row 1: Pitch + ADSR ─────────────────────────────────────────────
    juce::Slider pitchSlider, attackSlider, decaySlider, sustainSlider, releaseSlider;
    juce::Label  pitchLabel  {"", "Pitch"},   attackLabel {"", "Attack"},
                 decayLabel  {"", "Decay"},   sustainLabel{"", "Sustain"},
                 releaseLabel{"", "Release"};

    // ── Voice row 2: Filter + FX ──────────────────────────────────────────────
    juce::Slider filterFreqSlider, filterResSlider, distDriveSlider,
                 reverbMixSlider, reverbSizeSlider;
    juce::Label  filterFreqLabel{"", "Flt Freq"}, filterResLabel{"", "Flt Res"},
                 distDriveLabel {"", "Drive"},    reverbMixLabel{"", "Rvb Mix"},
                 reverbSizeLabel{"", "Rvb Size"};

    // ── Gain + Freeze ─────────────────────────────────────────────────────────
    juce::Slider       gainSlider;
    juce::Label        gainLabel {"", "Gain"};
    juce::ToggleButton freezeButton {"Freeze"};

    // ── Sequencer sliders ─────────────────────────────────────────────────────
    juce::Slider stepsSlider, hitsSlider, rotationSlider, rateSlider;
    juce::Label  stepsLabel {"", "Steps"}, hitsLabel   {"", "Hits"},
                 rotLabel   {"", "Rotat."}, rateLabel  {"", "Rate"};

    // ── Helpers ───────────────────────────────────────────────────────────────
    void syncAllSlidersFromParams();
    void updateAdvanceModeButton();
    void updatePlayButtonLabel();
    void drawWaveform   (juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawStepGrid   (juce::Graphics& g, juce::Rectangle<int> bounds);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerEditor)
};
