# CLAUDE.md — W2 Audio Plugs

Context file for Claude Code. Read this before making changes.
Last updated: 2026-03-19

---

## What This Project Is

A JUCE 8 audio plugin + standalone app.

**Current state (v0.2):** Single-voice euclidean sampler with DSP chain.
**Target (v1.0):** Three-voice phasor-based polyrhythmic granular sampler with
onset detection, phase manipulation, and eventually ONNX-based audio analysis.

Long-term goal: ML-powered instrument. UI will eventually move to HTML/JS
(WebView / CHOC). For now: JUCE native Components, tabbed layout.

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

- `Standalone/W2 Sampler.app` ← run directly
- `VST3/W2 Sampler.vst3`
- `AU/W2 Sampler.component`
- `CLAP/W2 Sampler.clap`

---

## Source Files — Current State

| File                        | Purpose                                                   |
| --------------------------- | --------------------------------------------------------- |
| `CMakeLists.txt`            | Entire build: FetchContent deps, JUCE plugin, source list |
| `src/EuclideanSequencer.h`  | Bjorklund algorithm — header-only, no JUCE                |
| `src/SamplerVoice.h`        | DSP chain: sample + ADSR + distortion + filter + reverb   |
| `src/SampleLibrary.h`       | Pre-loads a folder into memory, manages navigation        |
| `src/PluginProcessor.h/cpp` | AudioProcessor: params, clock, coordinates DSP            |
| `src/PluginEditor.h/cpp`    | JUCE UI: 620×430, waveform, sliders, step grid            |

---

## Phase 1 Refactor — What We Are Building Next

This is the active development phase. Do not write new features outside this
scope until Phase 1 is complete and committed.

### 1.1 — MasterClock (phasor-based)

Replace the current sample-counter clock with a **phasor**: a 0→1 ramp that
increments at audio rate, derived from a user-settable BPM.

```
masterPhase: 0.0 ──────────────────────────► 1.0 → wraps
increment per sample = BPM / (60 × sampleRate)
One full cycle = one beat.
```

Each voice derives its step timing by reading a **transformed version** of the
master phase. This is the key architectural idea — see §1.2.

**BPM control:**

- `AudioParameterFloat* bpm` — range 20–300, default 120
- Shown as a large knob/display on the Master tab
- Later: sync to host `AudioPlayHead` (read host BPM instead of own param)

**New file:** `src/MasterClock.h`

```cpp
class MasterClock {
public:
    double phase = 0.0;      // current phasor value, 0→1 per beat
    double bpm   = 120.0;    // beats per minute

    // Call once per sample. Returns current phase (0→1).
    double tick(double sampleRate) {
        phase += bpm / (60.0 * sampleRate);
        if (phase >= 1.0) phase -= 1.0;
        return phase;
    }

    void reset() { phase = 0.0; }
    void setBPM(double b) { bpm = b; }
};
```

### 1.2 — Phase Manipulation Per Voice

Each voice does NOT read the raw master phase. It reads a **transformed phase**.
This is where all the musical expressiveness comes from.

**Transformations (applied in this order):**

```
masterPhase (0→1)
     │
     ▼
① Rate multiply     phase × rateMultiplier
     │               e.g., 2.0 = runs twice as fast, 0.5 = half speed
     ▼
② Phase offset      fmod(phase + offset, 1.0)
     │               shifts the pattern start relative to master
     ▼
③ Warp / curve      apply a shaping function
     │               linear = normal, curve>0 = rushes early, curve<0 = drags early
     ▼
④ Reverse           if reversed: use (1.0 - phase) instead
     │               pattern plays backwards in time
     ▼
⑤ Step quantise     floor(phase × stepsPerLoop) / stepsPerLoop
                     turns the smooth ramp into a staircase = discrete steps
                     (this IS the sequencer clock — each stair = one step)
```

**Warp function (③):**
A single `warp` parameter (-1 → +1):

- `warp = 0`: linear (normal timing)
- `warp > 0`: exponential — steps early in the pattern happen faster, steps
  at the end are compressed. Feels like the pattern is "rushing."
- `warp < 0`: logarithmic — steps early are stretched, later steps rush.
  Feels like the pattern "drags then catches up."

Implementation: `warpedPhase = pow(phase, exp(warp * 2.0))`

- warp=0 → exp(0)=1 → pow(x,1) = x (linear)
- warp=1 → exp(2)≈7.4 → pow(x,7.4) heavy rush at start
- warp=-1 → exp(-2)≈0.14 → pow(x,0.14) drag at start

