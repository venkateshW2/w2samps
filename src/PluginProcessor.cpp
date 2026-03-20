#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
void W2SamplerProcessor::registerVoiceParams (int v, const juce::String& px)
{
    auto& p = vp[v];

    auto logR = [] (float mn, float mx) {
        return juce::NormalisableRange<float> (mn, mx, 0.0f, 0.3f);
    };

    // Phase source + rate
    addParameter (p.phaseSource  = new juce::AudioParameterInt   (px+"phSrc",    "Phase Src",     0,  3,   0));
    addParameter (p.rate         = new juce::AudioParameterFloat (px+"rate",     "Rate",
                                       juce::NormalisableRange<float> (0.125f, 8.0f, 0.0f, 0.315f), 1.0f));

    // Phase transform
    addParameter (p.phaseOffset  = new juce::AudioParameterFloat (px+"phOff",    "Ph Offset",  {0.0f, 1.0f},   0.0f));
    addParameter (p.warp         = new juce::AudioParameterFloat (px+"warp",     "Warp",       {-1.0f, 1.0f},  0.0f));
    addParameter (p.reverse      = new juce::AudioParameterBool  (px+"rev",      "Reverse",    false));
    addParameter (p.quantiseAmt  = new juce::AudioParameterFloat (px+"quant",    "Quantise",   {0.0f, 1.0f},   0.0f));

    // Euclidean
    addParameter (p.seqSteps     = new juce::AudioParameterInt   (px+"steps",    "Steps",      1, 32, 16));
    addParameter (p.seqHits      = new juce::AudioParameterInt   (px+"hits",     "Hits",       0, 32,  4));
    addParameter (p.seqRotation  = new juce::AudioParameterInt   (px+"rot",      "Rotation",   0, 31,  0));
    addParameter (p.sampleAdv    = new juce::AudioParameterInt   (px+"smpAdv",   "Sample Adv", 0,  2,  0));
    addParameter (p.rndFxChance  = new juce::AudioParameterFloat (px+"rndFx",    "Rnd FX %",   {0.0f, 1.0f},   0.0f));

    // Pitch + ADSR
    addParameter (p.pitch        = new juce::AudioParameterFloat (px+"pitch",    "Pitch",
                                       juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    addParameter (p.attack       = new juce::AudioParameterFloat (px+"att",      "Attack",   logR (0.001f, 2.0f), 0.005f));
    addParameter (p.decay        = new juce::AudioParameterFloat (px+"dec",      "Decay",    logR (0.001f, 2.0f), 0.1f));
    addParameter (p.sustain      = new juce::AudioParameterFloat (px+"sus",      "Sustain",  {0.0f, 1.0f},        0.8f));
    addParameter (p.release      = new juce::AudioParameterFloat (px+"rel",      "Release",  logR (0.001f, 4.0f), 0.2f));

    // Filter
    addParameter (p.filterFreq   = new juce::AudioParameterFloat (px+"fltFreq",  "Flt Freq",
                                       juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.25f), 20000.0f));
    addParameter (p.filterRes    = new juce::AudioParameterFloat (px+"fltRes",   "Flt Res",    {0.5f, 10.0f}, 0.707f));

    // Gain structure
    addParameter (p.preGain      = new juce::AudioParameterFloat (px+"preGain",  "Pre Amp",
                                       juce::NormalisableRange<float> (0.25f, 4.0f, 0.0f, 0.5f), 1.0f));
    addParameter (p.gain         = new juce::AudioParameterFloat (px+"gain",     "Level",      {0.0f, 2.0f}, 1.0f));
    addParameter (p.limitThresh  = new juce::AudioParameterFloat (px+"limit",    "Limit dB",
                                       juce::NormalisableRange<float> (-24.0f, 0.0f), 0.0f));

    // FX
    addParameter (p.distDrive    = new juce::AudioParameterFloat (px+"drive",    "Drive",      {0.0f, 1.0f}, 0.0f));
    addParameter (p.reverbMix    = new juce::AudioParameterFloat (px+"rvbMix",   "Rvb Mix",    {0.0f, 1.0f}, 0.0f));
    addParameter (p.reverbSize   = new juce::AudioParameterFloat (px+"rvbSize",  "Rvb Size",   {0.0f, 1.0f}, 0.5f));
    addParameter (p.reverbFreeze = new juce::AudioParameterBool  (px+"freeze",   "Freeze",     false));

    // Region + Loop
    addParameter (p.regionStart  = new juce::AudioParameterFloat (px+"regSt",    "Rgn Start",  {0.0f, 1.0f}, 0.0f));
    addParameter (p.regionEnd    = new juce::AudioParameterFloat (px+"regEn",    "Rgn End",    {0.0f, 1.0f}, 1.0f));
    addParameter (p.loopMode     = new juce::AudioParameterInt   (px+"loopMode", "Loop Mode",   0, 5,  0));
    addParameter (p.loopStart    = new juce::AudioParameterFloat (px+"loopSt",   "Loop Start", {0.0f, 1.0f}, 0.0f));
    addParameter (p.loopEnd      = new juce::AudioParameterFloat (px+"loopEn",   "Loop End",   {0.0f, 1.0f}, 0.25f));
    addParameter (p.loopSizeMs   = new juce::AudioParameterFloat (px+"loopMs",   "Loop Ms",
                                       juce::NormalisableRange<float> (5.0f, 5000.0f, 0.0f, 0.3f), 100.0f));
    addParameter (p.loopSizeLock = new juce::AudioParameterBool  (px+"loopLock", "Loop Lock",  false));

    // Function generators (4 per voice)
    for (int fg = 0; fg < W2SamplerProcessor::VoiceParamPtrs::kNumFg; ++fg)
    {
        juce::String fpx = px + "fg" + juce::String (fg) + "_";
        // Rate: 0.001–32.0 (multiplier when sync, Hz when free). Log skew for usable range.
        addParameter (p.fgRateVal[fg] = new juce::AudioParameterFloat (
            fpx+"rateV", "FG Rate",
            juce::NormalisableRange<float> (0.001f, 32.0f, 0.0f, 0.3f), 1.0f));
        addParameter (p.fgSync[fg]    = new juce::AudioParameterBool  (fpx+"sync",  "FG Sync",  true));
        addParameter (p.fgDest[fg]    = new juce::AudioParameterInt   (fpx+"dest",  "FG Dest",  0, kNumModDests - 1, 0));
        addParameter (p.fgDepth[fg]   = new juce::AudioParameterFloat (fpx+"depth", "FG Depth", {-1.0f, 1.0f}, 0.0f));
        addParameter (p.fgMin[fg]     = new juce::AudioParameterFloat (fpx+"min",   "FG Min",   {0.0f, 1.0f}, 0.0f));
        addParameter (p.fgMax[fg]     = new juce::AudioParameterFloat (fpx+"max",   "FG Max",   {0.0f, 1.0f}, 1.0f));
    }
}

//==============================================================================
W2SamplerProcessor::W2SamplerProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager_.registerBasicFormats();
    addParameter (bpm = new juce::AudioParameterFloat ("g_bpm", "BPM",
                             juce::NormalisableRange<float> (20.0f, 999.0f, 0.01f, 0.5f), 120.0f));
    // clkDiv: beats per phasor cycle. 1=beat, 2=half-bar, 4=1bar, 8=2bars.
    addParameter (clkDiv = new juce::AudioParameterInt ("g_clkDiv", "Beats/Cycle", 1, 8, 4));
    addParameter (masterGain = new juce::AudioParameterFloat ("g_masterGain", "Master Gain",
                                   juce::NormalisableRange<float> (0.0f, 2.0f, 0.0f, 0.5f), 0.7f));
    for (int i = 0; i < 3; ++i) voiceMuted_[i].store (false);
    registerVoiceParams (0, "v0_");
    registerVoiceParams (1, "v1_");
    registerVoiceParams (2, "v2_");
}

