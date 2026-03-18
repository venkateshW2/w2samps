#include "PluginEditor.h"

static constexpr int kW  = 620;   // window width
static constexpr int kH  = 430;   // window height
static constexpr int kMX = 10;    // left/right margin
static constexpr int kIW = kW - kMX * 2;  // inner width = 600

// Colour palette
static const juce::Colour kBg        { 0xff1a1a2e };  // dark navy background
static const juce::Colour kPanel     { 0xff16213e };  // slightly lighter panel
static const juce::Colour kAccent    { 0xff00bcd4 };  // cyan — active steps
static const juce::Colour kHit       { 0xffffeb3b };  // yellow — playhead on hit
static const juce::Colour kCursor    { 0xff546e7a };  // steel — playhead on rest
static const juce::Colour kInactive  { 0xff263238 };  // dark — inactive step
static const juce::Colour kBorder    { 0xff37474f };  // borders
static const juce::Colour kGreen     { 0xff00e676 };  // loaded / playing
static const juce::Colour kRed       { 0xffff5252 };  // not loaded
static const juce::Colour kTextGrey  { 0xff90a4ae };  // section labels

//==============================================================================
W2SamplerEditor::W2SamplerEditor (W2SamplerProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (kW, kH);

    // ── Helper: set up a horizontal slider + its label ────────────────────────
    auto setupSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& labelText,
                                double mn, double mx, double step, double def)
    {
        s.setRange (mn, mx, step);
        s.setValue (def, juce::dontSendNotification);
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 14);
        addAndMakeVisible (s);
        l.setText (labelText, juce::dontSendNotification);
        l.setFont (juce::FontOptions (10.0f));
        l.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (l);
    };

    // ── Voice row 1: Pitch / ADSR ──────────────────────────────────────────────
    setupSlider (pitchSlider,    pitchLabel,   "Pitch",   -24.0, 24.0,  0.01, 0.0);
    setupSlider (attackSlider,   attackLabel,  "Attack",   0.001, 2.0,  0.0,  0.005);
    setupSlider (decaySlider,    decayLabel,   "Decay",    0.001, 2.0,  0.0,  0.1);
    setupSlider (sustainSlider,  sustainLabel, "Sustain",  0.0,   1.0,  0.0,  0.8);
    setupSlider (releaseSlider,  releaseLabel, "Release",  0.001, 4.0,  0.0,  0.2);

    // ── Voice row 2: Filter / FX ───────────────────────────────────────────────
    setupSlider (filterFreqSlider, filterFreqLabel, "Flt Freq", 20.0, 20000.0, 0.0, 20000.0);
    setupSlider (filterResSlider,  filterResLabel,  "Flt Res",  0.5,  10.0,    0.0, 0.707);
    setupSlider (distDriveSlider,  distDriveLabel,  "Drive",    0.0,  1.0,     0.0, 0.0);
    setupSlider (reverbMixSlider,  reverbMixLabel,  "Rvb Mix",  0.0,  1.0,     0.0, 0.0);
    setupSlider (reverbSizeSlider, reverbSizeLabel, "Rvb Size", 0.0,  1.0,     0.0, 0.5);

    // ── Gain + Freeze ──────────────────────────────────────────────────────────
    setupSlider (gainSlider, gainLabel, "Gain", 0.0, 2.0, 0.0, 1.0);
    addAndMakeVisible (freezeButton);

    // ── Sequencer sliders ──────────────────────────────────────────────────────
    setupSlider (stepsSlider,    stepsLabel,    "Steps",   1, 32,  1,  16);
    setupSlider (hitsSlider,     hitsLabel,     "Hits",    0, 32,  1,   4);
    setupSlider (rotationSlider, rotLabel,      "Rotat.",  0, 31,  1,   0);
    setupSlider (rateSlider,     rateLabel,     "Rate",    0.25, 4.0, 0.0, 1.0);

    // ── Wire sliders → parameters ──────────────────────────────────────────────
    pitchSlider.onValueChange    = [this] { *proc.pitch       = (float) pitchSlider.getValue(); };
    attackSlider.onValueChange   = [this] { *proc.attack      = (float) attackSlider.getValue(); };
    decaySlider.onValueChange    = [this] { *proc.decay       = (float) decaySlider.getValue(); };
    sustainSlider.onValueChange  = [this] { *proc.sustain     = (float) sustainSlider.getValue(); };
    releaseSlider.onValueChange  = [this] { *proc.release     = (float) releaseSlider.getValue(); };

    filterFreqSlider.onValueChange = [this] { *proc.filterFreq  = (float) filterFreqSlider.getValue(); };
    filterResSlider.onValueChange  = [this] { *proc.filterRes   = (float) filterResSlider.getValue(); };
    distDriveSlider.onValueChange  = [this] { *proc.distDrive   = (float) distDriveSlider.getValue(); };
    reverbMixSlider.onValueChange  = [this] { *proc.reverbMix   = (float) reverbMixSlider.getValue(); };
    reverbSizeSlider.onValueChange = [this] { *proc.reverbSize  = (float) reverbSizeSlider.getValue(); };

    gainSlider.onValueChange     = [this] { *proc.sampleGain  = (float) gainSlider.getValue(); };
    freezeButton.onStateChange   = [this] { *proc.reverbFreeze = freezeButton.getToggleState(); };

    stepsSlider.onValueChange    = [this] { *proc.seqSteps    = (int)   stepsSlider.getValue(); };
    hitsSlider.onValueChange     = [this] { *proc.seqHits     = (int)   hitsSlider.getValue(); };
    rotationSlider.onValueChange = [this] { *proc.seqRotation = (int)   rotationSlider.getValue(); };
    rateSlider.onValueChange     = [this] { *proc.seqRate     = (float) rateSlider.getValue(); };

    // ── File / library buttons ─────────────────────────────────────────────────
    addAndMakeVisible (loadFileButton);
    loadFileButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Load a sample",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, chooser] (const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f.existsAsFile())
                    proc.loadFolder (f.getParentDirectory());  // load the whole folder
            });
    };

    addAndMakeVisible (loadFolderButton);
    loadFolderButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Load a folder of samples",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser] (const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f.isDirectory())
                    proc.loadFolder (f);
            });
    };

    addAndMakeVisible (prevButton);
    prevButton.onClick = [this] { proc.prevSample(); };

    addAndMakeVisible (nextButton);
    nextButton.onClick = [this] { proc.nextSample(); };

    addAndMakeVisible (rndSampleButton);
    rndSampleButton.onClick = [this] { proc.randomSample(); };

    // ── Advance mode button — cycles 0→1→2→0 ──────────────────────────────────
    addAndMakeVisible (advanceModeButton);
    advanceModeButton.onClick = [this]
    {
        int next = (proc.sampleAdvanceMode->get() + 1) % 3;
        *proc.sampleAdvanceMode = next;
        updateAdvanceModeButton();
    };

    addAndMakeVisible (randomizeFXButton);
    randomizeFXButton.onClick = [this] { proc.randomizeVoiceParams(); };

    // ── Transport ──────────────────────────────────────────────────────────────
    addAndMakeVisible (playButton);
    playButton.onClick = [this]
    {
        proc.setPlaying (!proc.getPlaying());
        updatePlayButtonLabel();
    };

    updatePlayButtonLabel();
    updateAdvanceModeButton();

    startTimerHz (20);  // 20fps refresh for waveform + grid animation
}