**Swing** is a special case of warp applied to every other step pair.
Can be added as a separate param later.

**Inter-voice phase relationships:**
Each voice has a `phaseSource` option:

- `Master` — reads from the global master phasor (independent)
- `Lock to V1` — uses Voice 1's transformed phase as input instead of master
- `Ratio N:M to V1` — derives from Voice 1's phase at a rational ratio
  (e.g., 3:2 means this voice runs 1.5× faster than Voice 1)

This means: Voice 2 and 3 can be completely free-running from master, OR they
can be phase-locked to Voice 1, OR they can run at musically-related ratios.
This enables polyrhythm (3:4, 5:4, 7:8) and nested loops (run V2 at 4× V1's
rate = V2 fires 4 times per one V1 cycle).

**Parameters per voice for phase manipulation:**

| Param              | Range     | Notes                                            |
| ------------------ | --------- | ------------------------------------------------ |
| `rateMultiplier`   | 0.125–8.0 | expressed as fraction: 1/8, 1/4, 1/2, 1, 2, 4, 8 |
| `phaseOffset`      | 0.0–1.0   | fraction of a beat                               |
| `warp`             | -1.0–+1.0 | 0 = linear                                       |
| `reverse`          | bool      | play pattern backwards                           |
| `phaseSource`      | enum      | Master / LockV1 / LockV2 / RatioV1 / RatioV2     |
| `ratioNumerator`   | 1–16      | for Ratio mode                                   |
| `ratioDenominator` | 1–16      | for Ratio mode                                   |

### 1.3 — Polyrhythm via Loop Length Fraction

Each voice's step sequencer has:

| Param               | Range            | Notes                                         |
| ------------------- | ---------------- | --------------------------------------------- |
| `loopNumerator`     | 1–16             | top of time signature                         |
| `loopDenominator`   | 1–16             | bottom (1=whole, 2=half, 4=quarter, 8=eighth) |
| `stepsPerLoop`      | 1–32             | how many steps in one loop cycle              |
| `euclideanHits`     | 0–stepsPerLoop   | active hits                                   |
| `euclideanRotation` | 0–stepsPerLoop-1 | rotate pattern                                |

**Loop length in beats** = `loopNumerator / loopDenominator`

**Step fire logic (per sample, audio thread):**

```
voicePhase = transformedPhase × stepsPerLoop / loopLengthBeats
currentStep = floor(voicePhase) % stepsPerLoop
if currentStep != previousStep:
    fire pattern[currentStep]
```

**Examples:**

| loopNum | loopDen | stepsPerLoop | Feel                        |
| ------- | ------- | ------------ | --------------------------- |
| 4       | 4       | 16           | standard 16th notes in 4/4  |
| 4       | 4       | 7            | 7-over-4 polyrhythm         |
| 3       | 4       | 7            | 7 steps in 3 beats = 7/8ish |
| 5       | 4       | 5            | 5/4 time                    |
| 3       | 4       | 3            | triplets                    |
| 7       | 8       | 7            | 7/8 time signature          |
| 2       | 1       | 32           | 32nd notes over 2 bars      |

### 1.4 — Three VoiceChannels

Replace the single processor-level voice with three independent `VoiceChannel`
objects. Each has its own library, its own granular voice, its own sequencer.

**New file:** `src/VoiceChannel.h`

```cpp
struct VoiceChannel {
    SampleLibrary   library;        // own folder of samples
    GranularVoice   voice;          // DSP: playback + loop + FX (replaces SamplerVoice)
    // Phase manipulation params (read from AudioParameter* at processBlock time)
    // Sequencer params (loopNum, loopDen, steps, hits, rotation)
    // Previous step tracker (for step-fire edge detection)
    int prevStep = -1;
    double transformedPhase = 0.0;  // computed each block from master + per-voice transforms
};
VoiceChannel voices[3];
```

All three call `voice.renderBlock()` into the same output buffer (summed).
Each has its own gain/pan.

**Parameter naming convention for 3 voices:**
Prefix all params with `v0_`, `v1_`, `v2_`. Example: `v0_seqSteps`, `v1_pitch`.
This keeps DAW automation lanes organised.

### 1.5 — GranularVoice (replaces SamplerVoice)

Upgrade the current one-shot voice to support region-based looping.
The one-shot behaviour is a special case (no loop = one-shot).

**Region model:**

```
|←────────────── sample buffer ──────────────────→|
        |←── play region ──────────────→|
                  |←── loop ──→|
        ▲         ▲            ▲         ▲
    regionStart loopStart  loopEnd   regionEnd
    (0.0–1.0)   (0.0–1.0)  (0.0–1.0) (0.0–1.0)
    all values normalised to buffer length
```

