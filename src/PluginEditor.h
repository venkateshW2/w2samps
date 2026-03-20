#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "FuncGen.h"

//==============================================================================
// FuncGenCanvas — drawable Catmull-Rom curve editor for one FuncGen.
//
// Click anywhere on canvas   → add control point
// Drag a control point       → move it (sort order maintained)
// Double-click a point       → delete it
//==============================================================================
class FuncGenCanvas : public juce::Component
{
public:
    std::function<void()> onChange;

    void setFuncGen (FuncGen* fg) { funcGen_ = fg; repaint(); }
    FuncGen* getFuncGen() const   { return funcGen_; }

    /** Call from timer with current FG phase [0,1] — draws a vertical playhead. */
    void setPlayhead (float phase)
    {
        playheadPhase_ = phase;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b  = getLocalBounds();
        int  bw = b.getWidth(), bh = b.getHeight();

        // Dark background
        g.setColour (juce::Colour (0xff1C1C1E));
        g.fillRoundedRectangle (b.toFloat(), 3.0f);

        // Subtle grid (4 columns, 2 rows)
        g.setColour (juce::Colour (0xff3A3A3C));
        for (int i = 1; i < 4; ++i)
            g.drawVerticalLine   (b.getX() + bw * i / 4, (float) b.getY(), (float) b.getBottom());
        g.drawHorizontalLine (b.getCentreY(), (float) b.getX(), (float) b.getRight());

        if (funcGen_ == nullptr) { g.drawRect (b, 1); return; }

        // Curve (sample LUT across full width)
        juce::Path curve;
        bool started = false;
        for (int px = 0; px < bw; ++px)
        {
            float t   = (float) px / (float) std::max (1, bw - 1);
            float val = funcGen_->evaluate (t);
            float fy  = (float) b.getBottom() - val * (float) bh;
            if (!started) { curve.startNewSubPath ((float)(b.getX() + px), fy); started = true; }
            else          { curve.lineTo           ((float)(b.getX() + px), fy); }
        }
        g.setColour (juce::Colour (0xff30D158));  // kActive green
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        // Control points
        const auto& pts = funcGen_->getPoints();
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            float px = (float) b.getX() + pts[(size_t)i].x * (float) bw;
            float py = (float) b.getBottom() - pts[(size_t)i].y * (float) bh;
            bool sel = (i == selectedPt_);
            g.setColour (sel ? juce::Colours::white : juce::Colour (0xff30D158));
            g.fillEllipse (px - 5.f, py - 5.f, 10.f, 10.f);
            g.setColour (juce::Colour (0xff1C1C1E));
            g.drawEllipse (px - 5.f, py - 5.f, 10.f, 10.f, 1.0f);
        }

        // Playhead — vertical line at current phase position
        if (playheadPhase_ >= 0.f)
        {
            int phX = b.getX() + (int) (playheadPhase_ * (float) bw);
            g.setColour (juce::Colour (0xffFFD60A).withAlpha (0.75f));  // gold
            g.drawVerticalLine (phX, (float) b.getY(), (float) b.getBottom());
            // Small triangle indicator at top
            juce::Path tri;
            tri.addTriangle ((float) phX, (float) b.getY(),
                             (float) phX - 4.f, (float) b.getY() - 6.f,
                             (float) phX + 4.f, (float) b.getY() - 6.f);
            g.fillPath (tri);
        }

