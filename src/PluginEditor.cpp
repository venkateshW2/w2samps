#include "PluginEditor.h"

//==============================================================================
// Style helpers
//==============================================================================
void W2SamplerEditor::styleSlider (juce::Slider& s, float mn, float mx, float def, bool isInt)
{
    s.setRange ((double)mn, (double)mx, isInt ? 1.0 : 0.0);
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    s.setPopupDisplayEnabled (true, true, this);
    s.setValue ((double)def, juce::dontSendNotification);
    s.setColour (juce::Slider::backgroundColourId,        juce::Colour (kTrack));
    s.setColour (juce::Slider::trackColourId,             juce::Colour (kActive));
    s.setColour (juce::Slider::thumbColourId,             juce::Colour (kText));
    s.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (kText));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (kPanel));
    s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (kTrack));
}

void W2SamplerEditor::styleButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (kElevated));
    b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kActive));
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kText));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kBg));
}

void W2SamplerEditor::styleLabel (juce::Label& l, bool bright)
{
    l.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (11.0f)));
    l.setColour (juce::Label::textColourId,
                 juce::Colour (bright ? kText : kTextDim));
    l.setJustificationType (juce::Justification::centredRight);
}

//==============================================================================
// Constructor
//==============================================================================
W2SamplerEditor::W2SamplerEditor (W2SamplerProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf_);
    setSize (940, 720);
    setResizable (true, true);
    setResizeLimits (700, 500, 1600, 1200);

    // Set up scrollable left panel
    leftContent_.setOpaque (false);
    leftViewport_.setViewedComponent (&leftContent_, false);
    leftViewport_.setScrollBarsShown (true, false);
    addAndMakeVisible (leftViewport_);

    buildTransportBar();

    // Voice selector buttons — direct editor children (above viewport)
    {
        const char* vLabels[] = { "V1", "V2", "V3" };
        for (int v = 0; v < 3; ++v)
        {
            voiceSelectBtn[v].setButtonText (vLabels[v]);
            voiceSelectBtn[v].setClickingTogglesState (false);
            styleButton (voiceSelectBtn[v]);
            voiceSelectBtn[v].onClick = [this, v] {
                selectedVoice = v;
                resized();
                repaint();
            };
            addAndMakeVisible (voiceSelectBtn[v]);
        }
    }

    // Master gain slider (right column)
    {
        styleSlider (masterGainSlider, 0.0f, 2.0f, proc.masterGain ? proc.masterGain->get() : 0.7f);
        masterGainSlider.setDoubleClickReturnValue (true, 0.7);
        masterGainSlider.setSliderStyle (juce::Slider::LinearVertical);
        masterGainSlider.onValueChange = [this] {
            if (proc.masterGain) *proc.masterGain = (float)masterGainSlider.getValue();
        };
        addAndMakeVisible (masterGainSlider);
    }

    for (int v = 0; v < 3; ++v)
        buildVoiceUI (v);

    startTimerHz (20);
    resized();
}

W2SamplerEditor::~W2SamplerEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
// buildTransportBar
//==============================================================================
void W2SamplerEditor::buildTransportBar()
{
    styleButton (playBtn);
    playBtn.onClick = [this] {
        bool now = !proc.getPlaying();
        proc.setPlaying (now);
    };
    addAndMakeVisible (playBtn);

    styleLabel (bpmLabel, true);
    bpmLabel.setText ("BPM", juce::dontSendNotification);
    addAndMakeVisible (bpmLabel);

    // BPM as drag-number display
    styleSlider (bpmSlider, 20.0f, 999.0f, proc.bpm->get());
    bpmSlider.setName ("bpm_drag");
    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    bpmSlider.setPopupDisplayEnabled (false, false, nullptr);
    bpmSlider.onValueChange = [this] { *proc.bpm = (float)bpmSlider.getValue(); };
    addAndMakeVisible (bpmSlider);

    const char* divNames[] = { "1 Beat", "2 Beats", "4=Bar", "8=2Bar" };
    for (int i = 0; i < kNumClkDivs; ++i)
    {
        clkDivBtns[i].setButtonText (divNames[i]);
        styleButton (clkDivBtns[i]);
        int val = kClkDivVals[i];
        clkDivBtns[i].onClick = [this, val] {
            *proc.clkDiv = val;
            for (int j = 0; j < kNumClkDivs; ++j)
                clkDivBtns[j].setColour (juce::TextButton::buttonColourId,
                    juce::Colour (kClkDivVals[j] == val ? kActive : kElevated));
        };
        addAndMakeVisible (clkDivBtns[i]);
    }

    // Mute/Solo buttons — built here, positioned in master column
    const char* vn[] = { "V1", "V2", "V3" };
    for (int v = 0; v < 3; ++v)
    {
        muteBtn[v].setButtonText (juce::String (vn[v]) + " M");
        styleButton (muteBtn[v]);
        muteBtn[v].onClick = [this, v] {
            bool m = !proc.getVoiceMuted (v);
            proc.setVoiceMute (v, m);
            muteBtn[v].setColour (juce::TextButton::buttonColourId,
                                  juce::Colour (m ? kMute : kElevated));
        };
        addAndMakeVisible (muteBtn[v]);

        soloBtn[v].setButtonText (juce::String (vn[v]) + " S");
        styleButton (soloBtn[v]);
        soloBtn[v].onClick = [this, v] {
            proc.setVoiceSolo (v);
            int s = proc.getSoloVoice();
            for (int j = 0; j < 3; ++j)
                soloBtn[j].setColour (juce::TextButton::buttonColourId,
                    juce::Colour ((s == j) ? kSolo : kElevated));
        };
        addAndMakeVisible (soloBtn[v]);
    }
}

