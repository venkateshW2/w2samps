#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
W2SamplerProcessor::W2SamplerProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager_.registerBasicFormats();

    // ── Register all parameters ───────────────────────────────────────────────
    // IDs are written into DAW project files — never rename them!
    // Sequencer
    addParameter (seqSteps    = new juce::AudioParameterInt   ("seqSteps",    "Steps",    1,  32, 16));
    addParameter (seqHits     = new juce::AudioParameterInt   ("seqHits",     "Hits",     0,  32,  4));
    addParameter (seqRotation = new juce::AudioParameterInt   ("seqRotation", "Rotation", 0,  31,  0));
    addParameter (seqRate     = new juce::AudioParameterFloat ("seqRate",     "Rate",
                                    juce::NormalisableRange<float> (0.25f, 4.0f, 0.0f, 0.5f), 1.0f));
    addParameter (sampleGain  = new juce::AudioParameterFloat ("sampleGain",  "Gain",     0.0f, 2.0f, 1.0f));

    // Pitch
    addParameter (pitch = new juce::AudioParameterFloat ("pitch", "Pitch",
                              juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));

    // ADSR — use logarithmic skew so short times have finer resolution
    auto logRange = [] (float mn, float mx) {
        return juce::NormalisableRange<float> (mn, mx, 0.0f, 0.3f);
    };
    addParameter (attack  = new juce::AudioParameterFloat ("attack",  "Attack",  logRange (0.001f, 2.0f), 0.005f));
    addParameter (decay   = new juce::AudioParameterFloat ("decay",   "Decay",   logRange (0.001f, 2.0f), 0.1f));
    addParameter (sustain = new juce::AudioParameterFloat ("sustain", "Sustain", { 0.0f, 1.0f },          0.8f));
    addParameter (release = new juce::AudioParameterFloat ("release", "Release", logRange (0.001f, 4.0f), 0.2f));

    // Filter — frequency with strong log skew so low end isn't crammed in 5%
    addParameter (filterFreq = new juce::AudioParameterFloat ("filterFreq", "Filter Freq",
                                   juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.25f), 20000.0f));
    addParameter (filterRes  = new juce::AudioParameterFloat ("filterRes",  "Filter Res",
                                   { 0.5f, 10.0f }, 0.707f));

    // Distortion + Reverb
    addParameter (distDrive    = new juce::AudioParameterFloat ("distDrive",    "Drive",      { 0.0f, 1.0f }, 0.0f));
    addParameter (reverbMix    = new juce::AudioParameterFloat ("reverbMix",    "Reverb Mix", { 0.0f, 1.0f }, 0.0f));
    addParameter (reverbSize   = new juce::AudioParameterFloat ("reverbSize",   "Room Size",  { 0.0f, 1.0f }, 0.5f));
    addParameter (reverbFreeze = new juce::AudioParameterBool  ("reverbFreeze", "Freeze",     false));

    // Sample advance mode
    addParameter (sampleAdvanceMode = new juce::AudioParameterInt ("sampleAdv", "Sample Mode", 0, 2, 0));

    // Set initial sequencer pattern
    sequencer.set (16, 4, 0);
}

W2SamplerProcessor::~W2SamplerProcessor() {}

//==============================================================================
void W2SamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    stepPhase_  = 0.0;
    sequencer.reset();

    // Invalidate all caches so first processBlock does a full rebuild
    lastSteps_ = lastHits_ = lastRotation_ = -1;
    lastRate_ = lastPitch_ = lastAttack_ = lastDecay_ = -1.0f;
    lastSustain_ = lastRelease_ = lastFilterFreq_ = lastFilterRes_ = -1.0f;
    lastDistDrive_ = lastReverbMix_ = lastReverbSize_ = -1.0f;
    lastReverbFreeze_ = false;

    // Prepare the DSP voice (filter, reverb, ADSR) for this sample rate/block size
    voice.prepare (sampleRate, samplesPerBlock);

    rebuildSequencerIfNeeded();
}

void W2SamplerProcessor::releaseResources() {}

//==============================================================================
// processBlock — THE HOT PATH. Audio thread. ~100x/sec. No alloc, no I/O.
//==============================================================================
void W2SamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Rebuild sequencer pattern if steps/hits/rotation/rate changed
    rebuildSequencerIfNeeded();

    // Build the voice params struct from current parameter values
    W2SamplerVoice::Params vp;
    rebuildVoiceParamsIfNeeded (vp);

    int numSamples = buffer.getNumSamples();

    // Advance the internal clock — fires sequencer ticks and voice triggers
    if (isPlaying_.load())
        advanceClock (numSamples);

    // Render the sample voice (with all DSP) into the output buffer
    if (voice.isPlaying())
        voice.renderBlock (buffer, 0, numSamples, sampleRate_, vp);

    // Handle MIDI note-on (manual keyboard trigger, bypasses sequencer)
    for (const auto meta : midi)
    {
        auto msg = meta.getMessage();
        if (msg.isNoteOn())
            voice.trigger (msg.getFloatVelocity());
    }
    midi.clear();
}