        g.setColour (juce::Colour (0xff636366));
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 3.0f, 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (funcGen_ == nullptr) return;
        selectedPt_ = findNearest (e.x, e.y);
        if (selectedPt_ == -1)
        {
            float nx = (float) e.x / (float) std::max (1, getWidth());
            float ny = 1.0f - (float) e.y / (float) std::max (1, getHeight());
            funcGen_->addPoint (nx, ny);
            selectedPt_ = findNearest (e.x, e.y);
            if (onChange) onChange();
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (funcGen_ == nullptr || selectedPt_ == -1) return;
        float nx = (float) e.x / (float) std::max (1, getWidth());
        float ny = 1.0f - (float) e.y / (float) std::max (1, getHeight());
        funcGen_->movePoint (selectedPt_, nx, ny);
        selectedPt_ = findNearest (e.x, e.y);
        if (onChange) onChange();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { selectedPt_ = -1; repaint(); }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (funcGen_ == nullptr) return;
        int idx = findNearest (e.x, e.y);
        if (idx != -1)
        {
            funcGen_->removePoint (idx);
            selectedPt_ = -1;
            if (onChange) onChange();
            repaint();
        }
    }

private:
    int findNearest (int mx, int my) const
    {
        if (funcGen_ == nullptr) return -1;
        constexpr int kR = 12;
        const auto& pts = funcGen_->getPoints();
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            int px = (int) (pts[(size_t)i].x * getWidth());
            int py = getHeight() - (int) (pts[(size_t)i].y * getHeight());
            int dx = mx - px, dy = my - py;
            if (dx*dx + dy*dy < kR*kR) return i;
        }
        return -1;
    }

    FuncGen* funcGen_      = nullptr;
    int      selectedPt_   = -1;
    float    playheadPhase_ = -1.f;  // -1 = hidden
};

//==============================================================================
// W2LookAndFeel — dark theme
//==============================================================================
class W2LookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Dark theme colours
    static constexpr uint32_t kBg       = 0xff1C1C1E;  // near-black bg
    static constexpr uint32_t kPanel    = 0xff2C2C2E;  // raised panel
    static constexpr uint32_t kElevated = 0xff3A3A3C;  // button bg
    static constexpr uint32_t kActive   = 0xff30D158;  // green highlight (on/active)
    static constexpr uint32_t kText     = 0xffF2F2F7;  // near-white text
    static constexpr uint32_t kTextDim  = 0xff8E8E93;  // dimmed text
    static constexpr uint32_t kTrack    = 0xff48484A;  // slider track bg
    static constexpr uint32_t kAccent   = 0xff636366;  // borders, separators
    static constexpr uint32_t kMute     = 0xffFF453A;  // mute red
    static constexpr uint32_t kSolo     = 0xffFFD60A;  // solo yellow
    // Per-voice accent colours
    static constexpr uint32_t kV0       = 0xff0A84FF;  // V1 blue
    static constexpr uint32_t kV1       = 0xffFF9F0A;  // V2 amber
    static constexpr uint32_t kV2       = 0xffBF5AF2;  // V3 purple

    W2LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour(kBg));
        setColour (juce::Label::textColourId,                 juce::Colour(kText));
        setColour (juce::Slider::backgroundColourId,          juce::Colour(kTrack));
        setColour (juce::Slider::trackColourId,               juce::Colour(kActive));
        setColour (juce::Slider::thumbColourId,               juce::Colour(kText));
        setColour (juce::TextButton::buttonColourId,          juce::Colour(kElevated));
        setColour (juce::TextButton::buttonOnColourId,        juce::Colour(kActive));
        setColour (juce::TextButton::textColourOffId,         juce::Colour(kText));
        setColour (juce::TextButton::textColourOnId,          juce::Colour(kBg));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        // BPM drag-number display — no track, no thumb
        if (slider.getName() == "bpm_drag")
        {
            auto b = juce::Rectangle<float> ((float)x, (float)y, (float)w, (float)h);
            g.setColour (juce::Colour (kElevated));
            g.fillRoundedRectangle (b, 5.0f);
            g.setColour (juce::Colour (kActive));
            g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);
            g.setColour (juce::Colour (kText));
            g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (15.0f)));
            g.drawText (juce::String ((int)slider.getValue()),
                        (int)b.getX(), (int)b.getY(), (int)b.getWidth(), (int)b.getHeight(),
                        juce::Justification::centred);
            return;
        }

        bool horiz = (style == juce::Slider::LinearHorizontal ||
                      style == juce::Slider::LinearBar);
        bool vert  = (style == juce::Slider::LinearVertical);

        const float trackH = 2.0f;
        float cx = x + w * 0.5f;
        float cy = y + h * 0.5f;

        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        if (horiz)
            g.fillRoundedRectangle ((float)x, cy - trackH/2, (float)w, trackH, trackH/2);
        else if (vert)
            g.fillRoundedRectangle (cx - trackH/2, (float)y, trackH, (float)h, trackH/2);

        g.setColour (slider.findColour (juce::Slider::trackColourId));
        if (horiz)
            g.fillRoundedRectangle ((float)x, cy - trackH/2, sliderPos - (float)x, trackH, trackH/2);
        else if (vert)
        {
            float fillH = (float)(y + h) - sliderPos;
            g.fillRoundedRectangle (cx - trackH/2, sliderPos, trackH, fillH, trackH/2);
        }

        const float thumbR = 5.0f;
        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        if (horiz)
            g.fillEllipse (sliderPos - thumbR, cy - thumbR, thumbR*2, thumbR*2);
        else if (vert)
            g.fillEllipse (cx - thumbR, sliderPos - thumbR, thumbR*2, thumbR*2);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& btn,
                                const juce::Colour& bgColour,
                                bool highlighted, bool down) override
    {
        auto b = btn.getLocalBounds().toFloat().reduced (0.5f);
        bool toggled = btn.getToggleState() || down;

        // bgColour is buttonColourId — check if app has set a custom fill
        bool hasFill = toggled
            || (bgColour.getARGB() != (uint32_t)kElevated
                && bgColour.getARGB() != (uint32_t)kBg
                && bgColour.getARGB() != (uint32_t)kPanel
                && bgColour.getAlpha() > 0);

        if (hasFill)
        {
            juce::Colour fillCol = toggled ? juce::Colour (kActive) : bgColour;
            g.setColour (fillCol);
            g.fillRoundedRectangle (b, 4.0f);
        }
        else
        {
            // Outline-only when inactive
            if (highlighted)
            {
                g.setColour (juce::Colour (kActive).withAlpha (0.10f));
                g.fillRoundedRectangle (b, 4.0f);
            }
            g.setColour (juce::Colour (highlighted ? kActive : kAccent).withAlpha (0.6f));
            g.drawRoundedRectangle (b, 4.0f, 1.0f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& btn,
                         bool /*highlighted*/, bool /*down*/) override
    {
        auto col = btn.findColour (juce::TextButton::buttonColourId);
        bool hasFill = btn.getToggleState()
            || (col.getARGB() != (uint32_t)kElevated
                && col.getARGB() != (uint32_t)kBg
                && col.getARGB() != (uint32_t)kPanel
                && col.getAlpha() > 0);

        // On a filled button use dark text, on outline use bright text
        g.setColour (hasFill ? juce::Colour (kBg) : juce::Colour (kText));
        g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (10.5f)));
        g.drawText (btn.getButtonText(), btn.getLocalBounds().reduced (2, 0),
                    juce::Justification::centred, true);
    }

    juce::Font getLabelFont (juce::Label&) override
    {
        return juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(11.0f));
    }
};