**Loop modes:**

| Mode       | Value | Behaviour                                                                |
| ---------- | ----- | ------------------------------------------------------------------------ |
| Off        | 0     | plays regionStart → regionEnd, stops (one-shot)                          |
| Fixed      | 1     | loops between loopStart → loopEnd continuously                           |
| Random     | 2     | on each loop-back, picks random loopStart within region (granular)       |
| Sequential | 3     | on each loop-back, advances loopStart by loopSize (scans through sample) |

**Loop size lock:** when enabled, `loopSize` is fixed (in ms or as beat fraction).
`loopStart` becomes a single position knob that slides the fixed window.
Combined with Random/Sequential mode this is a simple granular engine.

**Loop size parameter:** free float in milliseconds (1ms – 10000ms).
User can also "lock to beat" which snaps to current BPM subdivisions.

**New file:** `src/GranularVoice.h` (replaces `src/SamplerVoice.h`)
The DSP chain stays the same (ADSR + distortion + filter + reverb).
Only the sample-read section changes.

### 1.6 — Onset Detection

Run offline (message thread) when a file is loaded into a VoiceChannel.

**Algorithm: spectral flux**

1. STFT with 512-sample windows, 256-sample hop
2. For each window: sum of positive differences vs previous magnitude spectrum
3. Peak-pick above adaptive threshold
4. Store onset positions as `std::vector<float>` (normalised 0–1) in `SampleLibrary::Entry`

**Also compute:** estimated file BPM from onset interval autocorrelation.
Store as `float estimatedBPM` in Entry. Display in UI as hint.

**UI integration:**

- Onset positions shown as tick marks on the waveform display
- Region/loop handles snap to nearest onset when dragged
- "Set master BPM to file BPM" button

**New file:** `src/OnsetDetector.h`

### 1.7 — Tabbed UI Shell

Replace the current single-panel UI with a tab strip:

```
[Master] [Voice 1] [Voice 2] [Voice 3]
```

- **Master tab**: BPM knob, play/stop, global level, phase visualiser
  (shows all 3 voice phases as animated dots on a circle — easy to see
  polyrhythm relationships)
- **Voice tabs**: waveform + region handles + onset markers, loop controls,
  ADSR + FX sliders, sequencer (steps/hits/rotation/loopNum/loopDen),
  phase manipulation (rate, offset, warp, reverse, source)

Window size: **700 × 500 px**

**Implementation:** custom tab strip (drawn in paint(), buttons set active tab index,
`resized()` shows/hides component groups based on active tab).
Do NOT use `juce::TabbedComponent` — too hard to style.

---

## Design Decisions (Locked)

These were discussed and decided. Do not revisit without user approval.

| Decision              | Choice                                                                           | Reason                                                          |
| --------------------- | -------------------------------------------------------------------------------- | --------------------------------------------------------------- |
| UI layout             | Tabbed (custom tab strip)                                                        | Each voice needs enough room                                    |
| Loop size unit        | Milliseconds (free float)                                                        | Musician-friendly                                               |
| Time signature        | loopNumerator / loopDenominator (two int params)                                 | Maps to musical notation                                        |
| Onset threshold       | Manual + automatic (both available)                                              | Flexibility                                                     |
| Output                | Single stereo mix, per-voice level control                                       | Simplicity first                                                |
| Phase source          | Per voice: Master / Lock / Ratio                                                 | Enables polyrhythm + nesting                                    |
| Warp animation        | Supported — warp is an AudioParameter, animated via DAW automation or future LFO | Phase jumps on hard cuts are acceptable / musically interesting |
| Ratio UI              | Curated button strip (1:1 2:1 3:2 4:3 5:4 7:4 7:8 etc.) + free N:D integer entry | Curated sets N/D boxes; free entry overrides                   |
| Loop coord system     | loopStart/loopEnd are **absolute [0,1] fractions of the full buffer** (NOT relative to region). GranularVoice clamps them to region at render time. seqLoopAnchorNorm_ also in absolute buffer space. | Waveform handle positions directly match playback positions — no coordinate conversion needed |
| Loop handles in Off   | Loop handles always visible regardless of loop mode (Off/Fixed/Rnd/Seq). In Off mode the blue window shows the one-shot playback zone. | User needs to set the playback zone before switching mode |
| Active tab indicator  | Active tab: kActive (light green) background + 3px bottom border. Inactive tabs: kPanel. | Monochrome theme needs a clear indicator without colour overload |