W2SamplerProcessor::~W2SamplerProcessor() {}

//==============================================================================
void W2SamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    clock_.prepare (sampleRate);
    clock_.reset();
    clock_.setBPM (bpm ? (double) bpm->get() : 120.0);
    for (int v = 0; v < 3; ++v)
        voices_[v].prepare (sampleRate, samplesPerBlock);

    // Initialise K-weighting filters (BS.1770 coefficients at 44100 Hz, scaled)
    // Stage 1: high-shelf ~1681 Hz +4 dB
    for (int ch = 0; ch < 2; ++ch)
    {
        kw1_[ch] = { 1.53512485958697f, -2.69169618940638f, 1.19839281085285f,
                     -1.69065929318241f, 0.73248077421585f };
        kw1_[ch].reset();
        // Stage 2: RLB high-pass ~38 Hz
        kw2_[ch] = { 1.0f, -2.0f, 1.0f,
                     -1.99004745483398f, 0.99007225036603f };
        kw2_[ch].reset();
    }
    std::fill (std::begin (lufsBlockBuf_), std::end (lufsBlockBuf_), 0.0f);
    lufsBlockPos_ = 0;  lufsBlockFill_ = 0;
}

//==============================================================================
void W2SamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (!isPlaying_.load()) { midi.clear(); return; }

    clock_.setBPM (bpm ? (double) bpm->get() : 120.0);
    clock_.beatsPerCycle = clkDiv ? (double) clkDiv->get() : 4.0;

    int numSamples = buffer.getNumSamples();
    double masterPhase = clock_.phaseAfter (numSamples);
    clock_.phase = masterPhase;

    // Determine effective mute per voice (mute overrides solo; solo mutes others)
    int  solo     = soloVoice_.load();
    bool anySolo  = (solo >= 0 && solo < 3);

    // Tick all active timeline envelopes and collect their outputs
    struct TlOut { int voice; int dest; float norm; float depth; };
    std::array<TlOut, kMaxTimelines * TimelineEnv::kMaxDests> tlOuts;
    int tlOutCount = 0;
    for (int t = 0; t < kMaxTimelines; ++t)
    {
        auto& tl = timelines_[t];
        if (!tl.isActive()) continue;
        tl.tick (isPlaying_.load(), numSamples, sampleRate_);
        float raw = tl.evaluate();
        for (const auto& d : tl.getActiveDests())
        {
            float norm = d.min + raw * (d.max - d.min);
            if (tlOutCount < (int) tlOuts.size())
                tlOuts[(size_t) tlOutCount++] = { d.voice, d.dest, norm, d.depth };
        }
    }

    VoiceChannel::Params vcp;
    for (int v = 0; v < 3; ++v)
    {
        bool muted = voiceMuted_[v].load() || (anySolo && solo != v);
        voices_[v].setMuted (muted);

        fillVoiceParams (v, vcp);

        // Populate external mods from timeline outputs targeting this voice
        int emIdx = 0;
        for (int t = 0; t < tlOutCount && emIdx < VoiceChannel::Params::kMaxExtMods; ++t)
        {
            const auto& tlo = tlOuts[(size_t) t];
            if (tlo.voice != v) continue;
            vcp.extMods[emIdx++] = { tlo.dest, tlo.norm, tlo.depth };
        }
        // Clear remaining ext mod slots
        for (int i = emIdx; i < VoiceChannel::Params::kMaxExtMods; ++i)
            vcp.extMods[i] = {};

        double inputPhase = selectInputPhase (v, masterPhase);
        voices_[v].processBlock (inputPhase, vcp, buffer, 0, numSamples);
    }

    // Apply master gain
    float mg = masterGain ? masterGain->get() : 0.7f;
    if (std::abs (mg - 1.0f) > 0.001f)
        buffer.applyGain (mg);

    // Track output peaks (fast attack, UI reads these for meter)
    float pL = 0.0f, pR = 0.0f;
    int nCh = buffer.getNumChannels();
    for (int s = 0; s < numSamples; ++s)
    {
        if (nCh > 0) pL = std::max (pL, std::abs (buffer.getSample (0, s)));
        if (nCh > 1) pR = std::max (pR, std::abs (buffer.getSample (1, s)));
    }
    outputPeakL_.store (std::max (outputPeakL_.load (std::memory_order_relaxed) * 0.9999f, pL),
                        std::memory_order_relaxed);
    outputPeakR_.store (std::max (outputPeakR_.load (std::memory_order_relaxed) * 0.9999f, pR),
                        std::memory_order_relaxed);

    // ── Short-term LUFS (BS.1770 K-weighting) ────────────────────────────────
    {
        float sumSq = 0.0f;
        int   nCh2  = std::min (buffer.getNumChannels(), 2);
        for (int s = 0; s < numSamples; ++s)
            for (int ch = 0; ch < nCh2; ++ch)
            {
                float v = kw2_[ch].process (kw1_[ch].process (buffer.getSample (ch, s)));
                sumSq += v * v;
            }
        float meanSq = (numSamples > 0 && nCh2 > 0)
            ? sumSq / (float)(numSamples * nCh2) : 0.0f;

        lufsBlockBuf_[lufsBlockPos_] = meanSq;
        lufsBlockPos_ = (lufsBlockPos_ + 1) % 64;
        if (lufsBlockFill_ < 64) ++lufsBlockFill_;

        // Mean over filled blocks
        int   n64  = (int) lufsBlockFill_;
        float mean = 0.0f;
        for (int i = 0; i < n64; ++i) mean += lufsBlockBuf_[i];
        mean /= (float) std::max (1, n64);
        float lufs = mean > 1e-10f ? (-0.691f + 10.0f * std::log10 (mean)) : -70.0f;
        shortTermLufs_.store (lufs, std::memory_order_relaxed);
    }

    midi.clear();
}

