#include "PluginEditor.h"

//==============================================================================
// Style helpers
//==============================================================================
void W2SamplerEditor::styleSlider (juce::Slider& s, float mn, float mx, float def, bool isInt)
{
    s.setRange ((double)mn, (double)mx, isInt ? 1.0 : 0.0);
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    // No permanent text box — value pops up on drag (cleaner, wider track)
    s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    s.setPopupDisplayEnabled (true, true, this);
    s.setValue ((double)def, juce::dontSendNotification);

    s.setColour (juce::Slider::backgroundColourId,     juce::Colour (kTrack));
    s.setColour (juce::Slider::trackColourId,          juce::Colour (kAccent));
    s.setColour (juce::Slider::thumbColourId,          juce::Colour (kThumb));
    s.setColour (juce::Slider::textBoxTextColourId,    juce::Colour (kText));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (kPanel));
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff1e3a5f));
}

void W2SamplerEditor::styleButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (kPanel));
    b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kActive));
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kText));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kText));
}

void W2SamplerEditor::styleLabel (juce::Label& l, bool bright)
{
    l.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (11.5f)));
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
    setSize (820, 620);

    // ── Tab bar ───────────────────────────────────────────────────────────────
    const char* tnames[] = { "Master", "Voice 1", "Voice 2", "Voice 3" };
    for (int t = 0; t < kNumTabs; ++t)
    {
        tabButtons[t].setButtonText (tnames[t]);
        styleButton (tabButtons[t]);
        addAndMakeVisible (tabButtons[t]);
        tabButtons[t].onClick = [this, t] { activeTab = t; resized(); repaint(); };
    }

    buildGlobalBar();

    for (int v = 0; v < 3; ++v)
        buildVoiceUI (v);

    startTimerHz (20);
    resized();
}

W2SamplerEditor::~W2SamplerEditor() { stopTimer(); }

