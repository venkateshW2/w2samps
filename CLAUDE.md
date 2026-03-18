# CLAUDE.md — W2 Audio Plugs

Context file for Claude Code. Read this before making changes.

---

## What This Project Is

A JUCE 8 audio plugin + standalone app. Currently: **W2 Sampler** — a
euclidean-sequenced one-shot sample player with a full DSP chain.

Long-term goal: ML-powered instrument (ONNX inference, generative patterns).
UI will eventually move to HTML/JS (WebView). For now: JUCE native Components.

---

## Build

```bash
./build.sh              # Debug (default)
./build.sh Release      # Optimised
cmake --build build --parallel   # same thing manually
```

First build downloads JUCE 8.0.7 + clap-juce-extensions via CMake FetchContent.
Subsequent builds are fast (only changed .cpp files recompile).

Outputs in `build/W2Sampler_artefacts/Debug/`:
- `Standalone/W2 Sampler.app`   ← run directly
- `VST3/W2 Sampler.vst3`
- `AU/W2 Sampler.component`
- `CLAP/W2 Sampler.clap`

---

## Source Files — What Each Does

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Entire build: FetchContent deps, JUCE plugin definition, source list |
| `src/EuclideanSequencer.h` | Bjorklund algorithm — header-only, no JUCE |
| `src/SamplerVoice.h` | DSP chain: sample read + ADSR + distortion + filter + reverb |
| `src/SampleLibrary.h` | Loads a folder of one-shots into memory, manages navigation |
| `src/PluginProcessor.h/cpp` | AudioProcessor: all parameters, audio clock, coordinates DSP |
| `src/PluginEditor.h/cpp` | JUCE UI: 620×430 window, waveform, sliders, step grid |

---

## Parameters

All declared in `PluginProcessor` constructor as `AudioParameter*` members.
IDs written into DAW project files — **never rename the ID strings**.

| Member | ID | Range | Notes |
|--------|----|-------|-------|
| `seqSteps` | seqSteps | 1–32 | steps in pattern |
| `seqHits` | seqHits | 0–32 | active hits |
| `seqRotation` | seqRotation | 0–31 | pattern rotation |
| `seqRate` | seqRate | 0.25–4.0 | steps per beat |
| `sampleGain` | sampleGain | 0–2 | output gain |
| `pitch` | pitch | -24–+24 semitones | |
| `attack` | attack | 0.001–2.0 sec | log skew |
| `decay` | decay | 0.001–2.0 sec | log skew |
| `sustain` | sustain | 0–1 | |
| `release` | release | 0.001–4.0 sec | log skew |
| `filterFreq` | filterFreq | 20–20000 Hz | log skew 0.25 |
| `filterRes` | filterRes | 0.5–10.0 Q | StateVariableTPT |
| `distDrive` | distDrive | 0–1 | tanh pre-gain 1×–10× |
| `reverbMix` | reverbMix | 0–1 | wet level × 0.5 |
| `reverbSize` | reverbSize | 0–1 | room size |
| `reverbFreeze` | reverbFreeze | bool | juce::Reverb freeze mode |
| `sampleAdvanceMode` | sampleAdv | 0/1/2 | Hold/Sequential/Random |

---

## Threading Model

**Two threads. Never cross them without using the provided patterns.**

```
Audio thread  ──► processBlock()      HIGH PRIORITY
                  advanceClock()
                  voice.renderBlock()
                  ─── NO alloc, NO file I/O, NO mutex lock ───

Message thread ──► All UI callbacks
                   loadSample(), loadFolder()
                   randomizeVoiceParams()
                   All Editor/Timer code
```

**Safe cross-thread communication:**
- `AudioParameter*` — atomic internally; write from message thread, read from audio thread
- `SampleLibrary::currentIndex` — `std::atomic<int>`; audio thread reads/writes it
- `isPlaying_` — `std::atomic<bool>`
- `SamplerVoice::swapBuffer()` — called from audio thread (pointer swap only, no alloc)
- `SamplerVoice::loadBuffer()` — message thread only (full state reset)