//==============================================================================
double W2SamplerProcessor::selectInputPhase (int v, double masterPhase) const
{
    int src = vp[v].phaseSource ? vp[v].phaseSource->get() : 0;
    if (src == 1) return voices_[0].getTransformedPhase();
    if (src == 2) return voices_[1].getTransformedPhase();
    if (src == 3) return voices_[2].getTransformedPhase();
    return masterPhase;
}

//==============================================================================
void W2SamplerProcessor::fillVoiceParams (int v, VoiceChannel::Params& out) const
{
    const auto& p = vp[v];
    if (!p.phaseSource) return;

    out.phaseSource    = static_cast<VoiceChannel::PhaseSource> (p.phaseSource->get());
    out.rate           = p.rate->get();
    out.phaseOffset    = p.phaseOffset->get();
    out.warp           = p.warp->get();
    out.reverse        = p.reverse->get();
    out.quantiseAmount = p.quantiseAmt->get();
    out.seqSteps       = p.seqSteps->get();
    out.seqHits        = p.seqHits->get();
    out.seqRotation    = p.seqRotation->get();
    out.sampleAdvance  = static_cast<VoiceChannel::SampleAdvMode> (p.sampleAdv->get());
    out.rndFxChance    = p.rndFxChance->get();

    auto& g = out.granular;
    g.pitchSemitones = p.pitch->get();
    g.attackSec      = p.attack->get();
    g.decaySec       = p.decay->get();
    g.sustain        = p.sustain->get();
    g.releaseSec     = p.release->get();
    g.filterFreqHz   = p.filterFreq->get();
    g.filterRes      = p.filterRes->get();
    g.preGain        = p.preGain->get();
    g.gain           = p.gain->get();
    g.limitThreshDb  = p.limitThresh->get();
    g.distDrive      = p.distDrive->get();
    g.reverbMix      = p.reverbMix->get();
    g.reverbSize     = p.reverbSize->get();
    g.reverbFreeze   = p.reverbFreeze->get();
    g.regionStart    = p.regionStart->get();
    g.regionEnd      = p.regionEnd->get();
    g.loopMode       = static_cast<GranularVoice::LoopMode> (p.loopMode->get());
    g.loopStart      = p.loopStart->get();
    g.loopEnd        = p.loopEnd->get();
    g.loopSizeMs     = p.loopSizeMs->get();
    g.loopSizeLock   = p.loopSizeLock->get();

    // FuncGen mod routing
    for (int fg = 0; fg < W2SamplerProcessor::VoiceParamPtrs::kNumFg; ++fg)
    {
        if (!p.fgRateVal[fg]) continue;
        out.fgRateVal[fg] = p.fgRateVal[fg]->get();
        out.fgSync[fg]    = p.fgSync[fg]->get();
        out.fgDest[fg]    = p.fgDest[fg]->get();
        out.fgDepth[fg]   = p.fgDepth[fg]->get();
        out.fgMin[fg]     = p.fgMin[fg]->get();
        out.fgMax[fg]     = p.fgMax[fg]->get();
    }
}