//==============================================================================
// buildVoiceUI
//==============================================================================
void W2SamplerEditor::buildVoiceUI (int v)
{
    auto& ui = voiceUI[v];
    auto& p  = proc.vp[v];

    // Section header buttons — children of leftContent_
    static const char* secNames[] = { "SAMPLE", "SEQUENCE", "PHASE", "SOUND", "FX / PRESETS", "MODULATION" };
    for (int s = 0; s < 6; ++s)
    {
        ui.sectionBtn[s].setButtonText ((ui.sectionOpen[s] ? juce::String (L"\u25BC ") : juce::String (L"\u25BA ")) + secNames[s]);
        styleButton (ui.sectionBtn[s]);
        ui.sectionBtn[s].onClick = [this, v, s] {
            voiceUI[v].sectionOpen[s] = !voiceUI[v].sectionOpen[s];
            const char* names[] = { "SAMPLE", "SEQUENCE", "PHASE", "SOUND", "FX / PRESETS", "MODULATION" };
            voiceUI[v].sectionBtn[s].setButtonText (
                (voiceUI[v].sectionOpen[s] ? juce::String (L"\u25BC ") : juce::String (L"\u25BA ")) + names[s]);
            resized();
        };
        if (s != 4) leftContent_.addAndMakeVisible (ui.sectionBtn[s]);  // 4=FX bottom bar
    }

    // Nav
    styleButton (ui.loadBtn);
    styleButton (ui.prevBtn);
    styleButton (ui.nextBtn);
    styleButton (ui.rndBtn);
    ui.nameLabel.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (10.0f)));
    ui.nameLabel.setColour (juce::Label::textColourId, juce::Colour (kTextDim));
    ui.nameLabel.setJustificationType (juce::Justification::centredLeft);

    ui.loadBtn.onClick = [this, v] {
        fileChooser_ = std::make_shared<juce::FileChooser> ("Load folder", juce::File{}, "");
        fileChooser_->launchAsync (
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
            [this, v] (const juce::FileChooser& fc) {
                auto r = fc.getResult();
                if (r.isDirectory()) proc.loadFolder (v, r);
            });
    };
    ui.prevBtn.onClick = [this, v] { proc.prevSample (v); };
    ui.nextBtn.onClick = [this, v] { proc.nextSample (v); };
    ui.rndBtn.onClick  = [this, v] { proc.randomSample (v); };

    leftContent_.addAndMakeVisible (ui.loadBtn);  leftContent_.addAndMakeVisible (ui.prevBtn);
    leftContent_.addAndMakeVisible (ui.nextBtn);  leftContent_.addAndMakeVisible (ui.rndBtn);
    leftContent_.addAndMakeVisible (ui.nameLabel);

    // Onset sensitivity
    styleSlider (ui.onsetSensSlider, 0.0f, 1.0f, 0.5f);
    ui.onsetSensSlider.setDoubleClickReturnValue (true, 0.5);
    styleLabel  (ui.onsetSensLabel);
    ui.onsetSensSlider.onDragEnd = [this, v] {
        float s = (float) voiceUI[v].onsetSensSlider.getValue();
        proc.reanalyseOnsets (v, s);
    };
    leftContent_.addAndMakeVisible (ui.onsetSensSlider);
    leftContent_.addAndMakeVisible (ui.onsetSensLabel);

    // Waveform
    ui.waveform.onRegionStart = [this,v](float val) { *proc.vp[v].regionStart = val; };
    ui.waveform.onRegionEnd   = [this,v](float val) { *proc.vp[v].regionEnd   = val; };
    ui.waveform.onLoopStart   = [this,v](float val) { *proc.vp[v].loopStart   = val; };
    ui.waveform.onLoopEnd     = [this,v](float val) { *proc.vp[v].loopEnd     = val; };
    leftContent_.addAndMakeVisible (ui.waveform);

    // Sequence section
    styleSlider (ui.stepsSlider, 1, 32, (float)p.seqSteps->get(), true);
    styleSlider (ui.hitsSlider,  0, 32, (float)p.seqHits->get(),  true);
    styleSlider (ui.rotSlider,   0, 31, (float)p.seqRotation->get(), true);
    ui.stepsSlider.setDoubleClickReturnValue (true, 16.0);
    ui.hitsSlider .setDoubleClickReturnValue (true,  4.0);
    ui.rotSlider  .setDoubleClickReturnValue (true,  0.0);
    styleLabel  (ui.stepsLabel); styleLabel (ui.hitsLabel); styleLabel (ui.rotLabel);
    ui.stepsSlider.onValueChange = [this,v] { *proc.vp[v].seqSteps    = (int)voiceUI[v].stepsSlider.getValue(); };
    ui.hitsSlider.onValueChange  = [this,v] { *proc.vp[v].seqHits     = (int)voiceUI[v].hitsSlider.getValue(); };
    ui.rotSlider.onValueChange   = [this,v] { *proc.vp[v].seqRotation = (int)voiceUI[v].rotSlider.getValue(); };
    leftContent_.addAndMakeVisible (ui.stepsSlider); leftContent_.addAndMakeVisible (ui.stepsLabel);
    leftContent_.addAndMakeVisible (ui.hitsSlider);  leftContent_.addAndMakeVisible (ui.hitsLabel);
    leftContent_.addAndMakeVisible (ui.rotSlider);   leftContent_.addAndMakeVisible (ui.rotLabel);

    styleButton (ui.smpAdvBtn);
    ui.smpAdvBtn.onClick = [this,v] {
        *proc.vp[v].sampleAdv = (proc.vp[v].sampleAdv->get() + 1) % 3;
        updateCycleBtns (v);
    };
    leftContent_.addAndMakeVisible (ui.smpAdvBtn);

    styleSlider (ui.loopMsSlider, 5.0f, 5000.0f, p.loopSizeMs->get());
    ui.loopMsSlider.setDoubleClickReturnValue (true, 100.0);
    styleLabel  (ui.loopMsLabel);
    ui.loopMsSlider.onValueChange = [this,v] { *proc.vp[v].loopSizeMs = (float)voiceUI[v].loopMsSlider.getValue(); };
    leftContent_.addAndMakeVisible (ui.loopMsSlider); leftContent_.addAndMakeVisible (ui.loopMsLabel);

    styleButton (ui.freezeBtn);
    ui.freezeBtn.setClickingTogglesState (true);
    ui.freezeBtn.onClick = [this,v] {
        bool on = voiceUI[v].freezeBtn.getToggleState();
        *proc.vp[v].reverbFreeze = (int)on;
        voiceUI[v].freezeBtn.setColour (juce::TextButton::buttonColourId,
                                        juce::Colour (on ? kActive : kElevated));
    };
    leftContent_.addAndMakeVisible (ui.freezeBtn);

    styleButton (ui.loopModeBtn);
    ui.loopModeBtn.onClick = [this,v] {
        *proc.vp[v].loopMode = (proc.vp[v].loopMode->get() + 1) % 6;
        updateCycleBtns (v);
    };
    leftContent_.addAndMakeVisible (ui.loopModeBtn);

    styleButton (ui.loopLockBtn);
    ui.loopLockBtn.setClickingTogglesState (true);
    ui.loopLockBtn.onClick = [this,v] {
        bool on = voiceUI[v].loopLockBtn.getToggleState();
        *proc.vp[v].loopSizeLock = (int)on;
        voiceUI[v].loopLockBtn.setColour (juce::TextButton::buttonColourId,
                                          juce::Colour (on ? kActive : kElevated));
    };
    leftContent_.addAndMakeVisible (ui.loopLockBtn);

    // Phase section
    styleSlider (ui.offsetSlider, 0.0f,  1.0f, p.phaseOffset->get());
    styleSlider (ui.warpSlider,  -1.0f,  1.0f, p.warp->get());
    styleSlider (ui.quantSlider,  0.0f,  1.0f, p.quantiseAmt->get());
    ui.offsetSlider.setDoubleClickReturnValue (true, 0.0);
    ui.warpSlider  .setDoubleClickReturnValue (true, 0.0);
    ui.quantSlider .setDoubleClickReturnValue (true, 0.0);
    styleLabel  (ui.offsetLabel); styleLabel (ui.warpLabel); styleLabel (ui.quantLabel);
    ui.offsetSlider.onValueChange = [this,v] { *proc.vp[v].phaseOffset = (float)voiceUI[v].offsetSlider.getValue(); };
    ui.warpSlider.onValueChange   = [this,v] { *proc.vp[v].warp        = (float)voiceUI[v].warpSlider.getValue(); };
    ui.quantSlider.onValueChange  = [this,v] { *proc.vp[v].quantiseAmt = (float)voiceUI[v].quantSlider.getValue(); };
    leftContent_.addAndMakeVisible (ui.offsetSlider); leftContent_.addAndMakeVisible (ui.offsetLabel);
    leftContent_.addAndMakeVisible (ui.warpSlider);   leftContent_.addAndMakeVisible (ui.warpLabel);
    leftContent_.addAndMakeVisible (ui.quantSlider);  leftContent_.addAndMakeVisible (ui.quantLabel);

    styleButton (ui.revBtn);
    ui.revBtn.onClick = [this,v] {
        *proc.vp[v].reverse = !proc.vp[v].reverse->get();
        updateCycleBtns (v);
    };
    leftContent_.addAndMakeVisible (ui.revBtn);

    // Phase source
    styleButton (ui.phaseSrcBtn);
    ui.phaseSrcBtn.onClick = [this,v] {
        *proc.vp[v].phaseSource = (proc.vp[v].phaseSource->get() + 1) % 4;
        updateCycleBtns (v);
    };
    leftContent_.addAndMakeVisible (ui.phaseSrcBtn);

    // Rate presets
    for (int i = 0; i < kNumRatePresets; ++i)
    {
        ui.ratePresetBtns[i].setButtonText (kRatePresets[i].name);
        styleButton (ui.ratePresetBtns[i]);
        float val = kRatePresets[i].value;
        ui.ratePresetBtns[i].onClick = [this, v, val] {
            *proc.vp[v].rate = val;
            voiceUI[v].rateSlider.setValue ((double)val, juce::dontSendNotification);
            updateCycleBtns (v);
        };
        leftContent_.addAndMakeVisible (ui.ratePresetBtns[i]);
    }

    styleSlider (ui.rateSlider, 0.125f, 8.0f, p.rate->get());
    ui.rateSlider.setDoubleClickReturnValue (true, 1.0);
    styleLabel  (ui.rateLabel, false);
    ui.rateSlider.onValueChange = [this,v] {
        *proc.vp[v].rate = (float)voiceUI[v].rateSlider.getValue();
        updateCycleBtns (v);
    };
    leftContent_.addAndMakeVisible (ui.rateSlider);  leftContent_.addAndMakeVisible (ui.rateLabel);

    // Sound section — ADSR
    styleSlider (ui.pitchSlider, -24.0f, 24.0f, p.pitch->get());
    styleSlider (ui.attSlider,   0.001f,  2.0f, p.attack->get());
    styleSlider (ui.decSlider,   0.001f,  2.0f, p.decay->get());
    styleSlider (ui.susSlider,     0.0f,  1.0f, p.sustain->get());
    styleSlider (ui.relSlider,   0.001f,  4.0f, p.release->get());
    ui.pitchSlider.setDoubleClickReturnValue (true,  0.0);
    ui.attSlider  .setDoubleClickReturnValue (true, 0.01);
    ui.decSlider  .setDoubleClickReturnValue (true,  0.1);
    ui.susSlider  .setDoubleClickReturnValue (true,  0.8);
    ui.relSlider  .setDoubleClickReturnValue (true,  0.1);
    styleLabel (ui.pitchLabel); styleLabel (ui.attLabel); styleLabel (ui.decLabel);
    styleLabel (ui.susLabel);   styleLabel (ui.relLabel);
    ui.pitchSlider.onValueChange = [this,v] { *proc.vp[v].pitch   = (float)voiceUI[v].pitchSlider.getValue(); };
    ui.attSlider.onValueChange   = [this,v] { *proc.vp[v].attack  = (float)voiceUI[v].attSlider.getValue(); };
    ui.decSlider.onValueChange   = [this,v] { *proc.vp[v].decay   = (float)voiceUI[v].decSlider.getValue(); };
    ui.susSlider.onValueChange   = [this,v] { *proc.vp[v].sustain = (float)voiceUI[v].susSlider.getValue(); };
    ui.relSlider.onValueChange   = [this,v] { *proc.vp[v].release = (float)voiceUI[v].relSlider.getValue(); };
    leftContent_.addAndMakeVisible (ui.pitchSlider); leftContent_.addAndMakeVisible (ui.pitchLabel);
    leftContent_.addAndMakeVisible (ui.attSlider);   leftContent_.addAndMakeVisible (ui.attLabel);
    leftContent_.addAndMakeVisible (ui.decSlider);   leftContent_.addAndMakeVisible (ui.decLabel);
    leftContent_.addAndMakeVisible (ui.susSlider);   leftContent_.addAndMakeVisible (ui.susLabel);
    leftContent_.addAndMakeVisible (ui.relSlider);   leftContent_.addAndMakeVisible (ui.relLabel);

    // Sound section — FX
    styleSlider (ui.fFreqSlider,  20.0f, 20000.0f, p.filterFreq->get());
    styleSlider (ui.fResSlider,    0.5f,    10.0f, p.filterRes->get());
    styleSlider (ui.driveSlider,   0.0f,     1.0f, p.distDrive->get());
    styleSlider (ui.rvbMixSlider,  0.0f,     1.0f, p.reverbMix->get());
    styleSlider (ui.rvbSzSlider,   0.0f,     1.0f, p.reverbSize->get());
    ui.fFreqSlider .setDoubleClickReturnValue (true, 20000.0);
    ui.fResSlider  .setDoubleClickReturnValue (true,    0.7);
    ui.driveSlider .setDoubleClickReturnValue (true,    0.0);
    ui.rvbMixSlider.setDoubleClickReturnValue (true,    0.0);
    ui.rvbSzSlider .setDoubleClickReturnValue (true,    0.5);
    styleLabel (ui.fFreqLabel); styleLabel (ui.fResLabel); styleLabel (ui.driveLabel);
    styleLabel (ui.rvbMixLabel); styleLabel (ui.rvbSzLabel);
    ui.fFreqSlider.onValueChange  = [this,v] { *proc.vp[v].filterFreq = (float)voiceUI[v].fFreqSlider.getValue(); };
    ui.fResSlider.onValueChange   = [this,v] { *proc.vp[v].filterRes  = (float)voiceUI[v].fResSlider.getValue(); };
    ui.driveSlider.onValueChange  = [this,v] { *proc.vp[v].distDrive  = (float)voiceUI[v].driveSlider.getValue(); };
    ui.rvbMixSlider.onValueChange = [this,v] { *proc.vp[v].reverbMix  = (float)voiceUI[v].rvbMixSlider.getValue(); };
    ui.rvbSzSlider.onValueChange  = [this,v] { *proc.vp[v].reverbSize = (float)voiceUI[v].rvbSzSlider.getValue(); };
    leftContent_.addAndMakeVisible (ui.fFreqSlider);  leftContent_.addAndMakeVisible (ui.fFreqLabel);
    leftContent_.addAndMakeVisible (ui.fResSlider);   leftContent_.addAndMakeVisible (ui.fResLabel);
    leftContent_.addAndMakeVisible (ui.driveSlider);  leftContent_.addAndMakeVisible (ui.driveLabel);
    leftContent_.addAndMakeVisible (ui.rvbMixSlider); leftContent_.addAndMakeVisible (ui.rvbMixLabel);
    leftContent_.addAndMakeVisible (ui.rvbSzSlider);  leftContent_.addAndMakeVisible (ui.rvbSzLabel);

    // Sound section — Output
    styleSlider (ui.preGainSlider, 0.25f, 4.0f, p.preGain->get());
    styleSlider (ui.gainSlider,    0.0f,  2.0f, p.gain->get());
    styleSlider (ui.limitSlider,  -24.0f, 0.0f, p.limitThresh->get());
    ui.preGainSlider.setDoubleClickReturnValue (true, 1.0);
    ui.gainSlider   .setDoubleClickReturnValue (true, 1.0);
    ui.limitSlider  .setDoubleClickReturnValue (true, 0.0);
    styleLabel  (ui.preGainLabel); styleLabel (ui.gainLabel); styleLabel (ui.limitLabel);
    ui.preGainSlider.onValueChange = [this,v] { *proc.vp[v].preGain     = (float)voiceUI[v].preGainSlider.getValue(); };
    ui.gainSlider.onValueChange    = [this,v] { *proc.vp[v].gain        = (float)voiceUI[v].gainSlider.getValue(); };
    ui.limitSlider.onValueChange   = [this,v] { *proc.vp[v].limitThresh = (float)voiceUI[v].limitSlider.getValue(); };
    leftContent_.addAndMakeVisible (ui.preGainSlider); leftContent_.addAndMakeVisible (ui.preGainLabel);
    leftContent_.addAndMakeVisible (ui.gainSlider);    leftContent_.addAndMakeVisible (ui.gainLabel);
    leftContent_.addAndMakeVisible (ui.limitSlider);   leftContent_.addAndMakeVisible (ui.limitLabel);

    // FX / Presets — lock buttons
    static const char* lockNames[] = { "Pch","Atk","Dec","Sus","Rel","Flt","Res","Drv","Rvb","Sz" };
    for (int i = 0; i < 10; ++i)
    {
        ui.rndLockBtns[i].setButtonText (lockNames[i]);
        styleButton (ui.rndLockBtns[i]);
        ui.rndLockBtns[i].onClick = [this, v, i] {
            bool locked = !voiceUI[v].rndLocked[i];
            voiceUI[v].rndLocked[i] = locked;
            voiceUI[v].rndLockBtns[i].setColour (juce::TextButton::buttonColourId,
                juce::Colour (locked ? kMute : kElevated));
        };
        addAndMakeVisible (ui.rndLockBtns[i]);
    }

    // FX / Presets — rnd + reset
    styleSlider (ui.rndFxSlider, 0.0f, 1.0f, p.rndFxChance->get());
    ui.rndFxSlider.setDoubleClickReturnValue (true, 0.0);
    styleLabel  (ui.rndFxLabel);
    ui.rndFxSlider.onValueChange = [this,v] { *proc.vp[v].rndFxChance = (float)voiceUI[v].rndFxSlider.getValue(); };
    addAndMakeVisible (ui.rndFxSlider); addAndMakeVisible (ui.rndFxLabel);

    styleButton (ui.rndFxFireBtn);
    ui.rndFxFireBtn.onClick = [this,v] { proc.randomizeVoiceParams (v, voiceUI[v].rndLocked); };
    addAndMakeVisible (ui.rndFxFireBtn);

    styleButton (ui.resetFxBtn);
    ui.resetFxBtn.onClick = [this,v] { proc.resetVoiceFX (v); };
    addAndMakeVisible (ui.resetFxBtn);

    // FX / Presets — preset row
    styleButton (ui.presetSaveBtn);
    ui.presetSaveBtn.onClick = [this, v] {
        bool entering = !voiceUI[v].presetSaveMode;
        voiceUI[v].presetSaveMode = entering;
        voiceUI[v].presetSaveBtn.setColour (juce::TextButton::buttonColourId,
            juce::Colour (entering ? kActive : kElevated));
    };
    addAndMakeVisible (ui.presetSaveBtn);

    for (int s = 0; s < 8; ++s)
    {
        ui.presetBtns[s].setButtonText (juce::String (s + 1));
        styleButton (ui.presetBtns[s]);
        ui.presetBtns[s].onClick = [this, v, s] {
            if (voiceUI[v].presetSaveMode)
            {
                proc.saveVoicePreset (v, s);
                voiceUI[v].presetSaveMode = false;
                voiceUI[v].presetSaveBtn.setColour (juce::TextButton::buttonColourId,
                    juce::Colour (kElevated));
            }
            else
            {
                proc.loadVoicePreset (v, s);
            }
        };
        addAndMakeVisible (ui.presetBtns[s]);
    }

    // Modulation section (section 5)
    for (int fg = 0; fg < VoiceUI::kNumFg; ++fg)
    {
        // Canvas
        ui.fgCanvas[fg].setFuncGen (&proc.getVoiceFuncGen (v, fg));
        ui.fgCanvas[fg].onChange = [this] { repaint(); };
        leftContent_.addAndMakeVisible (ui.fgCanvas[fg]);

        // Rate cycling button
        ui.fgRateBtn[fg].setButtonText (kFgRateNames[proc.vp[v].fgRate[fg] ? proc.vp[v].fgRate[fg]->get() : 4]);
        styleButton (ui.fgRateBtn[fg]);
        ui.fgRateBtn[fg].onClick = [this, v, fg] {
            if (!proc.vp[v].fgRate[fg]) return;
            int next = (proc.vp[v].fgRate[fg]->get() + 1) % kNumFgRates;
            *proc.vp[v].fgRate[fg] = next;
            voiceUI[v].fgRateBtn[fg].setButtonText (kFgRateNames[next]);
        };
        leftContent_.addAndMakeVisible (ui.fgRateBtn[fg]);

        // Destination cycling button
        int curDest = proc.vp[v].fgDest[fg] ? proc.vp[v].fgDest[fg]->get() : 0;
        ui.fgDestBtn[fg].setButtonText (kModDestNames[curDest]);
        styleButton (ui.fgDestBtn[fg]);
        ui.fgDestBtn[fg].onClick = [this, v, fg] {
            if (!proc.vp[v].fgDest[fg]) return;
            int next = (proc.vp[v].fgDest[fg]->get() + 1) % kNumModDests;
            *proc.vp[v].fgDest[fg] = next;
            voiceUI[v].fgDestBtn[fg].setButtonText (kModDestNames[next]);
        };
        leftContent_.addAndMakeVisible (ui.fgDestBtn[fg]);

        // Depth slider
        styleSlider (ui.fgDepthSlider[fg], -1.0f, 1.0f,
                     proc.vp[v].fgDepth[fg] ? proc.vp[v].fgDepth[fg]->get() : 0.0f);
        ui.fgDepthSlider[fg].setDoubleClickReturnValue (true, 0.0);
        ui.fgDepthSlider[fg].onValueChange = [this, v, fg] {
            if (proc.vp[v].fgDepth[fg])
                *proc.vp[v].fgDepth[fg] = (float) voiceUI[v].fgDepthSlider[fg].getValue();
        };
        ui.fgDepthLabel[fg].setText ("Depth", juce::dontSendNotification);
        styleLabel (ui.fgDepthLabel[fg]);
        leftContent_.addAndMakeVisible (ui.fgDepthSlider[fg]);
        leftContent_.addAndMakeVisible (ui.fgDepthLabel[fg]);

        // Min slider
        styleSlider (ui.fgMinSlider[fg], 0.0f, 1.0f,
                     proc.vp[v].fgMin[fg] ? proc.vp[v].fgMin[fg]->get() : 0.0f);
        ui.fgMinSlider[fg].setDoubleClickReturnValue (true, 0.0);
        ui.fgMinSlider[fg].onValueChange = [this, v, fg] {
            if (proc.vp[v].fgMin[fg])
                *proc.vp[v].fgMin[fg] = (float) voiceUI[v].fgMinSlider[fg].getValue();
        };
        ui.fgMinLabel[fg].setText ("Min", juce::dontSendNotification);
        styleLabel (ui.fgMinLabel[fg]);
        leftContent_.addAndMakeVisible (ui.fgMinSlider[fg]);
        leftContent_.addAndMakeVisible (ui.fgMinLabel[fg]);

        // Max slider
        styleSlider (ui.fgMaxSlider[fg], 0.0f, 1.0f,
                     proc.vp[v].fgMax[fg] ? proc.vp[v].fgMax[fg]->get() : 1.0f);
        ui.fgMaxSlider[fg].setDoubleClickReturnValue (true, 1.0);
        ui.fgMaxSlider[fg].onValueChange = [this, v, fg] {
            if (proc.vp[v].fgMax[fg])
                *proc.vp[v].fgMax[fg] = (float) voiceUI[v].fgMaxSlider[fg].getValue();
        };
        ui.fgMaxLabel[fg].setText ("Max", juce::dontSendNotification);
        styleLabel (ui.fgMaxLabel[fg]);
        leftContent_.addAndMakeVisible (ui.fgMaxSlider[fg]);
        leftContent_.addAndMakeVisible (ui.fgMaxLabel[fg]);
    }

    updateCycleBtns (v);
}