//==============================================================================
// buildGlobalBar — always-visible controls
//==============================================================================
void W2SamplerEditor::buildGlobalBar()
{
    // Play/Stop
    styleButton (playBtn);
    playBtn.onClick = [this] {
        bool now = !proc.getPlaying();
        proc.setPlaying (now);
    };
    addAndMakeVisible (playBtn);

    // BPM
    styleLabel (bpmLabel, true);
    bpmLabel.setText ("BPM", juce::dontSendNotification);
    addAndMakeVisible (bpmLabel);

    styleSlider (bpmSlider, 20.0f, 999.0f, proc.bpm->get());
    bpmSlider.setPopupDisplayEnabled (false, false, nullptr);  // always show BPM
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 24);
    bpmSlider.setColour (juce::Slider::textBoxTextColourId,    juce::Colour (kText));
    bpmSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (kPanel));
    bpmSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (kTrack));
    bpmSlider.onValueChange = [this] { *proc.bpm = (float)bpmSlider.getValue(); };
    addAndMakeVisible (bpmSlider);

    // Clock cycle div label
    styleLabel (clkDivLabel, false);
    clkDivLabel.setText ("Cycle", juce::dontSendNotification);
    addAndMakeVisible (clkDivLabel);

    // Clock div buttons: 1b 2b 4b 8b
    const char* divNames[] = { "1 Beat", "2 Beats", "4=Bar", "8=2Bar" };
    for (int i = 0; i < kNumClkDivs; ++i)
    {
        clkDivBtns[i].setButtonText (divNames[i]);
        styleButton (clkDivBtns[i]);
        int val = kClkDivVals[i];
        clkDivBtns[i].onClick = [this, val] {
            *proc.clkDiv = val;
            // update button highlights
            for (int j = 0; j < kNumClkDivs; ++j)
                clkDivBtns[j].setColour (juce::TextButton::buttonColourId,
                    juce::Colour (kClkDivVals[j] == val ? kActive : kPanel));
        };
        addAndMakeVisible (clkDivBtns[i]);
    }

    // Mute / Solo per voice
    const char* vn[] = { "V1", "V2", "V3" };
    for (int v = 0; v < 3; ++v)
    {
        muteBtn[v].setButtonText (juce::String (vn[v]) + " M");
        styleButton (muteBtn[v]);
        muteBtn[v].setColour (juce::TextButton::buttonColourId, juce::Colour (kPanel));
        muteBtn[v].onClick = [this, v] {
            bool m = !proc.getVoiceMuted (v);
            proc.setVoiceMute (v, m);
            muteBtn[v].setColour (juce::TextButton::buttonColourId,
                                  juce::Colour (m ? kMute : kPanel));
        };
        addAndMakeVisible (muteBtn[v]);

        soloBtn[v].setButtonText (juce::String (vn[v]) + " S");
        styleButton (soloBtn[v]);
        soloBtn[v].onClick = [this, v] {
            proc.setVoiceSolo (v);
            // update solo button highlights
            int s = proc.getSoloVoice();
            for (int j = 0; j < 3; ++j)
                soloBtn[j].setColour (juce::TextButton::buttonColourId,
                    juce::Colour ((s == j) ? kSolo : kPanel));
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

    // Nav
    styleButton (ui.loadBtn);
    styleButton (ui.prevBtn);
    styleButton (ui.nextBtn);
    styleButton (ui.rndBtn);
    ui.nameLabel.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (11.5f)));
    ui.nameLabel.setColour (juce::Label::textColourId, juce::Colour (kText));
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

    addAndMakeVisible (ui.loadBtn);  addAndMakeVisible (ui.prevBtn);
    addAndMakeVisible (ui.nextBtn);  addAndMakeVisible (ui.rndBtn);
    addAndMakeVisible (ui.nameLabel);

    // Waveform callbacks → write AudioParameters directly
    ui.waveform.onRegionStart = [this,v](float val) { *proc.vp[v].regionStart = val; };
    ui.waveform.onRegionEnd   = [this,v](float val) { *proc.vp[v].regionEnd   = val; };
    ui.waveform.onLoopStart   = [this,v](float val) { *proc.vp[v].loopStart   = val; };
    ui.waveform.onLoopEnd     = [this,v](float val) { *proc.vp[v].loopEnd     = val; };
    addAndMakeVisible (ui.waveform);

    // Phase source button
    // Explains: which clock this voice follows.
    // "Master" = master bar clock. "Sync Vn" = follow voice n's transformed phase.
    styleButton (ui.phaseSrcBtn);
    ui.phaseSrcBtn.onClick = [this,v] {
        *proc.vp[v].phaseSource = (proc.vp[v].phaseSource->get() + 1) % 4;
        updateCycleBtns (v);
    };
    addAndMakeVisible (ui.phaseSrcBtn);

    // Rate preset buttons
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
        addAndMakeVisible (ui.ratePresetBtns[i]);
    }

    // Rate fine slider
    styleSlider (ui.rateSlider, 0.125f, 8.0f, p.rate->get());
    styleLabel  (ui.rateLabel, false);
    ui.rateSlider.onValueChange = [this,v] {
        *proc.vp[v].rate = (float)voiceUI[v].rateSlider.getValue();
        updateCycleBtns (v);
    };
    addAndMakeVisible (ui.rateSlider);  addAndMakeVisible (ui.rateLabel);

    // Phase transform row
    styleSlider (ui.offsetSlider, 0.0f,  1.0f, p.phaseOffset->get());
    styleSlider (ui.warpSlider,  -1.0f,  1.0f, p.warp->get());
    styleSlider (ui.quantSlider,  0.0f,  1.0f, p.quantiseAmt->get());
    styleLabel  (ui.offsetLabel); styleLabel (ui.warpLabel); styleLabel (ui.quantLabel);
    ui.offsetSlider.onValueChange = [this,v] { *proc.vp[v].phaseOffset = (float)voiceUI[v].offsetSlider.getValue(); };
    ui.warpSlider.onValueChange   = [this,v] { *proc.vp[v].warp        = (float)voiceUI[v].warpSlider.getValue(); };
    ui.quantSlider.onValueChange  = [this,v] { *proc.vp[v].quantiseAmt = (float)voiceUI[v].quantSlider.getValue(); };
    addAndMakeVisible (ui.offsetSlider); addAndMakeVisible (ui.offsetLabel);
    addAndMakeVisible (ui.warpSlider);   addAndMakeVisible (ui.warpLabel);
    addAndMakeVisible (ui.quantSlider);  addAndMakeVisible (ui.quantLabel);

    styleButton (ui.revBtn);
    ui.revBtn.onClick = [this,v] {
        *proc.vp[v].reverse = !proc.vp[v].reverse->get();
        updateCycleBtns (v);
    };
    addAndMakeVisible (ui.revBtn);

    // Seq row
    styleSlider (ui.stepsSlider, 1, 32, (float)p.seqSteps->get(), true);
    styleSlider (ui.hitsSlider,  0, 32, (float)p.seqHits->get(),  true);
    styleSlider (ui.rotSlider,   0, 31, (float)p.seqRotation->get(), true);
    styleLabel  (ui.stepsLabel); styleLabel (ui.hitsLabel); styleLabel (ui.rotLabel);
    ui.stepsSlider.onValueChange = [this,v] { *proc.vp[v].seqSteps    = (int)voiceUI[v].stepsSlider.getValue(); };
    ui.hitsSlider.onValueChange  = [this,v] { *proc.vp[v].seqHits     = (int)voiceUI[v].hitsSlider.getValue(); };
    ui.rotSlider.onValueChange   = [this,v] { *proc.vp[v].seqRotation = (int)voiceUI[v].rotSlider.getValue(); };
    addAndMakeVisible (ui.stepsSlider); addAndMakeVisible (ui.stepsLabel);
    addAndMakeVisible (ui.hitsSlider);  addAndMakeVisible (ui.hitsLabel);
    addAndMakeVisible (ui.rotSlider);   addAndMakeVisible (ui.rotLabel);

    styleButton (ui.smpAdvBtn);
    ui.smpAdvBtn.onClick = [this,v] {
        *proc.vp[v].sampleAdv = (proc.vp[v].sampleAdv->get() + 1) % 3;
        updateCycleBtns (v);
    };
    addAndMakeVisible (ui.smpAdvBtn);

    // ADSR row
    styleSlider (ui.pitchSlider, -24.0f, 24.0f, p.pitch->get());
    styleSlider (ui.attSlider,   0.001f,  2.0f, p.attack->get());
    styleSlider (ui.decSlider,   0.001f,  2.0f, p.decay->get());
    styleSlider (ui.susSlider,     0.0f,  1.0f, p.sustain->get());
    styleSlider (ui.relSlider,   0.001f,  4.0f, p.release->get());
    styleLabel (ui.pitchLabel); styleLabel (ui.attLabel); styleLabel (ui.decLabel);
    styleLabel (ui.susLabel);   styleLabel (ui.relLabel);
    ui.pitchSlider.onValueChange = [this,v] { *proc.vp[v].pitch   = (float)voiceUI[v].pitchSlider.getValue(); };
    ui.attSlider.onValueChange   = [this,v] { *proc.vp[v].attack  = (float)voiceUI[v].attSlider.getValue(); };
    ui.decSlider.onValueChange   = [this,v] { *proc.vp[v].decay   = (float)voiceUI[v].decSlider.getValue(); };
    ui.susSlider.onValueChange   = [this,v] { *proc.vp[v].sustain = (float)voiceUI[v].susSlider.getValue(); };
    ui.relSlider.onValueChange   = [this,v] { *proc.vp[v].release = (float)voiceUI[v].relSlider.getValue(); };
    addAndMakeVisible (ui.pitchSlider); addAndMakeVisible (ui.pitchLabel);
    addAndMakeVisible (ui.attSlider);   addAndMakeVisible (ui.attLabel);
    addAndMakeVisible (ui.decSlider);   addAndMakeVisible (ui.decLabel);
    addAndMakeVisible (ui.susSlider);   addAndMakeVisible (ui.susLabel);
    addAndMakeVisible (ui.relSlider);   addAndMakeVisible (ui.relLabel);

    // FX row
    styleSlider (ui.fFreqSlider,  20.0f, 20000.0f, p.filterFreq->get());
    styleSlider (ui.fResSlider,    0.5f,    10.0f, p.filterRes->get());
    styleSlider (ui.driveSlider,   0.0f,     1.0f, p.distDrive->get());
    styleSlider (ui.rvbMixSlider,  0.0f,     1.0f, p.reverbMix->get());
    styleSlider (ui.rvbSzSlider,   0.0f,     1.0f, p.reverbSize->get());
    styleLabel (ui.fFreqLabel); styleLabel (ui.fResLabel); styleLabel (ui.driveLabel);
    styleLabel (ui.rvbMixLabel); styleLabel (ui.rvbSzLabel);
    ui.fFreqSlider.onValueChange  = [this,v] { *proc.vp[v].filterFreq = (float)voiceUI[v].fFreqSlider.getValue(); };
    ui.fResSlider.onValueChange   = [this,v] { *proc.vp[v].filterRes  = (float)voiceUI[v].fResSlider.getValue(); };
    ui.driveSlider.onValueChange  = [this,v] { *proc.vp[v].distDrive  = (float)voiceUI[v].driveSlider.getValue(); };
    ui.rvbMixSlider.onValueChange = [this,v] { *proc.vp[v].reverbMix  = (float)voiceUI[v].rvbMixSlider.getValue(); };
    ui.rvbSzSlider.onValueChange  = [this,v] { *proc.vp[v].reverbSize = (float)voiceUI[v].rvbSzSlider.getValue(); };
    addAndMakeVisible (ui.fFreqSlider);  addAndMakeVisible (ui.fFreqLabel);
    addAndMakeVisible (ui.fResSlider);   addAndMakeVisible (ui.fResLabel);
    addAndMakeVisible (ui.driveSlider);  addAndMakeVisible (ui.driveLabel);
    addAndMakeVisible (ui.rvbMixSlider); addAndMakeVisible (ui.rvbMixLabel);
    addAndMakeVisible (ui.rvbSzSlider);  addAndMakeVisible (ui.rvbSzLabel);

    // Misc row
    styleSlider (ui.gainSlider, 0.0f, 2.0f, p.gain->get());
    styleSlider (ui.loopMsSlider, 5.0f, 5000.0f, p.loopSizeMs->get());
    styleLabel  (ui.gainLabel); styleLabel (ui.loopMsLabel);
    ui.gainSlider.onValueChange   = [this,v] { *proc.vp[v].gain       = (float)voiceUI[v].gainSlider.getValue(); };
    ui.loopMsSlider.onValueChange = [this,v] { *proc.vp[v].loopSizeMs = (float)voiceUI[v].loopMsSlider.getValue(); };
    addAndMakeVisible (ui.gainSlider);   addAndMakeVisible (ui.gainLabel);
    addAndMakeVisible (ui.loopMsSlider); addAndMakeVisible (ui.loopMsLabel);

    styleButton (ui.freezeBtn);
    ui.freezeBtn.setClickingTogglesState (true);
    ui.freezeBtn.onClick = [this,v] {
        bool on = voiceUI[v].freezeBtn.getToggleState();
        *proc.vp[v].reverbFreeze = (int)on;
        voiceUI[v].freezeBtn.setColour (juce::TextButton::buttonColourId,
                                        juce::Colour (on ? kActive : kPanel));
    };
    addAndMakeVisible (ui.freezeBtn);

    styleButton (ui.loopModeBtn);
    ui.loopModeBtn.onClick = [this,v] {
        *proc.vp[v].loopMode = (proc.vp[v].loopMode->get() + 1) % 4;
        updateCycleBtns (v);
    };
    addAndMakeVisible (ui.loopModeBtn);

    styleButton (ui.loopLockBtn);
    ui.loopLockBtn.setClickingTogglesState (true);
    ui.loopLockBtn.onClick = [this,v] {
        bool on = voiceUI[v].loopLockBtn.getToggleState();
        *proc.vp[v].loopSizeLock = (int)on;
        voiceUI[v].loopLockBtn.setColour (juce::TextButton::buttonColourId,
                                          juce::Colour (on ? kActive : kPanel));
    };
    addAndMakeVisible (ui.loopLockBtn);

    // RndFX row
    styleSlider (ui.rndFxSlider, 0.0f, 1.0f, p.rndFxChance->get());
    styleLabel  (ui.rndFxLabel);
    ui.rndFxSlider.onValueChange = [this,v] { *proc.vp[v].rndFxChance = (float)voiceUI[v].rndFxSlider.getValue(); };
    addAndMakeVisible (ui.rndFxSlider); addAndMakeVisible (ui.rndFxLabel);

    styleButton (ui.rndFxFireBtn);
    ui.rndFxFireBtn.onClick = [this,v] { proc.randomizeVoiceParams (v); };
    addAndMakeVisible (ui.rndFxFireBtn);

    updateCycleBtns (v);
}