//==============================================================================
// Message-thread API
//==============================================================================
void W2SamplerProcessor::loadFolder (int v, const juce::File& folder)
{
    if (v < 0 || v > 2) return;
    voices_[v].loadFolder (folder, formatManager_);
    voices_[v].getLibrary().analyseAllOnsets (0.5f);

    // Reset region to full buffer, loop to first 25% — makes handles visually distinct
    auto& p = vp[v];
    *p.regionStart = 0.0f;  *p.regionEnd = 1.0f;
    *p.loopStart   = 0.0f;  *p.loopEnd   = 0.25f;
}

void W2SamplerProcessor::prevSample   (int v) { if (v>=0&&v<3) voices_[v].prevSample(); }
void W2SamplerProcessor::nextSample   (int v) { if (v>=0&&v<3) voices_[v].nextSample(); }
void W2SamplerProcessor::randomSample (int v) { if (v>=0&&v<3) voices_[v].randomSample(); }

void W2SamplerProcessor::reanalyseOnsets (int v, float sensitivity)
{
    if (v < 0 || v > 2) return;
    auto& lib = voices_[v].getLibrary();
    for (int i = 0; i < lib.getCount(); ++i)
        lib.analyseOnsets (i, sensitivity, /*forceRerun=*/true);
}

void W2SamplerProcessor::randomizeVoiceParams (int v, const bool* locked)
{
    // locked[10]: pitch(0) attack(1) decay(2) sustain(3) release(4)
    //             filterFreq(5) filterRes(6) distDrive(7) reverbMix(8) reverbSize(9)
    if (v < 0 || v > 2) return;
    auto& p   = vp[v];
    auto& rng = juce::Random::getSystemRandom();
    auto skip = [&] (int i) { return locked != nullptr && locked[i]; };

    if (!skip (0)) *p.pitch      = rng.nextFloat() * 24.0f - 12.0f;
    if (!skip (1)) *p.attack     = 0.001f + rng.nextFloat() * 0.499f;
    if (!skip (2)) *p.decay      = 0.01f  + rng.nextFloat() * 0.490f;
    if (!skip (3)) *p.sustain    = 0.3f   + rng.nextFloat() * 0.7f;
    if (!skip (4)) *p.release    = 0.05f  + rng.nextFloat() * 0.950f;
    if (!skip (5)) *p.filterFreq = 200.0f + rng.nextFloat() * 15800.0f;
    if (!skip (6)) *p.filterRes  = 0.5f   + rng.nextFloat() * 3.0f;
    if (!skip (7)) *p.distDrive  = rng.nextFloat() * 0.6f;
    if (!skip (8)) *p.reverbMix  = rng.nextFloat() * 0.7f;
    if (!skip (9)) *p.reverbSize = 0.2f   + rng.nextFloat() * 0.8f;
}