//==============================================================================
/**
 * W2SamplerEditor — resizable (940×720 default), dark theme.
 *
 * Layout:
 *   y=0..44         Transport bar (full width)
 *   y=44..H-90      Content
 *     x=0..320      Left column — voice selector + accordion voice controls
 *     x=320..W-120  Center — concentric rings
 *     x=W-120..W    Right column — master meter + gain fader
 *   y=H-90..H       Bottom bar — FX/Presets for selected voice
 */
class W2SamplerEditor : public juce::AudioProcessorEditor,
                        public juce::Timer
{
public:
    explicit W2SamplerEditor (W2SamplerProcessor&);
    ~W2SamplerEditor() override;

    void paint    (juce::Graphics&) override;
    void resized  () override;
    void timerCallback() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    W2SamplerProcessor& proc;

    // LookAndFeel instance — must outlive all child components
    W2LookAndFeel laf_;

    // Mirror LookAndFeel colour constants as local aliases
    static constexpr uint32_t kBg       = W2LookAndFeel::kBg;
    static constexpr uint32_t kPanel    = W2LookAndFeel::kPanel;
    static constexpr uint32_t kElevated = W2LookAndFeel::kElevated;
    static constexpr uint32_t kActive   = W2LookAndFeel::kActive;
    static constexpr uint32_t kText     = W2LookAndFeel::kText;
    static constexpr uint32_t kTextDim  = W2LookAndFeel::kTextDim;
    static constexpr uint32_t kTrack    = W2LookAndFeel::kTrack;
    static constexpr uint32_t kAccent   = W2LookAndFeel::kAccent;
    static constexpr uint32_t kMute     = W2LookAndFeel::kMute;
    static constexpr uint32_t kSolo     = W2LookAndFeel::kSolo;
    static constexpr uint32_t kV0       = W2LookAndFeel::kV0;
    static constexpr uint32_t kV1       = W2LookAndFeel::kV1;
    static constexpr uint32_t kV2       = W2LookAndFeel::kV2;

    // Fixed layout constants
    static constexpr int kTransportH = 38;
    static constexpr int kLeftW      = 320;
    static constexpr int kRightW     = 120;
    static constexpr int kBottomH    = 90;

    // Selected voice
    int selectedVoice = 0;

    // Ring geometry (computed in resized)
    float ringCX_ = 0.0f, ringCY_ = 0.0f;
    float ringR_[3] = { 175.0f, 120.0f, 65.0f };

    // Scrollable left panel
    juce::Viewport   leftViewport_;
    juce::Component  leftContent_;

    //==========================================================================
    // Transport bar (y=0..38)
    juce::TextButton  playBtn    { "Play" };
    juce::Label       bpmLabel   { "", "BPM" };
    juce::Slider      bpmSlider;
    static constexpr int kNumClkDivs = 4;
    static constexpr int kClkDivVals[kNumClkDivs] = { 1, 2, 4, 8 };
    juce::TextButton  clkDivBtns[kNumClkDivs];
    juce::TextButton  muteBtn[3];
    juce::TextButton  soloBtn[3];

    // Voice selector row (top of left column)
    juce::TextButton  voiceSelectBtn[3];

    // Right column master controls
    juce::Rectangle<int> masterColumnRect_;
    juce::Slider      masterGainSlider;

    //==========================================================================
    // Per-voice UI (all three built; only selectedVoice shown in left panel)
    struct VoiceUI
    {
        // ── Section accordion (left panel: SAMPLE/SEQ/PHASE/SOUND/MOD; FX/PRESETS bottom) ──
        // 0=SAMPLE 1=SEQUENCE 2=PHASE 3=SOUND 4=FX/PRESETS(bottom) 5=MODULATION
        juce::TextButton sectionBtn[6];
        bool sectionOpen[6] = { true, true, false, false, false, false };

        // Nav row
        juce::TextButton loadBtn  { "Load" };
        juce::TextButton prevBtn  { "< Prev" };
        juce::TextButton nextBtn  { "Next >" };
        juce::TextButton rndBtn   { "Rnd" };
        juce::Label      nameLabel;

        // Onset sensitivity
        juce::Slider     onsetSensSlider;
        juce::Label      onsetSensLabel { "", "Sens" };

        // Waveform
        WaveformDisplay  waveform;

        // Sequence section
        juce::Slider     stepsSlider, hitsSlider, rotSlider;
        juce::Label      stepsLabel { "", "Steps" };
        juce::Label      hitsLabel  { "", "Hits" };
        juce::Label      rotLabel   { "", "Rot" };
        juce::TextButton smpAdvBtn  { "Hold" };
        juce::TextButton loopModeBtn { "Off" };
        juce::Slider     loopMsSlider;
        juce::Label      loopMsLabel { "", "Loop ms" };
        juce::TextButton loopLockBtn { "Lock" };
        juce::TextButton freezeBtn   { "Freeze" };

        // Phase section
        juce::Slider     offsetSlider, warpSlider, quantSlider;
        juce::Label      offsetLabel { "", "Offset" };
        juce::Label      warpLabel   { "", "Warp" };
        juce::Label      quantLabel  { "", "Grid" };
        juce::TextButton revBtn      { "Rev: Off" };
        juce::TextButton phaseSrcBtn { "Master" };
        juce::TextButton ratePresetBtns[kNumRatePresets];
        juce::Slider     rateSlider;
        juce::Label      rateLabel { "", "Rate" };

        // Sound section (linear sliders)
        juce::Slider     pitchSlider, attSlider, decSlider, susSlider, relSlider;
        juce::Label      pitchLabel { "", "Pitch" };
        juce::Label      attLabel   { "", "Attack" };
        juce::Label      decLabel   { "", "Decay" };
        juce::Label      susLabel   { "", "Sustain" };
        juce::Label      relLabel   { "", "Release" };
        juce::Slider     fFreqSlider, fResSlider, driveSlider, rvbMixSlider, rvbSzSlider;
        juce::Label      fFreqLabel  { "", "Filter Hz" };
        juce::Label      fResLabel   { "", "Filter Q" };
        juce::Label      driveLabel  { "", "Drive" };
        juce::Label      rvbMixLabel { "", "Rvb Mix" };
        juce::Label      rvbSzLabel  { "", "Rvb Size" };
        juce::Slider     gainSlider, preGainSlider, limitSlider;
        juce::Label      gainLabel     { "", "Level" };
        juce::Label      preGainLabel  { "", "Pre Gain" };
        juce::Label      limitLabel    { "", "Limit dB" };

        // ── Modulation section (section 5, left panel) ───────────────────────
        static constexpr int kNumFg = 4;
        FuncGenCanvas    fgCanvas[kNumFg];
        juce::TextButton fgSyncBtn[kNumFg];   // SYNC / FREE toggle
        juce::Slider     fgRateSlider[kNumFg]; // continuous rate (mult or Hz)
        juce::Label      fgRateLabel[kNumFg];
        juce::ComboBox   fgDestBox[kNumFg];    // dropdown for mod destination
        juce::Slider     fgDepthSlider[kNumFg];
        juce::Slider     fgMinSlider[kNumFg];
        juce::Slider     fgMaxSlider[kNumFg];
        juce::Label      fgDepthLabel[kNumFg];
        juce::Label      fgMinLabel[kNumFg];
        juce::Label      fgMaxLabel[kNumFg];

        // ── Mod indicator bars (one per ModDest, shown below SOUND sliders) ──
        // A thin accent-coloured strip below each modulated slider.
        // Width = destModNorm × slider width. Hidden when no FG targets that dest.
        struct ModBar : juce::Component
        {
            float normValue = 0.f;
            juce::Colour barColour;
            void paint (juce::Graphics& g) override
            {
                if (normValue < 0.001f) return;
                g.setColour (barColour.withAlpha (0.85f));
                g.fillRect (0, 0, (int)(normValue * (float)getWidth()), getHeight());
                // Dim fill for remaining range
                g.setColour (barColour.withAlpha (0.15f));
                g.fillRect ((int)(normValue * (float)getWidth()), 0,
                            getWidth() - (int)(normValue * (float)getWidth()), getHeight());
            }
        };
        ModBar destModBars[kNumModDests];

        // FX/Presets (shown in bottom bar)
        bool             rndLocked[10] = {};
        juce::TextButton rndLockBtns[10];
        juce::Slider     rndFxSlider;
        juce::Label      rndFxLabel  { "", "Rnd FX%" };
        juce::TextButton rndFxFireBtn { "Rnd FX Now" };
        juce::TextButton resetFxBtn   { "Reset FX" };
        juce::TextButton presetBtns[8];
        juce::TextButton presetSaveBtn { "Save" };
        bool             presetSaveMode = false;
    };

    VoiceUI voiceUI[3];

    std::shared_ptr<juce::FileChooser> fileChooser_;

    //==========================================================================
    void buildTransportBar();
    void buildVoiceUI   (int v);
    void layoutTransportBar();
    void layoutMasterColumn();
    void layoutVoicePanel   (int v);
    void layoutBottomBar    (int v);
    void hideVoiceAll       ();

    void styleSlider    (juce::Slider& s, float mn, float mx, float def, bool isInt = false);
    void styleButton    (juce::TextButton& b);
    void styleLabel     (juce::Label& l, bool bright = false);

    void syncVoiceFromParams  (int v);
    void updateCycleBtns      (int v);

    void drawRings         (juce::Graphics& g);
    void drawMasterColumn  (juce::Graphics& g);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerEditor)
};