//==============================================================================
// advanceClock — sample-accurate clock; fires sequencer ticks at step boundaries
//==============================================================================
void W2SamplerProcessor::advanceClock (int numSamples)
{
    if (samplesPerStep_ <= 0.0) return;

    int advMode = sampleAdvanceMode->get();

    for (int i = 0; i < numSamples; ++i)
    {
        stepPhase_ += 1.0;

        if (stepPhase_ >= samplesPerStep_)
        {
            stepPhase_ -= samplesPerStep_;  // wrap (preserves fractional overshoot)

            bool hit = sequencer.tick();
            if (hit)
            {
                // ── Sample advance on hit ─────────────────────────────────────
                // This is the "sample changer" feature: each hit can pull a new
                // sample from the library. No file I/O — just pointer swap.
                if (advMode == 1 && library_.getCount() > 1)
                {
                    // Sequential: advance to next file in folder
                    int idx = library_.advanceNext();
                    auto* e = library_.getEntry (idx);
                    if (e != nullptr)
                        voice.swapBuffer (&e->buffer, e->sampleRate);
                }
                else if (advMode == 2 && library_.getCount() > 1)
                {
                    // Random: pick any file from the folder
                    int idx = library_.advanceRandom (audioRng_);
                    auto* e = library_.getEntry (idx);
                    if (e != nullptr)
                        voice.swapBuffer (&e->buffer, e->sampleRate);
                }

                voice.trigger (1.0f);
            }
        }
    }
}

//==============================================================================
// rebuildSequencerIfNeeded — only calls sequencer.set() when params changed
//==============================================================================
void W2SamplerProcessor::rebuildSequencerIfNeeded()
{
    int   steps    = seqSteps->get();
    int   hits     = seqHits->get();
    int   rotation = seqRotation->get();
    float rate     = seqRate->get();

    if (steps != lastSteps_ || hits != lastHits_ || rotation != lastRotation_)
    {
        sequencer.set (steps, hits, rotation);
        lastSteps_ = steps; lastHits_ = hits; lastRotation_ = rotation;
    }

    if (std::abs (rate - lastRate_) > 1e-6f)
    {
        // Convert BPM + rate to samples/step
        // TODO: replace hardcoded 120 with host BPM from AudioPlayHead
        double bpm         = 120.0;
        samplesPerStep_    = sampleRate_ * (60.0 / bpm) / (double) rate;
        lastRate_          = rate;
    }
}

//==============================================================================
// rebuildVoiceParamsIfNeeded — fills the Params struct; updates caches if changed
//==============================================================================
void W2SamplerProcessor::rebuildVoiceParamsIfNeeded (W2SamplerVoice::Params& p)
{
    // Always fill the struct from current param values
    p.pitchSemitones = pitch->get();
    p.attackSec      = attack->get();
    p.decaySec       = decay->get();
    p.sustain        = sustain->get();
    p.releaseSec     = release->get();
    p.filterFreqHz   = filterFreq->get();
    p.filterRes      = filterRes->get();
    p.distDrive      = distDrive->get();
    p.reverbMix      = reverbMix->get();
    p.reverbSize     = reverbSize->get();
    p.reverbFreeze   = reverbFreeze->get();
    p.gain           = sampleGain->get();
    // (Voice reads these from the struct in renderBlock — no separate cache needed here)
}

//==============================================================================
// File / library management (MESSAGE THREAD)
//==============================================================================
void W2SamplerProcessor::loadFolder (const juce::File& folder)
{
    // Pre-load all samples from the folder into memory.
    // This decodes audio on the message thread — can take a moment for large folders.
    int n = library_.loadFolder (folder, formatManager_);
    juce::ignoreUnused (n);

    // Load the first sample into the voice
    loadCurrentSample();
}

void W2SamplerProcessor::prevSample()
{
    library_.prev();
    loadCurrentSample();
}

void W2SamplerProcessor::nextSample()
{
    library_.next();
    loadCurrentSample();
}

void W2SamplerProcessor::randomSample()
{
    library_.pickRandom();
    loadCurrentSample();
}

void W2SamplerProcessor::loadCurrentSample()
{
    auto* e = library_.current();
    if (e == nullptr) return;

    // loadBuffer is a message-thread operation — stop the voice first to avoid
    // the audio thread reading a half-swapped pointer (see ARCHITECTURE.md §13)
    voice.stop();
    voice.loadBuffer (&e->buffer, e->sampleRate);
}

bool W2SamplerProcessor::hasSample() const
{
    return library_.current() != nullptr;
}

const juce::AudioBuffer<float>* W2SamplerProcessor::getCurrentSampleBuffer() const
{
    auto* e = library_.current();
    return e ? &e->buffer : nullptr;
}