//==============================================================================
// resized
//==============================================================================
void W2SamplerEditor::resized()
{
    const int W = getWidth();
    const int H = getHeight();

    // Derived layout values
    int contentW  = W;
    int centerX   = kLeftW;
    int centerW   = contentW - kLeftW - kRightW;
    int rightX    = contentW - kRightW;
    int mainY     = kTransportH;
    int mainH     = H - kTransportH - kBottomH;
    int bottomY   = H - kBottomH;

    layoutTransportBar();
    layoutMasterColumn();

    // Compute ring centre (scales with window)
    {
        float minDim = (float)std::min (centerW, mainH) * 0.5f;
        ringR_[0] = minDim * 0.82f;
        ringR_[1] = minDim * 0.57f;
        ringR_[2] = minDim * 0.32f;
        ringCX_ = (float)(centerX + centerW / 2);
        ringCY_ = (float)(mainY + mainH / 2);
    }

    // Voice selector row at top of left column (direct editor children)
    {
        const int selBtnW = kLeftW / 3;
        const juce::Colour voiceCols[3] = {
            juce::Colour (kV0), juce::Colour (kV1), juce::Colour (kV2) };
        for (int v = 0; v < 3; ++v)
        {
            voiceSelectBtn[v].setBounds (v * selBtnW, mainY, selBtnW - 1, 28);
            bool active = (v == selectedVoice);
            voiceSelectBtn[v].setColour (juce::TextButton::buttonColourId,
                active ? voiceCols[v] : juce::Colour (kElevated));
            voiceSelectBtn[v].setColour (juce::TextButton::textColourOffId,
                active ? juce::Colour (kBg) : juce::Colour (kTextDim));
        }
    }

    // Scrollable left panel viewport — below voice selector buttons
    leftViewport_.setBounds (0, mainY + 28, kLeftW, mainH - 28);

    hideVoiceAll();
    layoutVoicePanel (selectedVoice);
    layoutBottomBar  (selectedVoice);

    // Suppress unused variable warnings
    (void)bottomY;
    (void)centerW;
    (void)rightX;
    (void)mainH;
}