W2SamplerEditor::~W2SamplerEditor() { stopTimer(); }

//==============================================================================
void W2SamplerEditor::timerCallback()
{
    syncAllSlidersFromParams();
    updatePlayButtonLabel();
    updateAdvanceModeButton();
    repaint();
}

void W2SamplerEditor::syncAllSlidersFromParams()
{
    auto set = [] (juce::Slider& s, double v) { s.setValue (v, juce::dontSendNotification); };

    set (pitchSlider,      proc.pitch->get());
    set (attackSlider,     proc.attack->get());
    set (decaySlider,      proc.decay->get());
    set (sustainSlider,    proc.sustain->get());
    set (releaseSlider,    proc.release->get());
    set (filterFreqSlider, proc.filterFreq->get());
    set (filterResSlider,  proc.filterRes->get());
    set (distDriveSlider,  proc.distDrive->get());
    set (reverbMixSlider,  proc.reverbMix->get());
    set (reverbSizeSlider, proc.reverbSize->get());
    set (gainSlider,       proc.sampleGain->get());

    freezeButton.setToggleState (proc.reverbFreeze->get(), juce::dontSendNotification);

    set (stepsSlider,    proc.seqSteps->get());
    set (hitsSlider,     proc.seqHits->get());
    set (rotationSlider, proc.seqRotation->get());
    set (rateSlider,     proc.seqRate->get());
}

void W2SamplerEditor::updatePlayButtonLabel()
{
    playButton.setButtonText (proc.getPlaying() ? juce::String (L"\u25a0 Stop")
                                                : juce::String (L"\u25b6 Play"));
}

void W2SamplerEditor::updateAdvanceModeButton()
{
    static const char* labels[] = { "Mode: Hold", "Mode: Seq", "Mode: Rnd" };
    advanceModeButton.setButtonText (labels[proc.sampleAdvanceMode->get()]);
}