//==============================================================================
// resized
//==============================================================================
void W2SamplerEditor::resized()
{
    int w = getWidth();

    // Tab bar (y=0..28)
    int tabW = (w * 2) / kNumTabs / 2;  // use half the width for tabs
    tabW = std::min (tabW, w / kNumTabs);
    for (int t = 0; t < kNumTabs; ++t)
        tabButtons[t].setBounds (t * tabW, 0, tabW, 28);

    layoutGlobalBar();

    hideVoiceAll();

    if (activeTab == 0)
        layoutMasterTab();
    else
        layoutVoiceTab (activeTab - 1);
}

//==============================================================================
void W2SamplerEditor::layoutGlobalBar()
{
    int w   = getWidth();
    int y   = 28;
    int h   = 32;
    int x   = 0;

    // Play/Stop — 72px
    playBtn.setBounds (x, y, 72, h); x += 74;

    // BPM label + slider — 140px total
    bpmLabel.setBounds (x, y, 32, h); x += 32;
    bpmSlider.setBounds (x, y, 110, h); x += 114;

    // "Cycle:" label — 40px
    clkDivLabel.setBounds (x, y + 8, 40, 16); x += 42;

    // Clock div buttons: [1 Beat][2 Beats][4=Bar][8=2Bar] — 4×64px
    int dbW = (w - x - 220) / 4;  // share remaining space before mute/solo
    dbW = std::max (dbW, 52);
    for (int i = 0; i < kNumClkDivs; ++i)
    {
        clkDivBtns[i].setBounds (x, y, dbW, h); x += dbW + 2;
        // highlight active div
        bool active = proc.clkDiv && proc.clkDiv->get() == kClkDivVals[i];
        clkDivBtns[i].setColour (juce::TextButton::buttonColourId,
                                 juce::Colour (active ? kActive : kPanel));
    }

    // Mute/Solo per voice — 3×(38+38+4)=240px
    x = w - 228;
    for (int v = 0; v < 3; ++v)
    {
        bool m = proc.getVoiceMuted (v);
        bool s = proc.getSoloVoice() == v;
        muteBtn[v].setBounds (x, y, 36, h);
        muteBtn[v].setColour (juce::TextButton::buttonColourId, juce::Colour (m ? kMute : kPanel));
        x += 38;
        soloBtn[v].setBounds (x, y, 36, h);
        soloBtn[v].setColour (juce::TextButton::buttonColourId, juce::Colour (s ? kSolo : kPanel));
        x += 38;
    }
}