---

## Current Parameters (v0.2 — to be superseded by Phase 1)

These will be replaced with per-voice prefixed params (`v0_`, `v1_`, `v2_`).
Keep them until Phase 1 is complete to avoid breaking existing sessions.

| Member              | ID           | Range             | Notes                                                |
| ------------------- | ------------ | ----------------- | ---------------------------------------------------- |
| `seqSteps`          | seqSteps     | 1–32              | steps in pattern                                     |
| `seqHits`           | seqHits      | 0–32              | active hits                                          |
| `seqRotation`       | seqRotation  | 0–31              | pattern rotation                                     |
| `seqRate`           | seqRate      | 0.25–4.0          | steps per beat (DEPRECATED: replaced by loopNum/Den) |
| `sampleGain`        | sampleGain   | 0–2               | output gain                                          |
| `pitch`             | pitch        | -24–+24 semitones |                                                      |
| `attack`            | attack       | 0.001–2.0 sec     | log skew                                             |
| `decay`             | decay        | 0.001–2.0 sec     | log skew                                             |
| `sustain`           | sustain      | 0–1               |                                                      |
| `release`           | release      | 0.001–4.0 sec     | log skew                                             |
| `filterFreq`        | filterFreq   | 20–20000 Hz       | log skew 0.25                                        |
| `filterRes`         | filterRes    | 0.5–10.0 Q        | StateVariableTPT                                     |
| `distDrive`         | distDrive    | 0–1               | tanh pre-gain 1×–10×                                 |
| `reverbMix`         | reverbMix    | 0–1               | wet level × 0.5                                      |
| `reverbSize`        | reverbSize   | 0–1               | room size                                            |
| `reverbFreeze`      | reverbFreeze | bool              | juce::Reverb freeze mode                             |
| `sampleAdvanceMode` | sampleAdv    | 0/1/2             | Hold/Sequential/Random                               |

---

## Threading Model

**Two threads. Never cross them without using the provided patterns.**

```
Audio thread  ──► processBlock()          HIGH PRIORITY
                  MasterClock::tick()
                  applyPhaseTransforms()  (per voice, per sample)
                  VoiceChannel::fire()
                  GranularVoice::renderBlock()
                  ─── NO alloc, NO file I/O, NO mutex lock ───

Message thread ──► All UI callbacks
                   loadFolder(), prevSample(), nextSample()
                   OnsetDetector::analyse()   (offline, on load)
                   randomizeVoiceParams()
                   All Editor/Timer code
```

**Safe cross-thread communication:**

- `AudioParameter*` — atomic internally; message thread writes, audio thread reads
- `SampleLibrary::currentIndex` — `std::atomic<int>`
- `isPlaying_` — `std::atomic<bool>`
- `GranularVoice::swapBuffer()` — audio thread only (pointer swap, no alloc)
- `GranularVoice::loadBuffer()` — message thread only (full state reset)
- `OnsetDetector` results written to `SampleLibrary::Entry` before audio uses
  the entry (safe because onset detection runs before the sample is activated)

**Known benign races (documented, acceptable for this codebase):**

- `loadFolder()` while playing: `voice.stop()` called first to minimise window
- `reverb_.setParameters()` — audio thread only (inside renderBlock)

---

## DSP Chain (inside GranularVoice::renderBlock)

```
Sample read (linear interp, pitch ratio, region clamp, loop logic)
     ↓
juce::ADSR envelope
     ↓
tanh distortion        (pre-gain = 1 + drive × 9, normalised)
     ↓
StateVariableTPTFilter (low-pass; skipped if freq ≥ 19900 Hz)
     ↓
juce::Reverb           (processStereo; dryLevel=1, wetLevel=mix×0.5)
     ↓
Gain × velocity → addFrom into output buffer
```

---

## Sample Advance Modes (per voice)

On each hit, optionally switch which sample plays before triggering.
No file I/O — SampleLibrary pre-loads all buffers into memory.

| Mode       | Value | Behaviour                        |
| ---------- | ----- | -------------------------------- |
| Hold       | 0     | same sample every hit            |
| Sequential | 1     | next file in folder each hit     |
| Random     | 2     | random file from folder each hit |

LCG RNG (`audioRng_`) used on audio thread (no shared state).

---

## Adding a New Parameter (Phase 1 convention)

1. Declare in `PluginProcessor.h` with voice prefix: `juce::AudioParameterFloat* v0_pitch`
2. Register in constructor: `addParameter(v0_pitch = new ..., "v0_pitch", ...)`
3. Read in `applyPhaseTransforms()` or `buildVoiceParams()` inside processBlock
4. Add to state I/O (bump version byte)
5. Add to the voice's tab in the editor