//==============================================================================
// paint — background, waveform, section labels, step grid
//==============================================================================
void W2SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // ── Title ──────────────────────────────────────────────────────────────────
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (15.0f).withStyle ("Bold"));
    g.drawText ("W2 Sampler", kMX, 8, 130, 20, juce::Justification::centredLeft);

    // Sample name + index
    g.setFont (juce::FontOptions (10.0f));
    g.setColour (proc.hasSample() ? kGreen : kRed);
    juce::String sName = proc.hasSample()
        ? (proc.getCurrentSampleName()
           + "  [" + juce::String (proc.getLibraryIndex() + 1)
           + " / " + juce::String (proc.getLibraryCount()) + "]")
        : "no sample loaded";
    g.drawText (sName, kMX, 26, kW - 400, 14, juce::Justification::centredLeft);

    // ── Waveform ───────────────────────────────────────────────────────────────
    drawWaveform (g, { kMX, 42, kIW, 58 });

    // ── Section dividers + labels ──────────────────────────────────────────────
    g.setColour (kBorder);
    g.drawHorizontalLine (126, (float) kMX, (float)(kW - kMX));
    g.drawHorizontalLine (236, (float) kMX, (float)(kW - kMX));
    g.drawHorizontalLine (320, (float) kMX, (float)(kW - kMX));

    g.setFont (juce::FontOptions (9.0f).withStyle ("Bold"));
    g.setColour (kTextGrey);
    g.drawText ("VOICE",    kMX, 118, 60, 10, juce::Justification::centredLeft);
    g.drawText ("SEQUENCE", kMX, 228, 80, 10, juce::Justification::centredLeft);
    g.drawText ("EUCLIDEAN PATTERN", kMX, 326, 160, 10, juce::Justification::centredLeft);

    // ── Euclidean step grid ────────────────────────────────────────────────────
    drawStepGrid (g, { kMX, 338, kIW, 38 });

    // ── Playing status ─────────────────────────────────────────────────────────
    g.setFont (juce::FontOptions (10.0f));
    g.setColour (proc.getPlaying() ? kGreen : kTextGrey);
    g.drawText (proc.getPlaying() ? L"\u25cf PLAYING" : L"\u25cb STOPPED",
                kMX, 380, 100, 12, juce::Justification::centredLeft);

    // Sample advance mode reminder text
    g.setColour (kTextGrey);
    static const char* modeText[] = {
        "Each hit: same sample",
        "Each hit: next sample in folder",
        "Each hit: random sample from folder"
    };
    g.drawText (modeText[proc.sampleAdvanceMode->get()],
                120, 380, 400, 12, juce::Justification::centredLeft);
}

//==============================================================================
void W2SamplerEditor::drawWaveform (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    // Background of waveform area
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds.toFloat(), 3.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (bounds.toFloat(), 3.0f, 1.0f);

    auto* buf = proc.getCurrentSampleBuffer();
    if (buf == nullptr || buf->getNumSamples() == 0)
    {
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (kTextGrey);
        g.drawText ("no sample", bounds, juce::Justification::centred);
        return;
    }

    // Draw waveform: scan the buffer, computing min/max per pixel column.
    // This is the standard DAW waveform drawing technique.
    int   numPixels  = bounds.getWidth() - 4;
    int   srcLen     = buf->getNumSamples();
    float centreY    = bounds.getCentreY();
    float halfHeight = (float)(bounds.getHeight() - 6) * 0.5f;
    int   startX     = bounds.getX() + 2;

    g.setColour (kAccent.withAlpha (0.8f));

    for (int px = 0; px < numPixels; ++px)
    {
        // Map this pixel to a range of samples in the source buffer
        int sampleStart = (int)((double) px       / numPixels * srcLen);
        int sampleEnd   = (int)((double)(px + 1)  / numPixels * srcLen);
        sampleEnd       = std::min (sampleEnd, srcLen);
        if (sampleStart >= sampleEnd) continue;

        // Find min/max across channels and this sample range
        float mn = 1.0f, mx = -1.0f;
        for (int ch = 0; ch < buf->getNumChannels(); ++ch)
        {
            auto* data = buf->getReadPointer (ch);
            for (int s = sampleStart; s < sampleEnd; ++s)
            {
                mn = std::min (mn, data[s]);
                mx = std::max (mx, data[s]);
            }
        }

        // Draw a vertical line from min to max (clamp to ±1)
        float yTop    = centreY - std::clamp (mx, -1.0f, 1.0f) * halfHeight;
        float yBottom = centreY - std::clamp (mn, -1.0f, 1.0f) * halfHeight;
        if (yBottom - yTop < 1.0f) yBottom = yTop + 1.0f;  // always draw at least 1px

        g.drawLine ((float)(startX + px), yTop, (float)(startX + px), yBottom, 1.0f);
    }

    // Centre line
    g.setColour (kBorder);
    g.drawHorizontalLine ((int) centreY, (float)(startX), (float)(startX + numPixels));
}