void W2SamplerProcessor::resetVoiceFX (int v)
{
    if (v < 0 || v > 2) return;
    auto& p = vp[v];
    *p.pitch       = 0.0f;
    *p.attack      = 0.005f;
    *p.decay       = 0.1f;
    *p.sustain     = 0.8f;
    *p.release     = 0.2f;
    *p.filterFreq  = 20000.0f;
    *p.filterRes   = 0.707f;
    *p.distDrive   = 0.0f;
    *p.reverbMix   = 0.0f;
    *p.reverbSize  = 0.5f;
    *p.reverbFreeze = false;
    *p.preGain     = 1.0f;
    *p.gain        = 1.0f;
    *p.limitThresh = 0.0f;
}

void W2SamplerProcessor::saveVoicePreset (int v, int slot)
{
    if (v < 0 || v > 2 || slot < 0 || slot > 7) return;
    const auto& p = vp[v];
    auto& pr = presets_[v][slot];
    pr.valid      = true;
    pr.pitch      = p.pitch->get();
    pr.attack     = p.attack->get();
    pr.decay      = p.decay->get();
    pr.sustain    = p.sustain->get();
    pr.release_   = p.release->get();
    pr.filterFreq = p.filterFreq->get();
    pr.filterRes  = p.filterRes->get();
    pr.distDrive  = p.distDrive->get();
    pr.reverbMix  = p.reverbMix->get();
    pr.reverbSize = p.reverbSize->get();
    pr.preGain    = p.preGain->get();
    pr.gain       = p.gain->get();
    pr.limitThresh = p.limitThresh->get();
}