//==============================================================================
// Voice params randomize (MESSAGE THREAD — AudioParameter::operator= is atomic)
//==============================================================================
void W2SamplerProcessor::randomizeVoiceParams()
{
    auto& rng = juce::Random::getSystemRandom();
    *pitch       = rng.nextFloat() * 24.0f - 12.0f;         // -12 to +12 semitones
    *attack      = 0.001f + rng.nextFloat() * 0.499f;        // 1ms – 500ms
    *decay       = 0.01f  + rng.nextFloat() * 0.490f;        // 10ms – 500ms
    *sustain     = 0.3f   + rng.nextFloat() * 0.7f;          // 30% – 100%
    *release     = 0.05f  + rng.nextFloat() * 0.950f;        // 50ms – 1s
    *filterFreq  = 200.0f + rng.nextFloat() * 15800.0f;      // 200Hz – 16kHz
    *filterRes   = 0.5f   + rng.nextFloat() * 3.0f;          // 0.5 – 3.5
    *distDrive   = rng.nextFloat() * 0.6f;                   // 0% – 60%
    *reverbMix   = rng.nextFloat() * 0.7f;                   // 0% – 70%
    *reverbSize  = 0.2f   + rng.nextFloat() * 0.8f;          // 20% – 100%
    // Leave: sampleGain, reverbFreeze, all seq params
}

//==============================================================================
// Transport
//==============================================================================
void W2SamplerProcessor::setPlaying (bool shouldPlay)
{
    if (!shouldPlay)
    {
        stepPhase_ = 0.0;
        sequencer.reset();
        voice.stop();
    }
    isPlaying_.store (shouldPlay);
}

bool W2SamplerProcessor::getPlaying() const { return isPlaying_.load(); }

//==============================================================================
// UI accessors
//==============================================================================
std::vector<bool> W2SamplerProcessor::getCurrentPattern() const
{
    return sequencer.getPattern();
}

int W2SamplerProcessor::getCurrentStep() const { return sequencer.getStep(); }

//==============================================================================
// Editor factory
//==============================================================================
juce::AudioProcessorEditor* W2SamplerProcessor::createEditor()
{
    return new W2SamplerEditor (*this);
}

//==============================================================================
// State save / load — version 2 format
// Byte 0 = version marker (2)
// Followed by: 5 original params + 11 new params + 1 advance mode = 17 values
//==============================================================================
void W2SamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream s (destData, true);
    s.writeByte (2);  // version marker
    // Sequencer (5)
    s.writeInt   (seqSteps->get());
    s.writeInt   (seqHits->get());
    s.writeInt   (seqRotation->get());
    s.writeFloat (seqRate->get());
    s.writeFloat (sampleGain->get());
    // Voice (11)
    s.writeFloat (pitch->get());
    s.writeFloat (attack->get());
    s.writeFloat (decay->get());
    s.writeFloat (sustain->get());
    s.writeFloat (release->get());
    s.writeFloat (filterFreq->get());
    s.writeFloat (filterRes->get());
    s.writeFloat (distDrive->get());
    s.writeFloat (reverbMix->get());
    s.writeFloat (reverbSize->get());
    s.writeBool  (reverbFreeze->get());
    // Mode (1)
    s.writeInt   (sampleAdvanceMode->get());
}

void W2SamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream s (data, (size_t) sizeInBytes, false);
    if (s.getNumBytesRemaining() < 1) return;

    int8_t version = s.readByte();

    if (version == 2)
    {
        // Full v2 state
        if (s.getNumBytesRemaining() < 20) return;
        *seqSteps    = s.readInt();
        *seqHits     = s.readInt();
        *seqRotation = s.readInt();
        *seqRate     = s.readFloat();
        *sampleGain  = s.readFloat();

        if (s.getNumBytesRemaining() < 44) return;
        *pitch       = s.readFloat();
        *attack      = s.readFloat();
        *decay       = s.readFloat();
        *sustain     = s.readFloat();
        *release     = s.readFloat();
        *filterFreq  = s.readFloat();
        *filterRes   = s.readFloat();
        *distDrive   = s.readFloat();
        *reverbMix   = s.readFloat();
        *reverbSize  = s.readFloat();
        *reverbFreeze = s.readBool();
        *sampleAdvanceMode = s.readInt();
    }
    else
    {
        // v1 compat: the 'version' byte was actually the first byte of seqSteps int
        // Reconstruct the int from that byte + 3 more bytes
        int steps = (int) ((uint8_t) version);
        if (s.getNumBytesRemaining() < 3) return;
        // Read the remaining 3 bytes of the int (little-endian)
        steps |= ((int) s.readByte()) << 8;
        steps |= ((int) s.readByte()) << 16;
        steps |= ((int) s.readByte()) << 24;
        *seqSteps = steps;
        if (s.getNumBytesRemaining() < 16) return;
        *seqHits     = s.readInt();
        *seqRotation = s.readInt();
        *seqRate     = s.readFloat();
        *sampleGain  = s.readFloat();
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new W2SamplerProcessor();
}
