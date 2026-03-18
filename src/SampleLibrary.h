#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

/**
 * SampleLibrary — manages a folder of one-shot audio files.
 *
 * All methods are MESSAGE-THREAD ONLY except:
 *   - getCount()         — read-only, safe anywhere after load completes
 *   - getEntry(index)    — read-only, safe anywhere after load completes
 *   - advanceNext()      — audio-thread safe (uses atomic index)
 *   - advanceRandom(rng) — audio-thread safe (uses atomic index)
 *   - currentIndex       — public atomic, readable/writable from either thread
 *
 * Design: all sample data is decoded into AudioBuffer<float> objects IN MEMORY
 * when the folder is loaded. The audio thread never does file I/O — it just
 * reads from pre-loaded buffers, which is instantaneous and allocation-free.
 * This limits practical folder size to ~50–100 short one-shots (a few hundred MB),
 * which is fine for drum/one-shot libraries.
 *
 * Thread safety note: if loadFolder() is called while the audio thread is reading
 * from entries_, there is a benign race. The convention in this codebase is to
 * call setPlaying(false) before loading a folder. Flagged for future migration
 * to a lock-free handoff pattern.
 */
class SampleLibrary
{
public:
    /** One loaded sample: decoded audio + metadata. */
    struct Entry
    {
        juce::AudioBuffer<float> buffer;    // decoded PCM audio
        double                   sampleRate = 44100.0;  // original file sample rate
        juce::String             name;      // filename without extension (for UI display)
        juce::File               file;      // original file path
    };

    static constexpr int kMaxSamples = 128;  // safety cap

    //==========================================================================
    // MESSAGE THREAD

    /**
     * Scan folder, decode every supported audio file into entries_.
     * Clears previous contents first. Returns number of samples loaded.
     * formatMgr must already have formats registered (call registerBasicFormats first).
     */
    int loadFolder (const juce::File& folder, juce::AudioFormatManager& formatMgr)
    {
        entries_.clear();
        currentIndex.store (0);

        if (! folder.isDirectory()) return 0;

        // Collect matching files, sorted alphabetically
        juce::Array<juce::File> found;
        for (const auto& ext : { "wav", "aif", "aiff", "flac", "mp3", "ogg" })
            folder.findChildFiles (found, juce::File::findFiles, false,
                                   juce::String ("*.") + ext);
        found.sort();

        int loaded = 0;
        for (auto& f : found)
        {
            if (loaded >= kMaxSamples) break;

            std::unique_ptr<juce::AudioFormatReader> reader (
                formatMgr.createReaderFor (f));
            if (reader == nullptr) continue;

            auto e = std::make_unique<Entry>();
            e->sampleRate = reader->sampleRate;
            e->name       = f.getFileNameWithoutExtension();
            e->file       = f;

            // Decode entire file into a float buffer
            e->buffer.setSize ((int) reader->numChannels,
                               (int) reader->lengthInSamples);
            reader->read (&e->buffer, 0, (int) reader->lengthInSamples, 0,
                          /*readLeft=*/ true, /*readRight=*/ true);

            entries_.push_back (std::move (e));
            ++loaded;
        }
        return loaded;
    }

    bool isEmpty()    const { return entries_.empty(); }
    int  getCount()   const { return (int) entries_.size(); }

    /** Get entry by absolute index. Safe to call from any thread after load completes. */
    const Entry* getEntry (int index) const
    {
        if (isEmpty() || index < 0 || index >= (int) entries_.size())
            return nullptr;
        return entries_[(size_t) index].get();
    }

    /** Get the entry currently selected by currentIndex. */
    const Entry* current() const
    {
        return getEntry (currentIndex.load());
    }

    juce::String currentName() const
    {
        auto* e = current();
        return e ? e->name : juce::String ("none");
    }

    //==========================================================================
    // SAFE FROM AUDIO THREAD (atomic index operations, no allocation)

    /**
     * Advance to the next sample sequentially (wraps).
     * Returns the new index. Safe to call from audio thread.
     */
    int advanceNext()
    {
        if (isEmpty()) return 0;
        int next = (currentIndex.load() + 1) % (int) entries_.size();
        currentIndex.store (next);
        return next;
    }

    /**
     * Advance to a random sample using caller-provided random state.
     * We pass a uint32_t& so the caller owns the RNG state — no shared state.
     * Returns the new index.
     */
    int advanceRandom (uint32_t& rngState)
    {
        if (isEmpty()) return 0;
        // LCG: fast, cheap, good enough for sample selection
        rngState = rngState * 1664525u + 1013904223u;
        int next = (int)(rngState >> 16) % (int) entries_.size();
        currentIndex.store (next);
        return next;
    }

    /** Navigate (message thread) — wraps around. */
    void prev()
    {
        if (isEmpty()) return;
        int n = (currentIndex.load() - 1 + (int) entries_.size()) % (int) entries_.size();
        currentIndex.store (n);
    }

    void next()
    {
        if (isEmpty()) return;
        currentIndex.store ((currentIndex.load() + 1) % (int) entries_.size());
    }

    void pickRandom()
    {
        if (isEmpty()) return;
        currentIndex.store (juce::Random::getSystemRandom().nextInt ((int) entries_.size()));
    }

    //==========================================================================
    // Public atomic — readable from audio thread, writable from either thread
    std::atomic<int> currentIndex { 0 };

private:
    // Heap-allocated entries so resize doesn't invalidate existing pointers.
    // (push_back into vector<unique_ptr> only invalidates the vector's internal
    // array, not the pointed-to objects — so existing pointers remain valid.)
    std::vector<std::unique_ptr<Entry>> entries_;
};
