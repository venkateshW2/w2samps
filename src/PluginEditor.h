#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"

/**
 * W2SamplerEditor — 820 × 620.
 *
 * Layout (always-visible rows above tab content):
 *   y=0..28    Tab bar: [Master] [Voice 1] [Voice 2] [Voice 3]
 *   y=28..60   Global bar: [▶ Play] [BPM slider] [Cycle: 1b 2b 4b 8b]
 *                           [V1: M S] [V2: M S] [V3: M S]
 *
 * Master tab (y=60..):
 *   Three phasor circles
 *
 * Voice tab (y=60..):
 *   Row 0  28px  — [Load] [◄] [►] [Rnd]  sample name / onset info
 *   Row 1 160px  — WaveformDisplay (region + loop handles, playhead, length overlay)
 *   Row 2  44px  — [phasor] [Clock Src btn] [÷8..×8 rate presets] [Rate fine]
 *   Row 3  32px  — Offset | Warp | Quant | [Reverse]
 *   Row 4  32px  — Steps | Hits | Rotation | [SmpAdv]
 *   Row 5  24px  — euclidean step grid (drawn)
 *   Row 6  32px  — Pitch | Attack | Decay | Sustain | Release
 *   Row 7  32px  — FltFreq | FltRes | Drive | RvbMix | RvbSize
 *   Row 8  32px  — Gain | [Freeze] | LoopMode | LoopSizeMs | [Lock]
 *   Row 9  32px  — RndFX% | [Randomize FX Now]
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

    // ── Colours (white/grey/black monochrome theme) ───────────────────────────
    static constexpr uint32_t kBg       = 0xffF5F5F5;  // near white
    static constexpr uint32_t kPanel    = 0xffE8E8E8;  // light grey panel (button off)
    static constexpr uint32_t kActive   = 0xffB8D4B8;  // light sage green (button on/selected)
    static constexpr uint32_t kAccent   = 0xff222222;  // near black (track fill, borders)
    static constexpr uint32_t kTrack    = 0xffCCCCCC;  // slider track
    static constexpr uint32_t kThumb    = 0xff333333;  // slider thumb
    static constexpr uint32_t kText     = 0xff111111;  // near black text
    static constexpr uint32_t kTextDim  = 0xff777777;  // dimmed text
    static constexpr uint32_t kMute     = 0xffE8B0B0;  // light red (muted button)
    static constexpr uint32_t kSolo     = 0xffA8C8E8;  // light blue (soloed button)

    //==========================================================================
    // Always-visible tab bar
    static constexpr int kNumTabs = 4;
    int activeTab = 0;
    juce::TextButton tabButtons[kNumTabs];

    // Always-visible global bar (y=28..60)
    juce::TextButton  playBtn    { "Play" };
    juce::Label       bpmLabel   { "", "BPM" };
    juce::Slider      bpmSlider;
    // Clock cycle division: 1b 2b 4b 8b → 1/2/4/8 beats per cycle
    static constexpr int kNumClkDivs = 4;
    static constexpr int kClkDivVals[kNumClkDivs] = { 1, 2, 4, 8 };
    juce::TextButton  clkDivBtns[kNumClkDivs];
    juce::Label       clkDivLabel { "", "Cycle" };
    // Mute / Solo per voice
    juce::TextButton  muteBtn[3];
    juce::TextButton  soloBtn[3];

    //==========================================================================
    // Master tab
    juce::Rectangle<int> masterPhasorArea_;

    //==========================================================================
    // Per-voice UI
    struct VoiceUI
    {
        // Nav row
        juce::TextButton loadBtn  { "Load Folder" };
        juce::TextButton prevBtn  { "< Prev" };
        juce::TextButton nextBtn  { "Next >" };
        juce::TextButton rndBtn   { "Rnd" };
        juce::Label      nameLabel;

        // Waveform
        WaveformDisplay  waveform;

        // Clock row
        //   phasorRect drawn in paint()
        juce::TextButton phaseSrcBtn { "Master" };  // cycles: Master / Sync V1 / Sync V2 / Sync V3
        juce::TextButton ratePresetBtns[kNumRatePresets];
        juce::Slider     rateSlider;
        juce::Label      rateLabel { "", "Rate" };

        // Phase transform row
        juce::Slider     offsetSlider, warpSlider, quantSlider;
        juce::Label      offsetLabel { "", "Offset" };
        juce::Label      warpLabel   { "", "Warp" };
        juce::Label      quantLabel  { "", "Quant" };
        juce::TextButton revBtn      { "Rev: Off" };

        // Seq row
        juce::Slider     stepsSlider, hitsSlider, rotSlider;
        juce::Label      stepsLabel { "", "Steps" };
        juce::Label      hitsLabel  { "", "Hits" };
        juce::Label      rotLabel   { "", "Rot" };
        juce::TextButton smpAdvBtn  { "Hold" };

        // ADSR row
        juce::Slider     pitchSlider, attSlider, decSlider, susSlider, relSlider;
        juce::Label      pitchLabel { "", "Pitch" };
        juce::Label      attLabel   { "", "Atk" };
        juce::Label      decLabel   { "", "Dec" };
        juce::Label      susLabel   { "", "Sus" };
        juce::Label      relLabel   { "", "Rel" };

        // FX row
        juce::Slider     fFreqSlider, fResSlider, driveSlider, rvbMixSlider, rvbSzSlider;
        juce::Label      fFreqLabel { "", "FltFreq" };
        juce::Label      fResLabel  { "", "FltRes" };
        juce::Label      driveLabel { "", "Drive" };
        juce::Label      rvbMixLabel{ "", "RvbMix" };
        juce::Label      rvbSzLabel { "", "RvbSz" };

        // Misc row
        juce::Slider       gainSlider, loopMsSlider;
        juce::Label        gainLabel  { "", "Gain" };
        juce::Label        loopMsLabel{ "", "Loop ms" };
        juce::TextButton   freezeBtn  { "Freeze" };
        juce::TextButton   loopModeBtn{ "Off" };
        juce::TextButton   loopLockBtn { "Lock" };

        // RndFX row
        juce::Slider     rndFxSlider;
        juce::Label      rndFxLabel { "", "Rnd FX%" };
        juce::TextButton rndFxFireBtn { "Rnd FX Now" };

        // Paint-drawn rects
        juce::Rectangle<int> phasorRect;
        juce::Rectangle<int> gridRect;
    };

    VoiceUI voiceUI[3];

    std::shared_ptr<juce::FileChooser> fileChooser_;

    //==========================================================================
    void buildGlobalBar();
    void buildVoiceUI   (int v);
    void layoutGlobalBar();
    void layoutMasterTab();
    void layoutVoiceTab (int v);
    void hideVoiceAll   ();

    void styleSlider    (juce::Slider& s, float mn, float mx, float def, bool isInt = false);
    void styleButton    (juce::TextButton& b);
    void styleLabel     (juce::Label& l, bool bright = false);

    void syncVoiceFromParams (int v);
    void updateCycleBtns     (int v);
    void drawMasterTab  (juce::Graphics& g);
    void drawStepGrid   (juce::Graphics& g, juce::Rectangle<int> b, int v);
    void drawPhasor     (juce::Graphics& g, juce::Rectangle<int> b, int v, juce::Colour col);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerEditor)
};