//==============================================================================
void W2SamplerEditor::layoutTransportBar()
{
    const int W = getWidth();
    int y = 0;
    int h = kTransportH;
    int x = 4;

    playBtn.setBounds (x, y + 5, 56, h - 10); x += 62;

    bpmLabel.setBounds (x, y + 10, 28, 18); x += 30;
    bpmSlider.setBounds (x, y + 4, 80, h - 8); x += 86;

    // 4 clkDiv buttons — fill remaining space before right column
    int divAreaW = W - x - kRightW - 8;
    int btnW = juce::jlimit (52, 80, divAreaW / kNumClkDivs);
    for (int i = 0; i < kNumClkDivs; ++i)
    {
        clkDivBtns[i].setBounds (x, y + 5, btnW - 2, h - 10); x += btnW;
        bool active = proc.clkDiv && proc.clkDiv->get() == kClkDivVals[i];
        clkDivBtns[i].setColour (juce::TextButton::buttonColourId,
                                 juce::Colour (active ? kActive : kElevated));
    }

    // Mute/Solo hidden from transport — they live in the master column
    for (int v = 0; v < 3; ++v)
    {
        muteBtn[v].setVisible (false);
        soloBtn[v].setVisible (false);
    }
}

//==============================================================================
void W2SamplerEditor::layoutMasterColumn()
{
    const int W = getWidth();
    const int H = getHeight();
    int rightX = W - kRightW;
    masterColumnRect_ = { rightX, 0, kRightW, H };

    // Mute/Solo — one row per voice: [M] [S] side by side
    const int msRowH    = 20;
    const int msStartY  = kTransportH + 16;
    const int halfW     = kRightW / 2 - 3;
    for (int v = 0; v < 3; ++v)
    {
        int rowY = msStartY + v * (msRowH + 2);
        muteBtn[v].setBounds (rightX + 2,            rowY, halfW, msRowH - 2);
        muteBtn[v].setVisible (true);
        soloBtn[v].setBounds (rightX + halfW + 5,    rowY, kRightW - halfW - 7, msRowH - 2);
        soloBtn[v].setVisible (true);
    }

    // Meter bars start below mute/solo section
    const int barTopY = msStartY + 3 * (msRowH + 2) + 8;
    const int barH    = H - barTopY - 36;

    // Vertical fader
    masterGainSlider.setSliderStyle (juce::Slider::LinearVertical);
    masterGainSlider.setBounds (rightX + 76, barTopY, 28, barH);
    masterGainSlider.setVisible (true);
}