**Known benign races (documented):**
- `loadFolder()` while playing: `voice.stop()` is called first to minimise window
- `reverb_.setParameters()` is called from audio thread only (all renderBlock calls)

---

## DSP Chain (inside W2SamplerVoice::renderBlock)

```
Raw sample data (linear interpolation, pitch ratio = 2^(semitones/12))
     ↓
juce::ADSR envelope    (noteOn on trigger, noteOff when buffer exhausted)
     ↓
tanh distortion        (pre-gain = 1 + drive × 9, normalised output)
     ↓
StateVariableTPTFilter (low-pass; only processes if freq < 19900 Hz)
     ↓
juce::Reverb           (processStereo; dryLevel=1, wetLevel=mix×0.5)
     ↓
Gain × velocity → addFrom into output buffer
```

Voice stays "playing" until ADSR says `isActive() == false` — enables reverb tails.

---

## Sample Advance Modes

On each sequencer **hit** (euclidean tick), the audio thread can optionally switch
which sample plays before triggering:

| Mode | Value | Behaviour |
|------|-------|-----------|
| Hold | 0 | Same sample every hit (default) |
| Sequential | 1 | Advances to next file in folder each hit |
| Random | 2 | Picks a random file from folder each hit |

Implementation: `SampleLibrary` pre-loads ALL samples into memory at folder-load
time. The audio thread only does pointer swaps — no file I/O ever on audio thread.
LCG RNG (`audioRng_`) used for audio-thread random selection (no shared state).

---

## Adding a New Parameter

1. Declare it in `PluginProcessor.h`: `juce::AudioParameterFloat* myParam = nullptr;`
2. Register in `PluginProcessor.cpp` constructor: `addParameter(myParam = new ...)`
3. Read it in `rebuildVoiceParamsIfNeeded()` (if it goes into `W2SamplerVoice::Params`)
   or directly in `processBlock` for sequencer-level things
4. Add a slider in `PluginEditor.h` + `PluginEditor.cpp` (setupSlider + resized + onChange)
5. Add to `getStateInformation` / `setStateInformation` (bump version byte if breaking)

---

## Adding a New Source File

1. Create `src/MyThing.h` and `src/MyThing.cpp`
2. Add `src/MyThing.cpp` to `target_sources(W2Sampler PRIVATE ...)` in `CMakeLists.txt`
3. Rebuild

---

## Known JUCE Quirks

- **VST3 build assertion** `juce_AudioProcessor.cpp:451` — non-fatal, from the
  VST3 manifest helper tool. Plugin works fine.
- **Standalone settings dialog** — enumerates all CoreAudio devices, can take
  3–5 seconds on Macs with many audio interfaces. Not frozen.
- **`juce::Reverb` thread safety** — `setParameters()` writes internal filter
  state directly. Always call from the audio thread (inside renderBlock).
  Never call from message thread while audio is running.
- **`AudioParameterBool::get()`** returns `bool`. Use `*param = value` to set.

---

## Roadmap

### Next: Phase 2
- [ ] Host BPM sync (`AudioPlayHead`)
- [ ] Multi-pad layout (8 pads, each with own sample + sequencer + FX)
- [ ] MIDI note → pad mapping
- [ ] Envelope display (draw ADSR curve in UI)

### Phase 3: ML
- [ ] ONNX Runtime via CMake FetchContent
- [ ] Drum pattern generator (model outputs hit pattern)
- [ ] Onset detection / spectral analysis

### Phase 4: UI
- [ ] WebView frontend (CHOC library)
- [ ] HTML/CSS/JS UI, C++ backend

### Phase 5: Release
- [ ] Code signing + notarisation (required for macOS distribution)
- [ ] Windows build (CMake + MSVC)
- [ ] Installer packaging
