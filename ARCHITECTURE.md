# W2 Audio Plugs — Architecture & Developer Guide

This document explains every part of the project: what each file does, how the
audio signal flows, how the math works, and where everything is heading.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Folder Structure](#2-folder-structure)
3. [How the Build Works](#3-how-the-build-works)
4. [What is JUCE?](#4-what-is-juce)
5. [Plugin Formats (VST3 / CLAP / AU / Standalone)](#5-plugin-formats)
6. [Signal Flow — from audio card to your ears](#6-signal-flow)
7. [File-by-File Breakdown](#7-file-by-file-breakdown)
8. [How the Euclidean Sequencer Works](#8-how-the-euclidean-sequencer-works)
9. [How the Sampler Voice Works](#9-how-the-sampler-voice-works)
10. [The Internal Clock](#10-the-internal-clock)
11. [The UI System](#11-the-ui-system)
12. [Parameters and State Saving](#12-parameters-and-state-saving)
13. [Known Gotchas](#13-known-gotchas)
14. [Roadmap](#14-roadmap)

---

## 1. Project Overview

**W2 Sampler** is an audio plugin (and standalone app) that combines two things:

- A **one-shot sample player** — load any WAV/AIFF/FLAC file and it fires on each hit
- A **Euclidean sequencer** — automatically distributes a chosen number of hits as
  evenly as possible across a number of steps, creating rhythmic patterns

The goal is an AI-driven, ML-powered instrument. This is the foundation.

---

## 2. Folder Structure

```
w2-audio-plugs/
│
├── CMakeLists.txt          ← The entire build is defined here. All dependencies
│                             (JUCE, CLAP extensions) are downloaded automatically
│                             the first time you build. Nothing is installed globally.
│
├── build.sh                ← Convenience script: runs cmake configure + build.
│                             Usage: ./build.sh          (Debug)
│                                    ./build.sh Release  (optimised, no debug symbols)
│
├── .vscode/
│   ├── settings.json       ← Tells VSCodium to use CMake + Ninja, C++20
│   └── tasks.json          ← Cmd+Shift+B = build. Also has "Run Standalone" task.
│
├── src/
│   ├── EuclideanSequencer.h/cpp   ← The rhythm algorithm
│   ├── SamplerVoice.h/cpp         ← The sample playback engine
│   ├── PluginProcessor.h/cpp      ← The audio brain (DSP thread)
│   └── PluginEditor.h/cpp         ← The UI (main thread)
│
└── build/                  ← Created on first build (gitignored)
    └── W2Sampler_artefacts/
        └── Debug/
            ├── Standalone/W2 Sampler.app   ← Run directly on Mac
            ├── VST3/W2 Sampler.vst3        ← Load in Ableton, Logic, Reaper, etc.
            ├── AU/W2 Sampler.component     ← Logic Pro / GarageBand format
            └── CLAP/W2 Sampler.clap        ← Modern open standard (Bitwig, Reaper)
```

---

## 3. How the Build Works

### CMake + FetchContent

CMake is the build system generator — it reads `CMakeLists.txt` and generates
Ninja build files (fast parallel compilation). The key concept here is
**FetchContent**: instead of manually downloading JUCE and copying it into the
project, CMake downloads it automatically from GitHub on first configure.

```
cmake -S . -B build -G Ninja    ← Configure (downloads deps, ~30s first time)
cmake --build build --parallel  ← Compile (parallel, ~60s first time, fast after)
```

After the first build, only changed `.cpp` files are recompiled — incremental
builds take a few seconds.

### What `juce_add_plugin` does

This single CMake call from JUCE handles an enormous amount of boilerplate:
- Registers the plugin with each format (VST3, AU, CLAP, Standalone)
- Generates the required Info.plist, PkgInfo, and code-signing for macOS
- Creates a shared static library (`libW2 Sampler_SharedCode.a`) that all
  format targets link against — your code compiles once, links into 4 formats

### C++20

We use `std::clamp`, `std::rotate`, lambda captures, and structured bindings.
C++20 is the modern baseline for audio plugin development.

---

## 4. What is JUCE?

JUCE (Jules' Utility Class Extensions) is a C++ framework specifically built for
audio applications. It provides:

- **Audio I/O abstraction** — talks to CoreAudio (Mac), WASAPI (Windows), ALSA
  (Linux) through one API
- **Plugin format wrappers** — your one `AudioProcessor` class gets wrapped
  automatically into VST3, AU, CLAP, Standalone
- **GUI system** — a complete 2D drawing + widget system (like a mini browser
  engine, but for audio UIs)
- **Audio utilities** — buffers, MIDI, format readers (WAV/AIFF/FLAC/MP3)

JUCE uses a **two-thread model**:
```
Audio thread  ──→  processBlock()   HIGH PRIORITY, never allocate memory, never lock
Message thread ──→ paint(), resized(), button callbacks   NORMAL PRIORITY
```

This is the single most important rule in audio programming: **do not block the
audio thread**. No `new`, no `delete`, no `std::mutex::lock()`, no file I/O.

---

## 5. Plugin Formats

| Format | File | Who uses it |
|--------|------|-------------|
| VST3   | `.vst3` | Ableton, Reaper, Cubase, FL Studio, most DAWs |
| AU     | `.component` | Logic Pro, GarageBand (macOS only) |
| CLAP   | `.clap` | Bitwig, Reaper (modern open standard) |
| Standalone | `.app` | Runs as a regular Mac app, great for development |

The Standalone format wraps your `AudioProcessor` in a `StandaloneFilterWindow`
which creates its own audio device connection and a simple settings menu. The
"Settings" button in the Standalone app opens CoreAudio device selection — on
some Macs with multiple audio interfaces this dialog can take a moment to appear.

---

## 6. Signal Flow

Here is what happens every ~10ms (at 512 samples, 48kHz):

```
Audio Hardware (CoreAudio)
        │
        ▼
processBlock(AudioBuffer, MidiBuffer)
        │
        ├── 1. advanceClock(numSamples)
        │         Counts samples. When enough have accumulated for one step,
        │         calls sequencer.tick(). If tick() returns true (a hit),
        │         calls voice.trigger().
        │
        ├── 2. voice.renderBlock(buffer, ...)
        │         If the voice is playing, it reads from the loaded sample
        │         buffer (with linear interpolation for pitch-correct
        │         resampling) and adds the audio to the output buffer.
        │
        ├── 3. MIDI input loop
        │         Any incoming MIDI Note On also triggers the voice, so you
        │         can play it from a keyboard even while the sequencer runs.
        │
        └── 4. Output → Audio Hardware
```

The **UI thread** runs separately at 20fps (timer-driven):
```
timerCallback() every 50ms
    │
    ├── syncSlidersFromParams()  — reads current param values, updates sliders
    └── repaint()                — redraws the step grid with current position
```

---

## 7. File-by-File Breakdown

### `CMakeLists.txt`
Declares the whole project. If you want to add a new `.cpp` file to compile,
add it to the `target_sources(W2Sampler PRIVATE ...)` block. If you want to
add a new JUCE module (e.g. `juce_dsp` for filters), add it to
`target_link_libraries`.

### `EuclideanSequencer.h` / `.cpp`
**Pure algorithm, no JUCE dependency.** Implements the Bjorklund algorithm to
compute euclidean rhythms. See section 8 for the math.
Key methods:
- `set(steps, hits, rotation)` — recompute the pattern. Does NOT reset playhead.
- `tick()` — advance one step, return true if it's a hit
- `getPattern()` — return the full bool array for drawing the UI grid
- `getStep()` — return current position (for the UI highlight)

### `SamplerVoice.h` / `.cpp`
**One voice, one sample.** No JUCE `Synthesiser` class — kept simple intentionally.
- `loadBuffer(buf, sampleRate)` — point at a loaded audio buffer
- `trigger(velocity)` — start playback from the beginning
- `renderBlock(output, start, num, playbackRate)` — mix into output with linear
  interpolation so the sample plays at the correct pitch even if the project
  sample rate differs from the file's sample rate

### `PluginProcessor.h` / `.cpp`
**The audio brain.** Subclasses `juce::AudioProcessor`.
- `prepareToPlay` — called when audio starts. Set up sample rate, reset clock.
- `processBlock` — the hot path called ~100x/sec. Does clock + voice rendering.
- `rebuildSequencer` — compares current param values to cached values. Only
  calls `sequencer.set()` if something actually changed, so the playhead is
  preserved during playback.
- `loadSample` — reads a file from disk using JUCE's `AudioFormatManager`
  (supports WAV, AIFF, FLAC, MP3, OGG). Called from the UI thread so it's
  safe to allocate memory here.
- `getStateInformation` / `setStateInformation` — called by the DAW to save/
  restore your plugin state in a project file.

### `PluginEditor.h` / `.cpp`
**The UI.** Subclasses `juce::AudioProcessorEditor`.
- Draws everything in `paint()` using JUCE's immediate-mode `Graphics` API
- Uses a `Timer` (20fps) to pull current state from the processor and repaint
- Sliders write directly to `AudioParameter` objects via `operator=`
- Play/Stop button toggles `proc.isPlaying_`

---

## 8. How the Euclidean Sequencer Works

A **Euclidean rhythm** answers the question: how do you distribute **k** hits
across **n** steps as evenly as possible?

**Example:** 4 hits in 16 steps → `[1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0]`
That's just 4/4 time. But 5 hits in 16 steps? The algorithm computes:
`[1,0,0,1, 0,0,1,0, 0,1,0,0, 1,0,0,0]` — a displaced, asymmetric groove.

This is called the **Bjorklund algorithm** (also known as Euclidean rhythms in
music, after Godfried Toussaint's paper "The Euclidean Algorithm Generates
Traditional Musical Rhythms" 2005).

### The Algorithm Step by Step

Start with two groups of sequences:
- `groups`    = k sequences of `[1]` (the hits)
- `remainder` = (n-k) sequences of `[0]` (the rests)

Repeatedly: append one remainder to one group, until remainder has ≤ 1 element.
Then flatten groups + remainder into the final pattern.

```
Example: n=8, k=3

Start:
  groups    = [[1],[1],[1]]
  remainder = [[0],[0],[0],[0],[0]]

Round 1: distribute 3 remainders onto 3 groups:
  groups    = [[1,0],[1,0],[1,0]]
  remainder = [[0],[0]]               (leftover)

Round 2: distribute 2 remainders onto 2 groups:
  groups    = [[1,0,0],[1,0,0]]
  remainder = [[1,0]]                 (the undistributed group becomes remainder)

Round 3: distribute 1 remainder onto 1 group:
  groups    = [[1,0,0,1,0]]
  remainder = [[1,0,0]]

Flatten: [1,0,0,1,0,1,0,0]
         ↑       ↑   ↑
         hit     hit hit   — 3 hits, as evenly spaced as possible
```

**Rotation** just shifts the array left by N positions, changing which step is
the "downbeat".

### Why this matters musically

Euclidean rhythms map onto many traditional world music patterns:
- E(3,8) = `[1,0,0,1,0,0,1,0]` — Cuban tresillo
- E(5,8) = `[1,0,1,1,0,1,1,0]` — Cuban cinquillo
- E(7,12) = West African bell pattern
- E(4,16) = basic 4-on-the-floor kick

---

## 9. How the Sampler Voice Works

`W2SamplerVoice` stores a pointer to a `juce::AudioBuffer<float>` (the loaded
sample) and a fractional playback position `position_` (a `double`).

### Linear Interpolation

When the sample file's rate (e.g. 44100 Hz) differs from the project rate
(e.g. 48000 Hz), we need to read the sample at a different speed. The ratio is:

```
ratio = sourceSampleRate / playbackSampleRate
      = 44100 / 48000 = 0.91875
```

For each output sample, we advance `position_` by `ratio`. Since position is
fractional, we interpolate between the two nearest samples:

```cpp
float s = sample[floor(pos)] * (1 - frac) + sample[ceil(pos)] * frac
```

This is **linear interpolation** — the simplest form of resampling. It's good
enough for one-shot samples. For pitch-shifting or high-quality playback, you'd
use higher-order interpolation (Hermite, sinc).

---

## 10. The Internal Clock

The clock converts time (in samples) into sequencer steps.

At 120 BPM:
```
seconds per beat = 60 / 120 = 0.5 sec
```

With `seqRate = 1.0` (1 step per beat) at 44100 Hz:
```
samplesPerStep = 44100 * 0.5 / 1.0 = 22050 samples
```

The clock accumulates samples in a counter. When the counter reaches
`samplesPerStep`, it wraps and calls `sequencer.tick()`. If tick returns true,
the voice fires.

```
[sample 0     ] [sample 1    ] ... [sample 22050] → tick! hit? → trigger voice
[sample 22051 ] ...                [sample 44100] → tick! hit? → trigger voice
```

Higher `seqRate` = more steps per beat = faster sequence.
More steps with same rate = the pattern takes longer per cycle.

---

## 11. The UI System

JUCE uses an **immediate-mode** drawing model in `paint()`. Every repaint:
- `paint()` is called with a `Graphics` object
- You call `g.fillRect()`, `g.drawText()` etc. to draw directly
- The result is composited to screen
- There is no retained DOM or widget tree for custom drawing

For standard controls (sliders, buttons), JUCE has a **component** system:
- `addAndMakeVisible(mySlider)` — adds the widget as a child
- `resized()` — you set pixel positions with `setBounds(x, y, w, h)`
- The component draws itself; you just position it

The 20fps timer (`startTimerHz(20)`) is the bridge between audio thread and UI:
- Audio thread writes to `AudioParameter` objects (atomic, thread-safe)
- Timer reads back those values and updates sliders + repaints the grid

**Rule:** Never read/write audio state directly from paint() without going
through AudioParameter (which uses atomic operations internally).

---

## 12. Parameters and State Saving

`juce::AudioParameterInt` and `juce::AudioParameterFloat` are thread-safe
parameter objects. They:
- Are registered with the `AudioProcessor` so the DAW can automate them
- Use atomic operations internally so the audio thread and UI thread can both
  read/write safely
- Are saved/restored by `getStateInformation` / `setStateInformation`

The DAW calls `getStateInformation` when saving a project and
`setStateInformation` when loading one. We serialise to a binary stream.
A future improvement: use `juce::AudioProcessorValueTreeState` (APVTS) which
handles all of this automatically including undo history.

---

## 13. Known Gotchas

### Standalone audio settings dialog
The standalone app has a settings gear icon that opens CoreAudio device
selection. On some Macs (especially with multiple audio interfaces or Bluetooth
audio), this dialog can take several seconds to populate. It is not frozen —
it's enumerating devices. Click away from it or wait for it to finish.

### VST3 assertion in build output
```
JUCE Assertion failure in juce_AudioProcessor.cpp:451
```
This comes from the `juce_vst3_helper` tool that generates `moduleinfo.json`
at the end of the build. It is a non-fatal debug assertion about bus naming —
the VST3 plugin works correctly in all tested DAWs.

### Hits > Steps
The `AudioParameterInt` for hits allows up to 32 hits but steps can be as low
as 1. The sequencer clamps `hits = min(hits, steps)` so you can't have more
hits than steps, but the slider UI can show an out-of-range number. This will
be fixed when we add APVTS with proper linked ranges.

---

## 14. Roadmap

### Phase 1 — Foundation (current)
- [x] JUCE 8 + CMake + Ninja build system
- [x] VST3 + AU + CLAP + Standalone from one codebase
- [x] Euclidean sequencer with animated grid
- [x] One-shot sample player with file loading
- [x] Play/Stop control

### Phase 2 — Proper sampler
- [ ] Multi-pad layout (8 or 16 pads, each with its own sample + sequencer)
- [ ] Pitch control (root note, pitch shift)
- [ ] Envelope (attack, decay, sustain, release per pad)
- [ ] Host BPM sync (via `AudioPlayHead`)
- [ ] MIDI note mapping (which MIDI note triggers which pad)

### Phase 3 — ML Integration
- [ ] Add ONNX Runtime via CMake FetchContent
- [ ] First ML feature: drum pattern generation (model generates hit patterns)
- [ ] Audio feature extraction (onset detection, spectral analysis)
- [ ] Neural stretch / pitch shift

### Phase 4 — UI Upgrade
- [ ] Replace JUCE native UI with WebView (CHOC library or JUCE WebBrowserComponent)
- [ ] HTML/CSS/JS frontend, C++ backend
- [ ] Hot-reloadable UI during development

### Phase 5 — Release
- [ ] Code signing + notarisation (Apple requires this for distribution)
- [ ] Windows build (CMake + MSVC or Clang-cl)
- [ ] Installer / drag-to-install packaging
