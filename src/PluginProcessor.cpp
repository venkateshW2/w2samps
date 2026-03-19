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

    // FX
    addParameter (p.distDrive    = new juce::AudioParameterFloat (px+"drive",    "Drive",      {0.0f, 1.0f}, 0.0f));
    addParameter (p.reverbMix    = new juce::AudioParameterFloat (px+"rvbMix",   "Rvb Mix",    {0.0f, 1.0f}, 0.0f));
    addParameter (p.reverbSize   = new juce::AudioParameterFloat (px+"rvbSize",  "Rvb Size",   {0.0f, 1.0f}, 0.5f));
    addParameter (p.reverbFreeze = new juce::AudioParameterBool  (px+"freeze",   "Freeze",     false));
    addParameter (p.gain         = new juce::AudioParameterFloat (px+"gain",     "Gain",       {0.0f, 2.0f}, 1.0f));

    // Region + Loop
    addParameter (p.regionStart  = new juce::AudioParameterFloat (px+"regSt",    "Rgn Start",  {0.0f, 1.0f}, 0.0f));
    addParameter (p.regionEnd    = new juce::AudioParameterFloat (px+"regEn",    "Rgn End",    {0.0f, 1.0f}, 1.0f));
    addParameter (p.loopMode     = new juce::AudioParameterInt   (px+"loopMode", "Loop Mode",   0, 3,  0));
    addParameter (p.loopStart    = new juce::AudioParameterFloat (px+"loopSt",   "Loop Start", {0.0f, 1.0f}, 0.0f));
    addParameter (p.loopEnd      = new juce::AudioParameterFloat (px+"loopEn",   "Loop End",   {0.0f, 1.0f}, 1.0f));
    addParameter (p.loopSizeMs   = new juce::AudioParameterFloat (px+"loopMs",   "Loop Ms",
                                       juce::NormalisableRange<float> (5.0f, 5000.0f, 0.0f, 0.3f), 100.0f));
    addParameter (p.loopSizeLock = new juce::AudioParameterBool  (px+"loopLock", "Loop Lock",  false));
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

    VoiceChannel::Params vcp;
    for (int v = 0; v < 3; ++v)
    {
        bool muted = voiceMuted_[v].load() || (anySolo && solo != v);
        voices_[v].setMuted (muted);

        fillVoiceParams (v, vcp);
        double inputPhase = selectInputPhase (v, masterPhase);
        voices_[v].processBlock (inputPhase, vcp, buffer, 0, numSamples);
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
    g.distDrive      = p.distDrive->get();
    g.reverbMix      = p.reverbMix->get();
    g.reverbSize     = p.reverbSize->get();
    g.reverbFreeze   = p.reverbFreeze->get();
    g.gain           = p.gain->get();
    g.regionStart    = p.regionStart->get();
    g.regionEnd      = p.regionEnd->get();
    g.loopMode       = static_cast<GranularVoice::LoopMode> (p.loopMode->get());
    g.loopStart      = p.loopStart->get();
    g.loopEnd        = p.loopEnd->get();
    g.loopSizeMs     = p.loopSizeMs->get();
    g.loopSizeLock   = p.loopSizeLock->get();
}

//==============================================================================
// Message-thread API
//==============================================================================
void W2SamplerProcessor::loadFolder (int v, const juce::File& folder)
{
    if (v < 0 || v > 2) return;
    voices_[v].loadFolder (folder, formatManager_);
    voices_[v].getLibrary().analyseAllOnsets (0.5f);
}

void W2SamplerProcessor::prevSample   (int v) { if (v>=0&&v<3) voices_[v].prevSample(); }
void W2SamplerProcessor::nextSample   (int v) { if (v>=0&&v<3) voices_[v].nextSample(); }
void W2SamplerProcessor::randomSample (int v) { if (v>=0&&v<3) voices_[v].randomSample(); }

void W2SamplerProcessor::randomizeVoiceParams (int v)
{
    if (v < 0 || v > 2) return;
    auto& p   = vp[v];
    auto& rng = juce::Random::getSystemRandom();
    *p.pitch       = rng.nextFloat() * 24.0f - 12.0f;
    *p.attack      = 0.001f + rng.nextFloat() * 0.499f;
    *p.decay       = 0.01f  + rng.nextFloat() * 0.490f;
    *p.sustain     = 0.3f   + rng.nextFloat() * 0.7f;
    *p.release     = 0.05f  + rng.nextFloat() * 0.950f;
    *p.filterFreq  = 200.0f + rng.nextFloat() * 15800.0f;
    *p.filterRes   = 0.5f   + rng.nextFloat() * 3.0f;
    *p.distDrive   = rng.nextFloat() * 0.6f;
    *p.reverbMix   = rng.nextFloat() * 0.7f;
    *p.reverbSize  = 0.2f   + rng.nextFloat() * 0.8f;
}

void W2SamplerProcessor::checkAndFireRandomizations()
{
    for (int v = 0; v < 3; ++v)
        if (voices_[v].takeRandomizeFXRequest())
            randomizeVoiceParams (v);
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
// State v3 (Phase 1 format)
//==============================================================================
void W2SamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream s (destData, true);
    s.writeByte (4);
    s.writeFloat (bpm->get());
    s.writeInt   (clkDiv->get());
    for (int v = 0; v < 3; ++v)
    {
        const auto& p = vp[v];
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
    }
}

void W2SamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream s (data, (size_t) sizeInBytes, false);
    if (s.getNumBytesRemaining() < 1) return;
    if (s.readByte() != 4) return;  // discard older formats
    if (s.getNumBytesRemaining() < 4) return;
    *bpm    = s.readFloat();
    *clkDiv = s.readInt();
    for (int v = 0; v < 3; ++v)
    {
        auto& p = vp[v];
        if (s.getNumBytesRemaining() < 4) return;
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