void W2SamplerProcessor::loadVoicePreset (int v, int slot)
{
    if (v < 0 || v > 2 || slot < 0 || slot > 7) return;
    const auto& pr = presets_[v][slot];
    if (!pr.valid) return;
    auto& p = vp[v];
    *p.pitch       = pr.pitch;
    *p.attack      = pr.attack;
    *p.decay       = pr.decay;
    *p.sustain     = pr.sustain;
    *p.release     = pr.release_;
    *p.filterFreq  = pr.filterFreq;
    *p.filterRes   = pr.filterRes;
    *p.distDrive   = pr.distDrive;
    *p.reverbMix   = pr.reverbMix;
    *p.reverbSize  = pr.reverbSize;
    *p.preGain     = pr.preGain;
    *p.gain        = pr.gain;
    *p.limitThresh = pr.limitThresh;
}

bool W2SamplerProcessor::takeRandomizeFXRequest (int v)
{
    if (v < 0 || v > 2) return false;
    return voices_[v].takeRandomizeFXRequest();
}

//==============================================================================
void W2SamplerProcessor::setPlaying (bool p)
{
    if (!p) clock_.reset();
    isPlaying_.store (p);
}

void W2SamplerProcessor::setVoiceMute (int v, bool muted)
{
    if (v >= 0 && v < 3) voiceMuted_[v].store (muted);
}

void W2SamplerProcessor::setVoiceSolo (int v)
{
    // v == -1 clears solo; v == current solo also clears (toggle)
    int cur = soloVoice_.load();
    soloVoice_.store ((cur == v) ? -1 : v);
}

//==============================================================================
// State v5 (adds preGain, limitThresh, presets; backward-reads v4)
//==============================================================================
static void writeVoiceCore (juce::MemoryOutputStream& s, const W2SamplerProcessor::VoiceParamPtrs& p)
{
    s.writeInt   (p.phaseSource->get());
    s.writeFloat (p.rate->get());
    s.writeFloat (p.phaseOffset->get());
    s.writeFloat (p.warp->get());
    s.writeBool  (p.reverse->get());
    s.writeFloat (p.quantiseAmt->get());
    s.writeInt   (p.seqSteps->get());
    s.writeInt   (p.seqHits->get());
    s.writeInt   (p.seqRotation->get());
    s.writeInt   (p.sampleAdv->get());
    s.writeFloat (p.rndFxChance->get());
    s.writeFloat (p.pitch->get());
    s.writeFloat (p.attack->get());
    s.writeFloat (p.decay->get());
    s.writeFloat (p.sustain->get());
    s.writeFloat (p.release->get());
    s.writeFloat (p.filterFreq->get());
    s.writeFloat (p.filterRes->get());
    s.writeFloat (p.distDrive->get());
    s.writeFloat (p.reverbMix->get());
    s.writeFloat (p.reverbSize->get());
    s.writeBool  (p.reverbFreeze->get());
    s.writeFloat (p.gain->get());
    s.writeFloat (p.regionStart->get());
    s.writeFloat (p.regionEnd->get());
    s.writeInt   (p.loopMode->get());
    s.writeFloat (p.loopStart->get());
    s.writeFloat (p.loopEnd->get());
    s.writeFloat (p.loopSizeMs->get());
    s.writeBool  (p.loopSizeLock->get());
    // v5 additions
    s.writeFloat (p.preGain->get());
    s.writeFloat (p.limitThresh->get());
    // v9 additions — FuncGen routing (4 FGs × 6 params: rateVal float + sync bool + dest + depth + min + max)
    for (int fg = 0; fg < W2SamplerProcessor::VoiceParamPtrs::kNumFg; ++fg)
    {
        if (!p.fgRateVal[fg]) { s.writeFloat(1.0f); s.writeBool(true); s.writeInt(0); s.writeFloat(0); s.writeFloat(0); s.writeFloat(1); continue; }
        s.writeFloat (p.fgRateVal[fg]->get());
        s.writeBool  (p.fgSync[fg]->get());
        s.writeInt   (p.fgDest[fg]->get());
        s.writeFloat (p.fgDepth[fg]->get());
        s.writeFloat (p.fgMin[fg]->get());
        s.writeFloat (p.fgMax[fg]->get());
    }
}