//==============================================================================
void W2SamplerEditor::hideVoiceAll()
{
    for (int v = 0; v < 3; ++v)
    {
        auto& ui = voiceUI[v];
        auto hide = [](juce::Component& c) { c.setVisible (false); };

        for (auto& b : ui.sectionBtn) hide (b);

        hide (ui.loadBtn);   hide (ui.prevBtn);  hide (ui.nextBtn);
        hide (ui.rndBtn);    hide (ui.nameLabel); hide (ui.waveform);
        hide (ui.onsetSensSlider); hide (ui.onsetSensLabel);

        hide (ui.stepsSlider);  hide (ui.stepsLabel);
        hide (ui.hitsSlider);   hide (ui.hitsLabel);
        hide (ui.rotSlider);    hide (ui.rotLabel);
        hide (ui.smpAdvBtn);
        hide (ui.loopModeBtn); hide (ui.loopMsSlider); hide (ui.loopMsLabel);
        hide (ui.loopLockBtn); hide (ui.freezeBtn);

        hide (ui.offsetSlider); hide (ui.offsetLabel);
        hide (ui.warpSlider);   hide (ui.warpLabel);
        hide (ui.quantSlider);  hide (ui.quantLabel);
        hide (ui.revBtn);
        hide (ui.phaseSrcBtn);
        for (auto& b : ui.ratePresetBtns) hide (b);
        hide (ui.rateSlider); hide (ui.rateLabel);

        hide (ui.pitchSlider);  hide (ui.pitchLabel);
        hide (ui.attSlider);    hide (ui.attLabel);
        hide (ui.decSlider);    hide (ui.decLabel);
        hide (ui.susSlider);    hide (ui.susLabel);
        hide (ui.relSlider);    hide (ui.relLabel);
        hide (ui.fFreqSlider);  hide (ui.fFreqLabel);
        hide (ui.fResSlider);   hide (ui.fResLabel);
        hide (ui.driveSlider);  hide (ui.driveLabel);
        hide (ui.rvbMixSlider); hide (ui.rvbMixLabel);
        hide (ui.rvbSzSlider);  hide (ui.rvbSzLabel);
        hide (ui.gainSlider);    hide (ui.gainLabel);
        hide (ui.preGainSlider); hide (ui.preGainLabel);
        hide (ui.limitSlider);   hide (ui.limitLabel);

        for (auto& b : ui.rndLockBtns) hide (b);
        hide (ui.rndFxSlider);   hide (ui.rndFxLabel);
        hide (ui.rndFxFireBtn);  hide (ui.resetFxBtn);
        for (auto& b : ui.presetBtns) hide (b);
        hide (ui.presetSaveBtn);

        // Modulation section
        for (int fg = 0; fg < VoiceUI::kNumFg; ++fg)
        {
            hide (ui.fgCanvas[fg]);
            hide (ui.fgRateBtn[fg]);   hide (ui.fgDestBtn[fg]);
            hide (ui.fgDepthSlider[fg]); hide (ui.fgDepthLabel[fg]);
            hide (ui.fgMinSlider[fg]);   hide (ui.fgMinLabel[fg]);
            hide (ui.fgMaxSlider[fg]);   hide (ui.fgMaxLabel[fg]);
        }
    }
}

//==============================================================================
// Helper: label left, slider fills remainder
static void placeSliderCell (juce::Label& lbl, juce::Slider& s,
                              int x, int y, int cw, int h, int labelW = 40)
{
    lbl.setBounds (x, y, labelW, h);
    s.setBounds   (x + labelW + 2, y, cw - labelW - 2, h);
    lbl.setVisible (true);
    s.setVisible   (true);
}

//==============================================================================
void W2SamplerEditor::layoutVoicePanel (int v)
{
    auto& ui = voiceUI[v];
    const int x0   = 2;
    const int panW = kLeftW - 14;  // viewport content width (minus scrollbar space)
    int       y    = 4;            // relative to leftContent_, start near top
    const int sectionHeaderH = 22;
    const int sectionGap     = 3;
    const int rowGap         = 2;
    const int waveH          = 76;

    auto showSection = [&] (int s)
    {
        ui.sectionBtn[s].setBounds (x0, y, panW, sectionHeaderH);
        ui.sectionBtn[s].setVisible (true);
        y += sectionHeaderH + 2;
    };

    //--------------------------------------------------------------------------
    // Section 0: SAMPLE
    //--------------------------------------------------------------------------
    showSection (0);
    if (ui.sectionOpen[0])
    {
        // Nav row
        {
            const int h = 28, lbW = 80, nbW = 52, rbW = 40;
            ui.loadBtn.setBounds (x0,                    y, lbW, h); ui.loadBtn.setVisible (true);
            ui.prevBtn.setBounds (x0 + lbW + 2,          y, nbW, h); ui.prevBtn.setVisible (true);
            ui.nextBtn.setBounds (x0 + lbW + nbW + 4,    y, nbW, h); ui.nextBtn.setVisible (true);
            ui.rndBtn.setBounds  (x0 + lbW + nbW*2 + 6,  y, rbW, h); ui.rndBtn.setVisible  (true);
            y += h + rowGap;
        }
        // Name label
        {
            ui.nameLabel.setBounds (x0, y, panW, 18); ui.nameLabel.setVisible (true);
            y += 18 + rowGap;
        }
        // Onset sens
        {
            const int h = 24;
            ui.onsetSensLabel .setBounds (x0,      y, 36, h); ui.onsetSensLabel .setVisible (true);
            ui.onsetSensSlider.setBounds (x0 + 38, y, panW - 38, h); ui.onsetSensSlider.setVisible (true);
            y += h + rowGap;
        }
        // Waveform
        {
            ui.waveform.setBounds (x0, y, panW, waveH); ui.waveform.setVisible (true);
            y += waveH + rowGap;
        }
        y += sectionGap;
    }

    //--------------------------------------------------------------------------
    // Section 1: SEQUENCE
    //--------------------------------------------------------------------------
    showSection (1);
    if (ui.sectionOpen[1])
    {
        const int cw = panW;
        const int rh = 28;
        const int lw = 88;

        // Full-width sliders
        placeSliderCell (ui.stepsLabel, ui.stepsSlider, x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.hitsLabel,  ui.hitsSlider,  x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.rotLabel,   ui.rotSlider,   x0, y, cw, rh, lw); y += rh + 2;

        // SmpAdv + LoopMode buttons side by side (each half width)
        {
            int hw = cw / 2 - 2;
            ui.smpAdvBtn.setBounds   (x0,          y, hw, rh - 2); ui.smpAdvBtn.setVisible (true);
            ui.loopModeBtn.setBounds (x0 + hw + 4, y, cw - hw - 4, rh - 2); ui.loopModeBtn.setVisible (true);
            y += rh + 2;
        }

        // Loop ms with Lock + Freeze buttons
        placeSliderCell (ui.loopMsLabel, ui.loopMsSlider, x0, y, cw - 112, rh, lw);
        ui.loopLockBtn.setBounds (x0 + cw - 108, y, 52, rh - 2); ui.loopLockBtn.setVisible (true);
        ui.freezeBtn.setBounds   (x0 + cw -  52, y, 52, rh - 2); ui.freezeBtn.setVisible (true);
        y += rh + 4;

        y += sectionGap;
    }

    //--------------------------------------------------------------------------
    // Section 2: PHASE
    //--------------------------------------------------------------------------
    showSection (2);
    if (ui.sectionOpen[2])
    {
        const int cw = panW;
        const int rh = 28;
        const int lw = 88;

        // Full-width sliders
        placeSliderCell (ui.offsetLabel, ui.offsetSlider, x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.warpLabel,   ui.warpSlider,   x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.quantLabel,  ui.quantSlider,  x0, y, cw, rh, lw); y += rh + 2;

        // Rev button (full width, slightly shorter)
        ui.revBtn.setBounds (x0, y, cw, 24); ui.revBtn.setVisible (true);
        y += 28;

        // Phase source button (full width)
        ui.phaseSrcBtn.setBounds (x0, y, cw, 24); ui.phaseSrcBtn.setVisible (true);
        y += 28;

        // Rate preset buttons — spread across full width
        {
            int btnW = cw / kNumRatePresets;
            for (int i = 0; i < kNumRatePresets; ++i)
            {
                ui.ratePresetBtns[i].setBounds (x0 + i * btnW, y, btnW - 2, 24);
                ui.ratePresetBtns[i].setVisible (true);
            }
            y += 28;
        }

        // Rate slider (full width)
        placeSliderCell (ui.rateLabel, ui.rateSlider, x0, y, cw, rh, lw);
        y += rh + 4;

        y += sectionGap;
    }

    //--------------------------------------------------------------------------
    // Section 3: SOUND — full-width readable rows
    //--------------------------------------------------------------------------
    showSection (3);
    if (ui.sectionOpen[3])
    {
        const int cw = panW;   // full content width
        const int rh = 28;     // row height
        const int lw = 88;     // label width

        placeSliderCell (ui.pitchLabel,   ui.pitchSlider,   x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.attLabel,     ui.attSlider,     x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.decLabel,     ui.decSlider,     x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.susLabel,     ui.susSlider,     x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.relLabel,     ui.relSlider,     x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.fFreqLabel,   ui.fFreqSlider,   x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.fResLabel,    ui.fResSlider,    x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.driveLabel,   ui.driveSlider,   x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.rvbMixLabel,  ui.rvbMixSlider,  x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.rvbSzLabel,   ui.rvbSzSlider,   x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.preGainLabel, ui.preGainSlider, x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.gainLabel,    ui.gainSlider,    x0, y, cw, rh, lw); y += rh + 2;
        placeSliderCell (ui.limitLabel,   ui.limitSlider,   x0, y, cw, rh, lw); y += rh + 2;
        y += sectionGap;
    }

    //--------------------------------------------------------------------------
    // Section 5: MODULATION
    //--------------------------------------------------------------------------
    showSection (5);
    if (ui.sectionOpen[5])
    {
        const int cw  = panW;
        const int rh  = 24;
        const int cvH = 80;   // canvas height

        for (int fg = 0; fg < VoiceUI::kNumFg; ++fg)
        {
            // Canvas (full width)
            ui.fgCanvas[fg].setBounds (x0, y, cw, cvH);
            ui.fgCanvas[fg].setVisible (true);
            y += cvH + 2;

            // Row: [Rate 56px] [Dest 106px] [Depth label 38px] [Depth slider fill]
            const int ratW = 56, desW = 106, dlbW = 38;
            ui.fgRateBtn[fg].setBounds   (x0,                     y, ratW,         rh - 2);
            ui.fgDestBtn[fg].setBounds   (x0 + ratW + 2,          y, desW,         rh - 2);
            ui.fgDepthLabel[fg].setBounds(x0 + ratW + desW + 4,   y, dlbW,         rh - 2);
            ui.fgDepthSlider[fg].setBounds(x0 + ratW + desW + dlbW + 4, y,
                                           cw - ratW - desW - dlbW - 6, rh - 2);
            ui.fgRateBtn[fg].setVisible  (true);
            ui.fgDestBtn[fg].setVisible  (true);
            ui.fgDepthLabel[fg].setVisible(true);
            ui.fgDepthSlider[fg].setVisible(true);
            y += rh + 2;

            // Row: [Min label 32px] [Min slider fill/2] [Max label 32px] [Max slider fill/2]
            const int mnlW = 32;
            int halfW = (cw - mnlW * 2) / 2;
            ui.fgMinLabel[fg].setBounds  (x0,                    y, mnlW,  rh - 2);
            ui.fgMinSlider[fg].setBounds (x0 + mnlW,             y, halfW, rh - 2);
            ui.fgMaxLabel[fg].setBounds  (x0 + mnlW + halfW + 2, y, mnlW,  rh - 2);
            ui.fgMaxSlider[fg].setBounds (x0 + mnlW*2 + halfW + 2, y, halfW, rh - 2);
            ui.fgMinLabel[fg].setVisible (true);
            ui.fgMinSlider[fg].setVisible(true);
            ui.fgMaxLabel[fg].setVisible (true);
            ui.fgMaxSlider[fg].setVisible(true);
            y += rh + 4;
        }
        y += sectionGap;
    }

    // Update leftContent_ size so viewport knows true scroll height
    leftContent_.setSize (kLeftW - 12, y + 8);
}