//==============================================================================
void W2SamplerEditor::hideVoiceAll()
{
    for (int v = 0; v < 3; ++v)
    {
        auto& ui = voiceUI[v];
        auto hide = [](juce::Component& c) { c.setVisible (false); };
        hide (ui.loadBtn);   hide (ui.prevBtn);  hide (ui.nextBtn);
        hide (ui.rndBtn);    hide (ui.nameLabel); hide (ui.waveform);
        hide (ui.phaseSrcBtn);
        for (auto& b : ui.ratePresetBtns) hide (b);
        hide (ui.rateSlider); hide (ui.rateLabel);
        hide (ui.offsetSlider); hide (ui.offsetLabel);
        hide (ui.warpSlider);   hide (ui.warpLabel);
        hide (ui.quantSlider);  hide (ui.quantLabel);
        hide (ui.revBtn);
        hide (ui.stepsSlider);  hide (ui.stepsLabel);
        hide (ui.hitsSlider);   hide (ui.hitsLabel);
        hide (ui.rotSlider);    hide (ui.rotLabel);
        hide (ui.smpAdvBtn);
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
        hide (ui.gainSlider);   hide (ui.gainLabel);
        hide (ui.freezeBtn);    hide (ui.loopModeBtn);
        hide (ui.loopMsSlider); hide (ui.loopMsLabel);
        hide (ui.loopLockBtn);
        hide (ui.rndFxSlider);  hide (ui.rndFxLabel);
        hide (ui.rndFxFireBtn);
    }
}