static bool readVoiceCore (juce::MemoryInputStream& s, W2SamplerProcessor::VoiceParamPtrs& p, int ver)
{
    if (s.getNumBytesRemaining() < 4) return false;
    *p.phaseSource  = s.readInt();
    *p.rate         = s.readFloat();
    *p.phaseOffset  = s.readFloat();
    *p.warp         = s.readFloat();
    *p.reverse      = s.readBool();
    *p.quantiseAmt  = s.readFloat();
    *p.seqSteps     = s.readInt();
    *p.seqHits      = s.readInt();
    *p.seqRotation  = s.readInt();
    *p.sampleAdv    = s.readInt();
    *p.rndFxChance  = s.readFloat();
    *p.pitch        = s.readFloat();
    *p.attack       = s.readFloat();
    *p.decay        = s.readFloat();
    *p.sustain      = s.readFloat();
    *p.release      = s.readFloat();
    *p.filterFreq   = s.readFloat();
    *p.filterRes    = s.readFloat();
    *p.distDrive    = s.readFloat();
    *p.reverbMix    = s.readFloat();
    *p.reverbSize   = s.readFloat();
    *p.reverbFreeze = s.readBool();
    *p.gain         = s.readFloat();
    *p.regionStart  = s.readFloat();
    *p.regionEnd    = s.readFloat();
    *p.loopMode     = s.readInt();
    *p.loopStart    = s.readFloat();
    *p.loopEnd      = s.readFloat();
    *p.loopSizeMs   = s.readFloat();
    *p.loopSizeLock = s.readBool();
    if (ver >= 5 && s.getNumBytesRemaining() >= 8)
    {
        *p.preGain     = s.readFloat();
        *p.limitThresh = s.readFloat();
    }
    else
    {
        *p.preGain     = 1.0f;
        *p.limitThresh = 0.0f;
    }
    // v7/v8: old int-index rate format; v9: float rateVal + bool sync
    {
        const int kNFg = W2SamplerProcessor::VoiceParamPtrs::kNumFg;
        auto applyFg = [&] (int fg, float rateV, bool sync, int dest, float depth, float fmin, float fmax)
        {
            if (p.fgRateVal[fg]) *p.fgRateVal[fg] = rateV;
            if (p.fgSync[fg])    *p.fgSync[fg]    = sync;
            if (p.fgDest[fg])    *p.fgDest[fg]    = dest;
            if (p.fgDepth[fg])   *p.fgDepth[fg]   = depth;
            if (p.fgMin[fg])     *p.fgMin[fg]     = fmin;
            if (p.fgMax[fg])     *p.fgMax[fg]     = fmax;
        };
        auto idxToRateV = [&] (int idx, bool& outSync) -> float {
            outSync = (idx < 7);
            return outSync ? kFgRateMults[juce::jlimit(0,6,idx)]
                           : kFgFreeRateHz[juce::jlimit(0,6,idx-7)];
        };

        if (ver == 7 && s.getNumBytesRemaining() >= 40)
        {
            for (int fg = 0; fg < 2; ++fg) {
                int ri = s.readInt(); int dest = s.readInt();
                float depth = s.readFloat(), fmin = s.readFloat(), fmax = s.readFloat();
                bool sync; float rv = idxToRateV(ri, sync);
                applyFg(fg, rv, sync, dest, depth, fmin, fmax);
            }
        }
        else if (ver == 8 && s.getNumBytesRemaining() >= kNFg * 20)
        {
            for (int fg = 0; fg < kNFg; ++fg) {
                int ri = s.readInt(); int dest = s.readInt();
                float depth = s.readFloat(), fmin = s.readFloat(), fmax = s.readFloat();
                bool sync; float rv = idxToRateV(ri, sync);
                applyFg(fg, rv, sync, dest, depth, fmin, fmax);
            }
        }
        else if (ver >= 9 && s.getNumBytesRemaining() >= kNFg * 21)
        {
            for (int fg = 0; fg < kNFg; ++fg) {
                float rv = s.readFloat(); bool sync = s.readBool();
                int dest = s.readInt();
                float depth = s.readFloat(), fmin = s.readFloat(), fmax = s.readFloat();
                applyFg(fg, rv, sync, dest, depth, fmin, fmax);
            }
        }
    }
    return true;
}