//==============================================================================
void W2SamplerEditor::layoutBottomBar (int v)
{
    auto& ui = voiceUI[v];
    const int W = getWidth();
    const int H = getHeight();
    const int bottomY = H - kBottomH;
    const int barW    = W - kRightW;  // bottom bar doesn't extend into right col
    const int x0      = 2;
    const int panW    = barW - 4;

    // Row 1 (y+4, h=22): 10 lock buttons evenly across width
    {
        const int h = 22;
        int btnW = panW / 10;
        for (int i = 0; i < 10; ++i)
        {
            ui.rndLockBtns[i].setBounds (x0 + i * btnW, bottomY + 4, btnW - 1, h);
            ui.rndLockBtns[i].setVisible (true);
        }
    }

    // Row 2 (y+28, h=26): rndFx label+slider + fire btn + reset btn
    {
        const int h = 26, fireBtnW = 110, resetBtnW = 88;
        placeSliderCell (ui.rndFxLabel, ui.rndFxSlider,
                         x0, bottomY + 28, panW - fireBtnW - resetBtnW - 6, h, 48);
        ui.rndFxFireBtn.setBounds (x0 + panW - fireBtnW - resetBtnW - 2,
                                   bottomY + 28, fireBtnW, h);
        ui.rndFxFireBtn.setVisible (true);
        ui.resetFxBtn.setBounds (x0 + panW - resetBtnW,
                                 bottomY + 28, resetBtnW, h);
        ui.resetFxBtn.setVisible (true);
    }

    // Row 3 (y+56, h=26): Save btn + 8 preset buttons
    {
        const int h = 26, saveBtnW = 52;
        ui.presetSaveBtn.setBounds (x0, bottomY + 56, saveBtnW, h);
        ui.presetSaveBtn.setVisible (true);
        int slotW = (panW - saveBtnW - 4) / 8;
        for (int s = 0; s < 8; ++s)
        {
            ui.presetBtns[s].setBounds (x0 + saveBtnW + 4 + s * (slotW + 1),
                                        bottomY + 56, slotW, h);
            ui.presetBtns[s].setVisible (true);
        }
    }
}

//==============================================================================
// mouseDown — click a ring to select that voice
//==============================================================================
void W2SamplerEditor::mouseDown (const juce::MouseEvent& e)
{
    auto pt = e.position;
    float dx   = pt.x - ringCX_;
    float dy   = pt.y - ringCY_;
    float dist = std::sqrt (dx*dx + dy*dy);

    // Use scaled ring radii with ±25px hit zone
    for (int v = 0; v < 3; ++v)
    {
        if (dist > ringR_[v] - 25.0f && dist < ringR_[v] + 25.0f)
        {
            selectedVoice = v;
            resized();
            repaint();
            return;
        }
    }
}

//==============================================================================
// paint
//==============================================================================
void W2SamplerEditor::paint (juce::Graphics& g)
{
    const int W = getWidth();
    const int H = getHeight();
    const int mainY   = kTransportH;
    const int bottomY = H - kBottomH;
    const int mainH   = bottomY - mainY;

    // Background
    g.fillAll (juce::Colour (kBg));

    // Transport bar separator
    g.setColour (juce::Colour (kAccent));
    g.drawHorizontalLine (kTransportH, 0.0f, (float)W);

    // Left column background (voice selector row + viewport area)
    g.setColour (juce::Colour (kPanel));
    g.fillRect (0, mainY, kLeftW, mainH);

    // Voice color left strip (3px) — connects ring selection to left panel
    {
        const uint32_t voiceCols[3] = { kV0, kV1, kV2 };
        g.setColour (juce::Colour (voiceCols[selectedVoice]).withAlpha (0.8f));
        g.fillRect (0, mainY, 3, mainH);
    }

    // Left column right-edge separator
    g.setColour (juce::Colour (kAccent));
    g.drawVerticalLine (kLeftW - 1, (float)mainY, (float)bottomY);

    // Bottom bar top separator line
    g.setColour (juce::Colour (kAccent));
    g.drawHorizontalLine (bottomY, 0.0f, (float)(W - kRightW));

    // Bottom bar background
    g.setColour (juce::Colour (kPanel));
    g.fillRect (0, bottomY, W - kRightW, kBottomH);

    // Center ring area
    drawRings (g);

    // Master column
    drawMasterColumn (g);
}