//==============================================================================
void W2SamplerEditor::layoutMasterTab()
{
    masterPhasorArea_ = { 0, 60, getWidth(), getHeight() - 60 };
}

//==============================================================================
// Helper: label on left, slider fills the rest of the cell
static void placeSliderCell (juce::Label& lbl, juce::Slider& s,
                              int x, int y, int cw, int h, int labelW = 52)
{
    lbl.setBounds (x, y, labelW, h);
    s.setBounds   (x + labelW + 2, y, cw - labelW - 2, h);
    lbl.setVisible (true);
    s.setVisible   (true);
}

//==============================================================================
void W2SamplerEditor::layoutVoiceTab (int v)
{
    auto& ui = voiceUI[v];
    int   w  = getWidth();
    int   y  = 60;      // below tab bar + global bar
    const int rowGap = 3;

    // ── Row 0: nav (h=28) ────────────────────────────────────────────────────
    ui.loadBtn.setBounds   (0,   y, 112, 28); ui.loadBtn.setVisible (true);
    ui.prevBtn.setBounds   (114, y,  58, 28); ui.prevBtn.setVisible (true);
    ui.nextBtn.setBounds   (174, y,  58, 28); ui.nextBtn.setVisible (true);
    ui.rndBtn.setBounds    (234, y,  48, 28); ui.rndBtn.setVisible  (true);
    ui.nameLabel.setBounds (286, y, w-288, 28); ui.nameLabel.setVisible (true);
    y += 28 + rowGap;

    // ── Row 1: waveform (h=148) ───────────────────────────────────────────────
    ui.waveform.setBounds (0, y, w, 148);
    ui.waveform.setVisible (true);
    y += 148 + rowGap;

    // ── Row 2: step grid (h=22, drawn in paint()) — immediately below waveform
    ui.gridRect = { 0, y, w, 22 };
    y += 22 + rowGap;

    // ── Row 3: seq controls (4 cols, h=28) ───────────────────────────────────
    {
        const int h = 28, lw = 46, cw = w / 4;
        placeSliderCell (ui.stepsLabel, ui.stepsSlider, 0,    y, cw, h, lw);
        placeSliderCell (ui.hitsLabel,  ui.hitsSlider,  cw,   y, cw, h, lw);
        placeSliderCell (ui.rotLabel,   ui.rotSlider,   2*cw, y, cw, h, lw);
        ui.smpAdvBtn.setBounds (3*cw + 2, y + h/2 - 12, cw - 4, 24);
        ui.smpAdvBtn.setVisible (true);
        y += h + rowGap;
    }

    // ── Row 4: loop controls (5 cols, h=28) ──────────────────────────────────
    // [LoopMode] [LoopMs label+slider] [Lock] [Gain label+slider] [Freeze]
    {
        const int h = 28, cw5 = w / 5, lw5 = 48;
        ui.loopModeBtn.setBounds (0, y, cw5 - 4, h); ui.loopModeBtn.setVisible (true);
        placeSliderCell (ui.loopMsLabel, ui.loopMsSlider, cw5, y, cw5, h, lw5);
        ui.loopLockBtn.setBounds (2*cw5 + 2, y, cw5 - 4, h); ui.loopLockBtn.setVisible (true);
        placeSliderCell (ui.gainLabel, ui.gainSlider, 3*cw5, y, cw5, h, lw5);
        ui.freezeBtn.setBounds (4*cw5 + 2, y, cw5 - 4, h); ui.freezeBtn.setVisible (true);
        y += h + rowGap;
    }

    // ── Row 5: clock row (h=40) ───────────────────────────────────────────────
    {
        const int h = 40;
        ui.phasorRect = { 0, y, h, h };
        int xc = h + 4;

        ui.phaseSrcBtn.setBounds (xc, y + h/2 - 12, 78, 24);
        ui.phaseSrcBtn.setVisible (true);
        xc += 80;

        const int btnW = 36;
        for (int i = 0; i < kNumRatePresets; ++i)
        {
            ui.ratePresetBtns[i].setBounds (xc, y + h/2 - 12, btnW, 24);
            ui.ratePresetBtns[i].setVisible (true);
            xc += btnW + 2;
        }

        int rateX  = xc + 4;
        int labelW = 36;
        ui.rateLabel.setBounds  (rateX, y + h/2 - 10, labelW, 20);
        ui.rateSlider.setBounds (rateX + labelW + 2, y + h/2 - 10, w - rateX - labelW - 4, 20);
        ui.rateLabel.setVisible (true);
        ui.rateSlider.setVisible (true);
        y += h + rowGap;
    }

    // ── Rows 6-9: rows with 4/5 columns (h=30 each) ──────────────────────────
    const int h4 = 30, lw4 = 50, cw4 = w / 4;
    const int h5 = 30, lw5 = 48, cw5 = w / 5;

    // Row 6: Phase transform
    placeSliderCell (ui.offsetLabel, ui.offsetSlider, 0,     y, cw4, h4, lw4);
    placeSliderCell (ui.warpLabel,   ui.warpSlider,   cw4,   y, cw4, h4, lw4);
    placeSliderCell (ui.quantLabel,  ui.quantSlider,  2*cw4, y, cw4, h4, lw4);
    ui.revBtn.setBounds (3*cw4 + 2, y + h4/2 - 12, cw4 - 4, 24); ui.revBtn.setVisible (true);
    y += h4 + rowGap;

    // Row 7: Pitch + ADSR
    placeSliderCell (ui.pitchLabel, ui.pitchSlider, 0*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.attLabel,   ui.attSlider,   1*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.decLabel,   ui.decSlider,   2*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.susLabel,   ui.susSlider,   3*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.relLabel,   ui.relSlider,   4*cw5, y, cw5, h5, lw5);
    y += h5 + rowGap;

    // Row 8: Filter + FX
    placeSliderCell (ui.fFreqLabel,  ui.fFreqSlider,  0*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.fResLabel,   ui.fResSlider,   1*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.driveLabel,  ui.driveSlider,  2*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.rvbMixLabel, ui.rvbMixSlider, 3*cw5, y, cw5, h5, lw5);
    placeSliderCell (ui.rvbSzLabel,  ui.rvbSzSlider,  4*cw5, y, cw5, h5, lw5);
    y += h5 + rowGap;

    // Row 9: RndFX%  | Randomize FX Now button
    int rndBtnW = 140;
    placeSliderCell (ui.rndFxLabel, ui.rndFxSlider, 0, y, w - rndBtnW - 6, h5, 56);
    ui.rndFxFireBtn.setBounds (w - rndBtnW - 2, y, rndBtnW, h5);
    ui.rndFxFireBtn.setVisible (true);
}