---

## Adding a New Source File

1. Create `src/MyThing.h` and `src/MyThing.cpp`
2. Add `src/MyThing.cpp` to `target_sources(W2Sampler PRIVATE ...)` in `CMakeLists.txt`
3. `cmake --build build --parallel`

---

## Known JUCE Quirks

- **VST3 build assertion** `juce_AudioProcessor.cpp:451` — non-fatal, from the
  VST3 manifest helper. Plugin works correctly.
- **Standalone settings dialog** — enumerates CoreAudio devices, can take
  3–5 seconds on Macs with many interfaces. Not frozen.
- **`juce::Reverb::setParameters()`** — not audio-thread-safe if called from
  message thread simultaneously. Always call from within renderBlock (audio thread).
- **`AudioParameterBool::get()`** returns `bool`. Set with `*param = value`.
- **`juce::dsp::StateVariableTPTFilter`** — Q must be ≥ 0.5 or it goes unstable.

---

## Roadmap

### Phase 1 — Phasor Clock + 3 Voices + Granular (CURRENT)

#### 1.1 MasterClock

- [ ] `src/MasterClock.h` — phasor tick, BPM param, host sync stub
- [ ] Wire into PluginProcessor, replace old sample-counter clock
- [ ] BPM knob in Master tab

#### 1.2 Phase Manipulation

- [ ] `src/PhaseTransform.h` — rate, offset, warp, reverse, step-quantise functions
- [ ] Per-voice params: rateMultiplier, phaseOffset, warp, reverse
- [ ] Inter-voice: phaseSource (Master/LockV1/LockV2/RatioV1/RatioV2) + ratioN/D
- [ ] Phase visualiser on Master tab (3 dots on circle showing current positions)

#### 1.3 Polyrhythm Sequencer

- [ ] Replace seqRate with loopNumerator + loopDenominator per voice
- [ ] Step-fire logic: `floor(transformedPhase × steps / loopBeats) % steps`
- [ ] Euclidean pattern per voice (steps, hits, rotation)

#### 1.4 Three VoiceChannels

- [ ] `src/VoiceChannel.h` — owns library + voice + sequencer state
- [ ] PluginProcessor holds `VoiceChannel voices[3]`
- [ ] Per-voice params with `v0_`, `v1_`, `v2_` prefix
- [ ] Per-voice gain, pan, level to output mix

#### 1.5 GranularVoice

- [ ] `src/GranularVoice.h` — replaces SamplerVoice
- [ ] Region params: regionStart, regionEnd, loopStart, loopEnd (all 0–1)
- [ ] Loop modes: Off / Fixed / Random / Sequential
- [ ] Loop size lock: fixed ms, slides position knob
- [ ] Same DSP chain (ADSR + distortion + filter + reverb)

#### 1.6 Onset Detection

- [ ] `src/OnsetDetector.h` — spectral flux onset detection
- [ ] Run on load in SampleLibrary::loadFolder
- [ ] Store onset positions + estimated BPM in SampleLibrary::Entry
- [ ] Display onset ticks on waveform in editor
- [ ] Snap region handles to onsets

#### 1.7 Tabbed UI

- [ ] Custom tab strip: [Master] [Voice 1] [Voice 2] [Voice 3]
- [ ] Master tab: BPM, play/stop, global level, phase circle visualiser
- [ ] Voice tabs: waveform + region, loop controls, ADSR+FX, seq params, phase params
- [ ] Window: 700 × 500 px

### Phase 2 — Preset Snapshot System

- [ ] 16 preset slots per voice (sample index + all voice params snapshot)
- [ ] Auto-fill from random (ring buffer of last 16 random states)
- [ ] Preset sequence lane (which preset fires on which step)
- [ ] Save/load presets to file
- [ ] Randomize as sequencer event layer with chance/probability

### Phase 3 — ONNX + ML Analysis

- [ ] ONNX Runtime via CMake FetchContent
- [ ] Instrument classification model (kick/snare/hat/melodic)
- [ ] Better BPM detection (replace autocorrelation with ML model)
- [ ] Source separation (advanced)

### Phase 4 — UI Upgrade

- [ ] WebView frontend (CHOC library)
- [ ] HTML/CSS/JS UI with C++ DSP backend

### Phase 5 — Release

- [ ] Code signing + notarisation (macOS distribution requirement)
- [ ] Windows build (CMake + MSVC or Clang-cl)
- [ ] Installer packaging