//==============================================================================
// drawRings
//==============================================================================
void W2SamplerEditor::drawRings (juce::Graphics& g)
{
    const uint32_t voiceCols[3] = { kV0, kV1, kV2 };
    const float    trackWidth   = 12.0f;
    bool           running      = proc.getPlaying();

    float cx = ringCX_;
    float cy = ringCY_;

    // Draw from inner to outer so outer renders on top (V1=outer, V3=inner)
    for (int v = 2; v >= 0; --v)
    {
        juce::Colour vc = juce::Colour (voiceCols[v]);
        float r = ringR_[v];
        bool  sel = (v == selectedVoice);

        // Track arc background — selected ring at full alpha, others dim
        g.setColour (vc.withAlpha (sel ? 0.9f : 0.15f));
        g.drawEllipse (cx - r, cy - r, r*2, r*2, trackWidth);

        // Track outline rings
        g.setColour (vc.withAlpha (sel ? 0.5f : 0.25f));
        g.drawEllipse (cx - r - trackWidth*0.5f, cy - r - trackWidth*0.5f,
                       (r + trackWidth*0.5f)*2, (r + trackWidth*0.5f)*2, 1.0f);
        g.drawEllipse (cx - r + trackWidth*0.5f, cy - r + trackWidth*0.5f,
                       (r - trackWidth*0.5f)*2, (r - trackWidth*0.5f)*2, 1.0f);

        // Selected ring glow
        if (sel)
        {
            g.setColour (vc.withAlpha (0.08f));
            g.drawEllipse (cx - r - 4, cy - r - 4, (r+4)*2, (r+4)*2, 6.0f);
        }

        // Step dots
        auto& seq   = proc.getVoice(v).getSequencer();
        int   steps = seq.getSteps();
        int   curSt = proc.getVoice(v).getCurrentStep();
        if (steps > 0)
        {
            for (int s = 0; s < steps; ++s)
            {
                float angle = juce::MathConstants<float>::twoPi * (float)s / (float)steps
                              - juce::MathConstants<float>::halfPi;
                float dx2 = cx + r * std::cos (angle);
                float dy2 = cy + r * std::sin (angle);
                bool hit    = seq.getStepValue (s);
                bool active = (s == curSt) && running;

                if (active)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.95f));
                    g.fillEllipse (dx2 - 5.0f, dy2 - 5.0f, 10.0f, 10.0f);
                    g.setColour (vc);
                    g.fillEllipse (dx2 - 2.5f, dy2 - 2.5f, 5.0f, 5.0f);
                }
                else if (hit)
                {
                    g.setColour (vc);
                    g.fillEllipse (dx2 - 3.5f, dy2 - 3.5f, 7.0f, 7.0f);
                }
                else
                {
                    g.setColour (vc.withAlpha (0.25f));
                    g.fillEllipse (dx2 - 1.8f, dy2 - 1.8f, 3.6f, 3.6f);
                }
            }
        }

        // Phasor hand
        {
            double phase = proc.getVoice(v).getTransformedPhase();
            float angle  = juce::MathConstants<float>::twoPi * (float)phase
                           - juce::MathConstants<float>::halfPi;
            float innerR = r - trackWidth * 0.6f;
            float outerR = r + trackWidth * 0.6f;
            float x1 = cx + innerR * std::cos (angle);
            float y1 = cy + innerR * std::sin (angle);
            float x2 = cx + outerR * std::cos (angle);
            float y2 = cy + outerR * std::sin (angle);
            g.setColour (vc.withAlpha (0.9f));
            g.drawLine (x1, y1, x2, y2, sel ? 2.5f : 1.5f);
        }

        // Voice label (right side at 3 o'clock)
        {
            const char* vnames[] = { "V1", "V2", "V3" };
            float labelX = cx + r + 10.0f;
            float labelY = cy - 7.0f;
            g.setColour (vc.withAlpha (sel ? 1.0f : 0.5f));
            g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (sel ? 11.0f : 9.0f)));
            g.drawText (vnames[v], (int)labelX, (int)labelY, 24, 14,
                        juce::Justification::centredLeft, false);
        }
    }

    // Center hub
    g.setColour (juce::Colour (kPanel));
    g.fillEllipse (cx - 16.0f, cy - 16.0f, 32.0f, 32.0f);
    g.setColour (juce::Colour (kAccent));
    g.drawEllipse (cx - 16.0f, cy - 16.0f, 32.0f, 32.0f, 1.0f);
    if (proc.bpm)
    {
        g.setColour (juce::Colour (kTextDim));
        g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (8.0f)));
        g.drawText (juce::String ((int)proc.bpm->get()),
                    (int)(cx - 16.0f), (int)(cy - 6.0f), 32, 12,
                    juce::Justification::centred, false);
    }

    // Info text below rings
    {
        float infoY = cy + ringR_[0] + 14.0f;
        if (infoY < (float)(getHeight() - kBottomH - 16))
        {
            g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.5f)));

            juce::String info;
            for (int vi = 0; vi < 3; ++vi)
            {
                const char* vnames[] = { "V1", "V2", "V3" };
                info += juce::String (vnames[vi]) + ":steps:"
                     + juce::String (proc.getVoice(vi).getSequencer().getSteps())
                     + "  ";
            }
            if (proc.bpm)
                info += "BPM:" + juce::String ((int)proc.bpm->get());
            g.setColour (juce::Colour (kTextDim));

            int centerX = (int)(ringCX_);
            int centerW = getWidth() - kLeftW - kRightW;
            g.drawText (info, centerX - centerW/2, (int)infoY, centerW, 14,
                        juce::Justification::centred, false);

            // Mute/solo status
            for (int vi = 0; vi < 3; ++vi)
            {
                bool muted  = proc.getVoiceMuted (vi);
                bool soloed = proc.getSoloVoice() == vi;
                if (muted || soloed)
                {
                    const char* vnames[] = { "V1", "V2", "V3" };
                    g.setColour (muted ? juce::Colour (kMute) : juce::Colour (kSolo));
                    juce::String st = juce::String (vnames[vi]) + ":" + (muted ? "MUTE" : "SOLO");
                    g.drawText (st, (int)(ringCX_ + (vi-1)*70 - 28), (int)infoY + 14, 56, 12,
                                juce::Justification::centred, false);
                }
            }
        }
    }
}

//==============================================================================
void W2SamplerEditor::drawMasterColumn (juce::Graphics& g)
{
    if (masterColumnRect_.isEmpty()) return;
    auto mr = masterColumnRect_;
    const int H = getHeight();

    // Panel background
    g.setColour (juce::Colour (kPanel));
    g.fillRect (mr);

    // Left border
    g.setColour (juce::Colour (kAccent));
    g.drawVerticalLine (mr.getX(), 0.0f, (float)H);

    // "OUTPUT" header
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.5f)));
    g.drawText ("OUTPUT", mr.getX(), mr.getY() + 6, mr.getWidth(), 12,
                juce::Justification::centred);

    // Mute/Solo section header
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.0f)));
    g.drawText ("M/S", mr.getX(), mr.getY() + kTransportH + 4, mr.getWidth(), 12,
                juce::Justification::centred);

    // "ST LUFS" label + value — below mute/solo rows
    // msStartY = kTransportH+16, 3 rows of 22px = 66, gap 8 → barTopY = kTransportH+90
    const int barTopYDraw = kTransportH + 90;
    float lufs = proc.getShortTermLufs();
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.0f)));
    g.drawText ("LUFS", mr.getX(), mr.getY() + barTopYDraw - 18, mr.getWidth(), 10,
                juce::Justification::centred);
    juce::Colour lufsCol = (lufs > -6.0f)  ? juce::Colour (0xffBB2222) :
                           (lufs > -14.0f) ? juce::Colour (0xffAA8800) :
                                             juce::Colour (0xff336622);
    g.setColour (lufsCol);
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.5f)));
    g.drawText (juce::String (lufs, 1), mr.getX(), mr.getY() + barTopYDraw - 8, mr.getWidth(), 11,
                juce::Justification::centred);

    // Stereo L/R bars + dB scale — tall meter (matches layoutMasterColumn)
    const int barY  = barTopYDraw;
    const int barH  = H - barY - 36;
    const int barW  = 22;
    const int lx    = mr.getX() + 4;
    const int rx    = mr.getX() + 28;

    auto drawVBar = [&] (float peak, int bx)
    {
        g.setColour (juce::Colour (kTrack));
        g.fillRect (bx, barY, barW, barH);

        float db   = peak > 0.001f ? 20.0f * std::log10 (peak) : -70.0f;
        float norm = juce::jlimit (0.0f, 1.0f, (db + 70.0f) / 70.0f);
        int   filled = (int)(norm * barH);

        for (int yi = barY + barH - filled; yi < barY + barH; yi += 2)
        {
            float frac  = (float)(yi - barY) / (float)barH;
            float dbAtY = -70.0f + frac * 70.0f;
            juce::Colour col = (dbAtY > -3.0f)  ? juce::Colour (0xffCC2222) :
                               (dbAtY > -9.0f)  ? juce::Colour (0xffBB9900) :
                               (dbAtY > -18.0f) ? juce::Colour (0xff66AA33) :
                                                  juce::Colour (0xff448822);
            g.setColour (col);
            g.fillRect (bx, yi, barW, 2);
        }

        g.setColour (juce::Colour (kTextDim));
        g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.0f)));
        g.drawText (juce::String (db, 1), bx, barY + barH + 2, barW, 12,
                    juce::Justification::centred);
    };

    // L/R channel labels
    g.setColour (juce::Colour (kText));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.5f)));
    g.drawText ("L", lx, barY - 14, barW, 12, juce::Justification::centred);
    g.drawText ("R", rx, barY - 14, barW, 12, juce::Justification::centred);

    drawVBar (proc.getOutputPeakL(), lx);
    drawVBar (proc.getOutputPeakR(), rx);

    // dB scale labels
    {
        g.setColour (juce::Colour (kTextDim));
        g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (8.0f)));
        const float dbs[] = { 0.0f, -6.0f, -12.0f, -18.0f, -30.0f };
        for (float db : dbs)
        {
            float norm = juce::jlimit (0.0f, 1.0f, (db + 70.0f) / 70.0f);
            int   yPos = barY + barH - (int)(norm * barH);
            g.drawText (juce::String ((int)db), mr.getX() + 52, yPos - 5, 20, 10,
                        juce::Justification::centredRight);
        }
    }

    // Fader slot background
    {
        g.setColour (juce::Colour (kTrack));
        g.fillRect (mr.getX() + 74, barY, 36, barH);
        g.setColour (juce::Colour (kAccent));
        g.drawRect (mr.getX() + 74, barY, 36, barH, 1);
    }

    // "MASTER" label at bottom of fader
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.0f)));
    g.drawText ("MASTER", mr.getX() + 70, H - 32, 50, 12,
                juce::Justification::centred);
}