//==============================================================================
// paint
//==============================================================================
void W2SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kBg));

    // Tab highlight — active tab gets a strong bottom border + fill
    for (int t = 0; t < kNumTabs; ++t)
    {
        bool active = (t == activeTab);
        tabButtons[t].setColour (juce::TextButton::buttonColourId,
                                 juce::Colour (active ? kActive : kPanel));
        if (active)
        {
            g.setColour (juce::Colour (kAccent));
            auto b = tabButtons[t].getBounds();
            g.fillRect (b.getX(), b.getBottom() - 3, b.getWidth(), 3);
        }
    }

    // Global bar separator line
    g.setColour (juce::Colour (kTrack));
    g.drawHorizontalLine (60, 0.0f, (float)getWidth());

    if (activeTab == 0)
    {
        drawMasterTab (g);
        return;
    }

    int v = activeTab - 1;
    auto& ui = voiceUI[v];

    // Phasor circle in clock row
    if (!ui.phasorRect.isEmpty())
        drawPhasor (g, ui.phasorRect, v, juce::Colour (kAccent));

    // Step grid
    if (!ui.gridRect.isEmpty())
        drawStepGrid (g, ui.gridRect, v);

    // Faint separator under waveform
    g.setColour (juce::Colour (kPanel));
    if (!ui.waveform.getBounds().isEmpty())
        g.drawHorizontalLine (ui.waveform.getBottom() + 1, 0.0f, (float)getWidth());

    // Row labels on left edge
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.5f)));
    if (!ui.phasorRect.isEmpty())
    {
        int ry = ui.phasorRect.getY();
        g.drawText ("CLOCK SRC", ui.phasorRect.getRight() + 2, ry, 60, 10,
                    juce::Justification::centredLeft, false);
    }
}