void W2SamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream s (destData, true);
    s.writeByte (9);  // version 9: FG rate = continuous float + sync bool (was int index)
    s.writeFloat (bpm->get());
    s.writeInt   (clkDiv->get());
    s.writeFloat (masterGain->get());
    for (int v = 0; v < 3; ++v)
        writeVoiceCore (s, vp[v]);

    // Presets
    for (int v = 0; v < 3; ++v)
        for (int slot = 0; slot < 8; ++slot)
        {
            const auto& pr = presets_[v][slot];
            s.writeBool  (pr.valid);
            if (pr.valid)
            {
                s.writeFloat (pr.pitch);
                s.writeFloat (pr.attack);
                s.writeFloat (pr.decay);
                s.writeFloat (pr.sustain);
                s.writeFloat (pr.release_);
                s.writeFloat (pr.filterFreq);
                s.writeFloat (pr.filterRes);
                s.writeFloat (pr.distDrive);
                s.writeFloat (pr.reverbMix);
                s.writeFloat (pr.reverbSize);
                s.writeFloat (pr.preGain);
                s.writeFloat (pr.gain);
                s.writeFloat (pr.limitThresh);
            }
        }

    // v8: FuncGen curve points (3 voices × 4 FGs × N points)
    for (int v = 0; v < 3; ++v)
        for (int fg = 0; fg < W2SamplerProcessor::VoiceParamPtrs::kNumFg; ++fg)
        {
            const auto& fgen = voices_[v].getFuncGen (fg);
            int nPts = fgen.serialisedPointCount();
            s.writeInt (nPts);
            for (int i = 0; i < nPts; ++i)
            {
                auto pt = fgen.serialisedPoint (i);
                s.writeFloat (pt.x);
                s.writeFloat (pt.y);
            }
        }
}

void W2SamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream s (data, (size_t) sizeInBytes, false);
    if (s.getNumBytesRemaining() < 1) return;
    int ver = (int) s.readByte();
    if (ver < 4 || ver > 9) return;  // discard older/unknown formats
    if (s.getNumBytesRemaining() < 4) return;
    *bpm    = s.readFloat();
    *clkDiv = s.readInt();
    if (ver >= 6 && s.getNumBytesRemaining() >= 4)
        *masterGain = s.readFloat();
    for (int v = 0; v < 3; ++v)
        if (!readVoiceCore (s, vp[v], ver)) return;

    // Presets (v5 only)
    if (ver >= 5)
    {
        for (int v = 0; v < 3; ++v)
            for (int slot = 0; slot < 8; ++slot)
            {
                if (s.getNumBytesRemaining() < 1) return;
                auto& pr = presets_[v][slot];
                pr.valid = s.readBool();
                if (pr.valid && s.getNumBytesRemaining() >= 52)
                {
                    pr.pitch      = s.readFloat();
                    pr.attack     = s.readFloat();
                    pr.decay      = s.readFloat();
                    pr.sustain    = s.readFloat();
                    pr.release_   = s.readFloat();
                    pr.filterFreq = s.readFloat();
                    pr.filterRes  = s.readFloat();
                    pr.distDrive  = s.readFloat();
                    pr.reverbMix  = s.readFloat();
                    pr.reverbSize = s.readFloat();
                    pr.preGain    = s.readFloat();
                    pr.gain       = s.readFloat();
                    pr.limitThresh = s.readFloat();
                }
            }
    }

    // v7+: FuncGen curve points (v7=2 FGs, v8/v9=4 FGs)
    if (ver >= 7)
    {
        int fgCurveCount = (ver >= 8) ? W2SamplerProcessor::VoiceParamPtrs::kNumFg : 2;
        for (int v = 0; v < 3; ++v)
            for (int fg = 0; fg < fgCurveCount; ++fg)
            {
                if (s.getNumBytesRemaining() < 4) break;
                int nPts = s.readInt();
                std::vector<FuncGen::Point> pts;
                pts.reserve ((size_t) nPts);
                for (int i = 0; i < nPts; ++i)
                {
                    if (s.getNumBytesRemaining() < 8) break;
                    FuncGen::Point p;
                    p.x = s.readFloat();
                    p.y = s.readFloat();
                    pts.push_back (p);
                }
                voices_[v].getFuncGen (fg).setPoints (pts);
            }
    }
}

//==============================================================================
juce::AudioProcessorEditor* W2SamplerProcessor::createEditor()
{
    return new W2SamplerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new W2SamplerProcessor();
}