//==============================================================================
// timerCallback
//==============================================================================
void W2SamplerEditor::timerCallback()
{
    for (int v = 0; v < 3; ++v)
        if (proc.takeRandomizeFXRequest (v))
            proc.randomizeVoiceParams (v, voiceUI[v].rndLocked);

    proc.decayOutputPeaks();

    if (proc.masterGain)
        masterGainSlider.setValue ((double)proc.masterGain->get(), juce::dontSendNotification);

    // Repaint master column (always visible)
    if (!masterColumnRect_.isEmpty())
        repaint (masterColumnRect_);

    // Sync BPM
    if (proc.bpm)
        bpmSlider.setValue ((double)proc.bpm->get(), juce::dontSendNotification);

    playBtn.setButtonText (proc.getPlaying() ? "Stop" : "Play");
    playBtn.setColour (juce::TextButton::buttonColourId,
        juce::Colour (proc.getPlaying() ? kActive : kElevated));

    // Sync clock div buttons
    if (proc.clkDiv)
    {
        int cur = proc.clkDiv->get();
        for (int i = 0; i < kNumClkDivs; ++i)
            clkDivBtns[i].setColour (juce::TextButton::buttonColourId,
                juce::Colour (kClkDivVals[i] == cur ? kActive : kElevated));
    }

    // Sync mute/solo colours
    for (int v = 0; v < 3; ++v)
    {
        muteBtn[v].setColour (juce::TextButton::buttonColourId,
            juce::Colour (proc.getVoiceMuted (v) ? kMute : kElevated));
        soloBtn[v].setColour (juce::TextButton::buttonColourId,
            juce::Colour (proc.getSoloVoice() == v ? kSolo : kElevated));
    }

    // Sync voice select button colors
    {
        const juce::Colour voiceCols[3] = {
            juce::Colour (kV0), juce::Colour (kV1), juce::Colour (kV2) };
        for (int v = 0; v < 3; ++v)
        {
            bool active = (v == selectedVoice);
            voiceSelectBtn[v].setColour (juce::TextButton::buttonColourId,
                active ? voiceCols[v] : juce::Colour (kElevated));
            voiceSelectBtn[v].setColour (juce::TextButton::textColourOffId,
                active ? juce::Colour (kBg) : juce::Colour (kTextDim));
        }
    }

    // Sync selected voice params
    syncVoiceFromParams (selectedVoice);

    const int W = getWidth();
    const int H = getHeight();

    // Repaint ring area
    repaint (kLeftW, kTransportH, W - kLeftW - kRightW, H - kTransportH - kBottomH);
    // Repaint left panel
    repaint (0, kTransportH, kLeftW, H - kTransportH);
    // Repaint bottom bar
    repaint (0, H - kBottomH, W - kRightW, kBottomH);
}

//==============================================================================
void W2SamplerEditor::syncVoiceFromParams (int v)
{
    const auto& p  = proc.vp[v];
    auto&       ui = voiceUI[v];
    auto d = juce::dontSendNotification;

    // Sample name + onset info
    auto* entry = proc.getVoice(v).getLibrary().current();
    juce::String name = entry ? entry->name : juce::String ("none");
    if (entry)
    {
        name += "  pk:" + juce::String (entry->peakDb, 1) + "dB";
        if (entry->onsetsAnalysed && entry->onsets.count > 0)
            name += "  " + juce::String (entry->onsets.count)
                 + " onsets ~" + juce::String (entry->onsets.estimatedBPM, 1) + "bpm";
    }
    ui.nameLabel.setText (name, d);

    float curRate = p.rate->get();
    ui.rateSlider.setValue ((double)curRate, d);
    for (int i = 0; i < kNumRatePresets; ++i)
    {
        bool active = std::abs (kRatePresets[i].value - curRate) < 0.01f;
        ui.ratePresetBtns[i].setColour (juce::TextButton::buttonColourId,
            juce::Colour (active ? kActive : kElevated));
    }

    ui.offsetSlider.setValue ((double)p.phaseOffset->get(), d);
    ui.warpSlider.setValue   ((double)p.warp->get(), d);
    ui.quantSlider.setValue  ((double)p.quantiseAmt->get(), d);
    ui.stepsSlider.setValue  ((double)p.seqSteps->get(), d);
    ui.hitsSlider.setValue   ((double)p.seqHits->get(), d);
    ui.rotSlider.setValue    ((double)p.seqRotation->get(), d);
    ui.pitchSlider.setValue  ((double)p.pitch->get(), d);

    // Pitch label shows note name relative to detected key
    {
        auto* e2 = proc.getVoice(v).getLibrary().current();
        if (e2 && e2->keyAnalysed && e2->detectedKey.keyIndex >= 0)
        {
            static const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            int rootPc = e2->detectedKey.keyIndex % 12;
            int semis  = (int) std::round (p.pitch->get());
            int notePc = ((rootPc + semis) % 12 + 12) % 12;
            ui.pitchLabel.setText (juce::String (noteNames[notePc]), d);
        }
        else
        {
            ui.pitchLabel.setText ("Pch", d);
        }
    }

    ui.attSlider.setValue    ((double)p.attack->get(), d);
    ui.decSlider.setValue    ((double)p.decay->get(), d);
    ui.susSlider.setValue    ((double)p.sustain->get(), d);
    ui.relSlider.setValue    ((double)p.release->get(), d);
    ui.fFreqSlider.setValue  ((double)p.filterFreq->get(), d);
    ui.fResSlider.setValue   ((double)p.filterRes->get(), d);
    ui.driveSlider.setValue  ((double)p.distDrive->get(), d);
    ui.rvbMixSlider.setValue ((double)p.reverbMix->get(), d);
    ui.rvbSzSlider.setValue  ((double)p.reverbSize->get(), d);
    ui.gainSlider.setValue     ((double)p.gain->get(), d);
    ui.preGainSlider.setValue  ((double)p.preGain->get(), d);
    ui.limitSlider.setValue    ((double)p.limitThresh->get(), d);
    ui.loopMsSlider.setValue   ((double)p.loopSizeMs->get(), d);
    ui.rndFxSlider.setValue    ((double)p.rndFxChance->get(), d);

    for (int s = 0; s < 8; ++s)
    {
        bool valid = proc.presets_[v][s].valid;
        ui.presetBtns[s].setButtonText (juce::String (s + 1) + (valid ? "*" : ""));
    }

    {
        bool fr = p.reverbFreeze->get();
        ui.freezeBtn.setToggleState (fr, d);
        ui.freezeBtn.setColour (juce::TextButton::buttonColourId,
                                juce::Colour (fr ? kActive : kElevated));
    }
    {
        bool lk = p.loopSizeLock->get();
        ui.loopLockBtn.setToggleState (lk, d);
        ui.loopLockBtn.setColour (juce::TextButton::buttonColourId,
                                  juce::Colour (lk ? kActive : kElevated));
    }

    // Waveform state
    float playPos  = proc.getVoice(v).getPlayPositionNorm();
    float loopAnch = proc.getVoice(v).getSeqLoopAnchorNorm();

    ui.waveform.setState (p.regionStart->get(), p.regionEnd->get(),
                          p.loopStart->get(),   p.loopEnd->get(),
                          p.loopMode->get(),     p.loopSizeLock->get(),
                          p.loopSizeMs->get(),
                          playPos, loopAnch);

    if (auto* e = proc.getVoice(v).getLibrary().current())
    {
        ui.waveform.setSampleInfo (e->sampleRate, e->buffer.getNumSamples());
        ui.waveform.setBuffer (&e->buffer);
    }
    else
    {
        ui.waveform.setSampleInfo (0.0, 0);
        ui.waveform.setBuffer (nullptr);
    }

    if (auto* e = proc.getVoice(v).getLibrary().current())
        if (e->onsetsAnalysed)
            ui.waveform.setOnsets (e->onsets.positions.data(), e->onsets.count);
        else
            ui.waveform.setOnsets (nullptr, 0);
    else
        ui.waveform.setOnsets (nullptr, 0);

    ui.waveform.repaint();

    updateCycleBtns (v);
}

//==============================================================================
void W2SamplerEditor::updateCycleBtns (int v)
{
    const auto& p  = proc.vp[v];
    auto&       ui = voiceUI[v];
    if (!p.phaseSource) return;

    static const char* srcNames[]  = { "Master", "Sync V1", "Sync V2", "Sync V3" };
    static const char* advModes[]  = { "Hold",   "Seq",     "Rnd" };
    static const char* loopModes[] = { "Off", "Fixed", "Rnd", "Seq", "OnsSeq", "OnsRnd" };

    ui.phaseSrcBtn.setButtonText  (srcNames  [p.phaseSource->get()]);
    ui.smpAdvBtn.setButtonText    (advModes  [p.sampleAdv->get()]);
    ui.loopModeBtn.setButtonText  (loopModes [p.loopMode->get()]);
    ui.revBtn.setButtonText       (p.reverse->get() ? "Rev: ON" : "Rev: Off");
    ui.revBtn.setColour (juce::TextButton::buttonColourId,
        juce::Colour (p.reverse->get() ? (uint32_t)kActive : (uint32_t)kElevated));
    // LoopMode: any mode other than Off (0) → active color
    ui.loopModeBtn.setColour (juce::TextButton::buttonColourId,
        juce::Colour (p.loopMode->get() != 0 ? (uint32_t)kActive : (uint32_t)kElevated));
    // SmpAdv: any mode other than Hold (0) → active color
    ui.smpAdvBtn.setColour (juce::TextButton::buttonColourId,
        juce::Colour (p.sampleAdv->get() != 0 ? (uint32_t)kActive : (uint32_t)kElevated));
}