//==============================================================================
void W2SamplerEditor::drawMasterTab (juce::Graphics& g)
{
    if (masterPhasorArea_.isEmpty()) return;

    int  cy      = masterPhasorArea_.getY() + 30;
    int  diam    = 100;
    int  spacing = 130;
    int  cx0     = masterPhasorArea_.getCentreX() - spacing;

    const juce::Colour cols[] = { juce::Colour(kAccent),
                                   juce::Colour(0xff4488CC),
                                   juce::Colour(0xff9C27B0) };
    const char* vnames[] = { "Voice 1", "Voice 2", "Voice 3" };

    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (12.0f)));
    for (int i = 0; i < 3; ++i)
    {
        juce::Rectangle<int> pRect { cx0 + i * spacing, cy, diam, diam };
        drawPhasor (g, pRect, i, cols[i]);
        g.setColour (juce::Colour (kTextDim));
        g.drawText (vnames[i], cx0 + i*spacing, cy + diam + 8, diam, 16,
                    juce::Justification::centred);
        // Show mute/solo state
        bool muted = proc.getVoiceMuted (i);
        bool soloed = proc.getSoloVoice() == i;
        juce::String st = muted ? "MUTED" : (soloed ? "SOLO" : "");
        if (!st.isEmpty())
        {
            g.setColour (muted ? juce::Colour (kMute) : juce::Colour (kSolo));
            g.drawText (st, cx0 + i*spacing, cy + diam + 26, diam, 14,
                        juce::Justification::centred);
        }
    }

    // Draw "bar clock" info
    int cdiv = proc.clkDiv ? proc.clkDiv->get() : 4;
    double bpmVal = proc.bpm ? (double)proc.bpm->get() : 120.0;
    double cycleSeconds = 60.0 / bpmVal * cdiv;
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (11.0f)));
    g.drawText ("Phasor cycle: " + juce::String (cdiv) + " beats = "
                + juce::String (cycleSeconds, 2) + "s",
                0, masterPhasorArea_.getY() + diam + 60, getWidth(), 20,
                juce::Justification::centred);
}

//==============================================================================
void W2SamplerEditor::drawPhasor (juce::Graphics& g,
                                   juce::Rectangle<int> b,
                                   int v, juce::Colour col)
{
    g.setColour (col.withAlpha (0.12f));
    g.fillEllipse (b.toFloat());
    g.setColour (col.withAlpha (0.6f));
    g.drawEllipse (b.toFloat().reduced (1.5f), 1.5f);

    double phase = proc.getVoice(v).getTransformedPhase();
    float  angle = juce::MathConstants<float>::twoPi * (float)phase
                   - juce::MathConstants<float>::halfPi;
    float  r     = b.getWidth() / 2.0f - 5.0f;
    float  xc    = (float)b.getCentreX();
    float  yc    = (float)b.getCentreY();
    g.setColour (col);
    g.drawLine (xc, yc, xc + r * std::cos(angle), yc + r * std::sin(angle), 2.0f);
    g.fillEllipse (xc - 2.5f, yc - 2.5f, 5.0f, 5.0f);
}

//==============================================================================
void W2SamplerEditor::drawStepGrid (juce::Graphics& g, juce::Rectangle<int> b, int v)
{
    auto& seq     = proc.getVoice(v).getSequencer();
    int   steps   = seq.getSteps();
    int   cur     = proc.getVoice(v).getCurrentStep();
    bool  running = proc.getPlaying();
    if (steps <= 0) return;

    g.setColour (juce::Colour (kPanel));
    g.fillRect (b);

    float cw = (float)b.getWidth() / (float)steps;
    float ch = (float)b.getHeight() - 4.0f;

    for (int s = 0; s < steps; ++s)
    {
        bool hit    = seq.getStepValue (s);
        bool active = (s == cur) && running;

        juce::Colour col;
        if (active && hit)  col = juce::Colour (0xff444444);  // active hit: dark
        else if (active)    col = juce::Colour (0xffB8D4B8);  // active no-hit: green
        else if (hit)       col = juce::Colour (0xff888888);  // hit: medium grey
        else                col = juce::Colour (0xffEEEEEE);  // no-hit: near white

        float x1 = (float)b.getX() + s * cw + 1.0f;
        g.setColour (col);
        g.fillRect (x1, (float)b.getY() + 2.0f, cw - 2.0f, ch);
    }

    // Hit/step count label
    g.setColour (juce::Colour (kTextDim));
    g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (9.0f)));
    g.drawText (juce::String (seq.getHits()) + "/" + juce::String (steps) + " hits",
                b.getRight() - 58, b.getY(), 56, b.getHeight(),
                juce::Justification::centredRight, false);
}