//==============================================================================
void W2SamplerEditor::drawStepGrid (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    auto pattern     = proc.getCurrentPattern();
    int  currentStep = proc.getCurrentStep();
    int  numSteps    = (int) pattern.size();
    if (numSteps == 0) return;

    float cellW = (float) bounds.getWidth() / (float) numSteps;

    for (int i = 0; i < numSteps; ++i)
    {
        juce::Rectangle<float> cell (
            bounds.getX() + (float) i * cellW + 1.0f,
            (float) bounds.getY(),
            cellW - 2.0f,
            (float) bounds.getHeight());

        bool isHit     = pattern[(size_t) i];
        bool isCurrent = (i == currentStep) && proc.getPlaying();

        juce::Colour fill;
        if      (isCurrent && isHit) fill = kHit;
        else if (isCurrent)          fill = kCursor;
        else if (isHit)              fill = kAccent;
        else                         fill = kInactive;

        g.setColour (fill);
        g.fillRoundedRectangle (cell, 3.0f);
        g.setColour (kBorder);
        g.drawRoundedRectangle (cell, 3.0f, 1.0f);
    }
}

//==============================================================================
// resized — set pixel positions of all components
//==============================================================================
void W2SamplerEditor::resized()
{
    // ── Top bar ────────────────────────────────────────────────────────────────
    // Right-aligned: [Load Folder] [Load File] [▶ Play]
    loadFolderButton.setBounds (kW - 130,  6, 120, 22);
    loadFileButton  .setBounds (kW - 260,  6, 120, 22);
    playButton      .setBounds (kW - 380,  6,  90, 22);

    // ── Library navigation row (y=102) ─────────────────────────────────────────
    prevButton       .setBounds (kMX,       102,  86, 22);
    nextButton       .setBounds (kMX + 90,  102,  86, 22);
    rndSampleButton  .setBounds (kMX + 180, 102,  96, 22);
    advanceModeButton.setBounds (kMX + 280, 102, 118, 22);
    randomizeFXButton.setBounds (kW - 130,  102, 120, 22);

    // ── 5-column voice slider layout ───────────────────────────────────────────
    // Available width 600px / 5 cols = 120px each.
    // Each cell: label (y=col_label_y, h=12) + slider (y=col_slider_y, h=18)
    const int lw5 = 116;   // cell width for 5-col (with 4px padding each side)
    const int sw5 = 116;

    auto place5 = [&] (juce::Label& l, juce::Slider& s, int col, int rowLabelY, int rowSliderY) {
        int x = kMX + col * 120;
        l.setBounds (x, rowLabelY, lw5, 12);
        s.setBounds (x, rowSliderY, sw5, 18);
    };

    // Row 1: Pitch | Attack | Decay | Sustain | Release
    place5 (pitchLabel,    pitchSlider,   0, 130, 142);
    place5 (attackLabel,   attackSlider,  1, 130, 142);
    place5 (decayLabel,    decaySlider,   2, 130, 142);
    place5 (sustainLabel,  sustainSlider, 3, 130, 142);
    place5 (releaseLabel,  releaseSlider, 4, 130, 142);

    // Row 2: FltFreq | FltRes | Drive | RvbMix | RvbSize
    place5 (filterFreqLabel, filterFreqSlider, 0, 164, 176);
    place5 (filterResLabel,  filterResSlider,  1, 164, 176);
    place5 (distDriveLabel,  distDriveSlider,  2, 164, 176);
    place5 (reverbMixLabel,  reverbMixSlider,  3, 164, 176);
    place5 (reverbSizeLabel, reverbSizeSlider, 4, 164, 176);

    // Gain + Freeze row (y=200)
    gainLabel  .setBounds (kMX,       200,  50, 14);
    gainSlider .setBounds (kMX + 54,  200, 180, 18);
    freezeButton.setBounds(kMX + 242, 198,  80, 22);

    // ── 4-column sequencer slider layout ───────────────────────────────────────
    // 600px / 4 cols = 150px each
    const int lw4 = 146;
    auto place4 = [&] (juce::Label& l, juce::Slider& s, int col, int rowLabelY, int rowSliderY) {
        int x = kMX + col * 150;
        l.setBounds (x, rowLabelY, lw4, 12);
        s.setBounds (x, rowSliderY, lw4, 18);
    };

    place4 (stepsLabel,    stepsSlider,    0, 242, 254);
    place4 (hitsLabel,     hitsSlider,     1, 242, 254);
    place4 (rotLabel,      rotationSlider, 2, 242, 254);
    place4 (rateLabel,     rateSlider,     3, 242, 254);
}