//==============================================================================
// timerCallback
//==============================================================================
void W2SamplerEditor::timerCallback()
{
    proc.checkAndFireRandomizations();

    // Sync BPM
    if (proc.bpm)
        bpmSlider.setValue ((double)proc.bpm->get(), juce::dontSendNotification);
    playBtn.setButtonText (proc.getPlaying() ? "Stop" : "Play");
    playBtn.setColour (juce::TextButton::buttonColourId,
        juce::Colour (proc.getPlaying() ? kActive : kPanel));

    // Sync clock div buttons
    if (proc.clkDiv)
    {
        int cur = proc.clkDiv->get();
        for (int i = 0; i < kNumClkDivs; ++i)
            clkDivBtns[i].setColour (juce::TextButton::buttonColourId,
                juce::Colour (kClkDivVals[i] == cur ? kActive : kPanel));
    }

    // Sync mute/solo button colours
    for (int v = 0; v < 3; ++v)
    {
        muteBtn[v].setColour (juce::TextButton::buttonColourId,
            juce::Colour (proc.getVoiceMuted (v) ? kMute : kPanel));
        soloBtn[v].setColour (juce::TextButton::buttonColourId,
            juce::Colour (proc.getSoloVoice() == v ? kSolo : kPanel));
    }

    // Sync active voice tab
    if (activeTab > 0)
        syncVoiceFromParams (activeTab - 1);

    // Repaint dynamic areas
    if (activeTab == 0)
        repaint (masterPhasorArea_);
    else
    {
        int v = activeTab - 1;
        repaint (voiceUI[v].phasorRect.expanded (4));
        repaint (voiceUI[v].gridRect);
    }
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
    if (entry && entry->onsetsAnalysed && entry->onsets.count > 0)
        name += "  |  " + juce::String (entry->onsets.count)
             + " onsets  ~" + juce::String (entry->onsets.estimatedBPM, 1) + " bpm";
    ui.nameLabel.setText (name, d);

    // Rate slider + preset highlight
    float curRate = p.rate->get();
    ui.rateSlider.setValue ((double)curRate, d);
    for (int i = 0; i < kNumRatePresets; ++i)
    {
        bool active = std::abs (kRatePresets[i].value - curRate) < 0.01f;
        ui.ratePresetBtns[i].setColour (juce::TextButton::buttonColourId,
            juce::Colour (active ? kActive : kPanel));
    }

    ui.offsetSlider.setValue ((double)p.phaseOffset->get(), d);
    ui.warpSlider.setValue   ((double)p.warp->get(), d);
    ui.quantSlider.setValue  ((double)p.quantiseAmt->get(), d);
    ui.stepsSlider.setValue  ((double)p.seqSteps->get(), d);
    ui.hitsSlider.setValue   ((double)p.seqHits->get(), d);
    ui.rotSlider.setValue    ((double)p.seqRotation->get(), d);
    ui.pitchSlider.setValue  ((double)p.pitch->get(), d);
    ui.attSlider.setValue    ((double)p.attack->get(), d);
    ui.decSlider.setValue    ((double)p.decay->get(), d);
    ui.susSlider.setValue    ((double)p.sustain->get(), d);
    ui.relSlider.setValue    ((double)p.release->get(), d);
    ui.fFreqSlider.setValue  ((double)p.filterFreq->get(), d);
    ui.fResSlider.setValue   ((double)p.filterRes->get(), d);
    ui.driveSlider.setValue  ((double)p.distDrive->get(), d);
    ui.rvbMixSlider.setValue ((double)p.reverbMix->get(), d);
    ui.rvbSzSlider.setValue  ((double)p.reverbSize->get(), d);
    ui.gainSlider.setValue   ((double)p.gain->get(), d);
    ui.loopMsSlider.setValue ((double)p.loopSizeMs->get(), d);
    ui.rndFxSlider.setValue  ((double)p.rndFxChance->get(), d);

    {
        bool fr = p.reverbFreeze->get();
        ui.freezeBtn.setToggleState (fr, d);
        ui.freezeBtn.setColour (juce::TextButton::buttonColourId,
                                juce::Colour (fr ? kActive : kPanel));
    }
    {
        bool lk = p.loopSizeLock->get();
        ui.loopLockBtn.setToggleState (lk, d);
        ui.loopLockBtn.setColour (juce::TextButton::buttonColourId,
                                  juce::Colour (lk ? kActive : kPanel));
    }

    // Waveform: pass sample playhead + seq loop anchor
    float playPos   = proc.getVoice(v).getPlayPositionNorm();
    float loopAnch  = proc.getVoice(v).getSeqLoopAnchorNorm();

    ui.waveform.setState (p.regionStart->get(), p.regionEnd->get(),
                          p.loopStart->get(),   p.loopEnd->get(),
                          p.loopMode->get(),     p.loopSizeLock->get(),
                          p.loopSizeMs->get(),
                          playPos, loopAnch);

    // Pass audio file info for length overlay
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

    ui.waveform.repaint();

    updateCycleBtns (v);
}

//==============================================================================
void W2SamplerEditor::updateCycleBtns (int v)
{
    const auto& p  = proc.vp[v];
    auto&       ui = voiceUI[v];
    if (!p.phaseSource) return;

    // Phase source names — "Sync Vn" explains following another voice's phase
    static const char* srcNames[]  = { "Master", "Sync V1", "Sync V2", "Sync V3" };
    static const char* advModes[]  = { "Hold",   "Seq",     "Rnd" };
    static const char* loopModes[] = { "Off",    "Fixed",   "Rnd",  "Seq" };

    ui.phaseSrcBtn.setButtonText  (srcNames  [p.phaseSource->get()]);
    ui.smpAdvBtn.setButtonText    (advModes  [p.sampleAdv->get()]);
    ui.loopModeBtn.setButtonText  (loopModes [p.loopMode->get()]);
    ui.revBtn.setButtonText       (p.reverse->get() ? "Rev: ON" : "Rev: Off");
    ui.revBtn.setColour (juce::TextButton::buttonColourId,
        p.reverse->get() ? juce::Colour (kActive) : juce::Colour (kPanel));
}
