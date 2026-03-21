#pragma once
/**
 * SampleDatabase — message-thread-only singleton for persisted audio analysis.
 *
 * Revised threading model:
 *   - entries_ is message-thread-only. NEVER written from background thread.
 *   - Background worker does: file I/O + FluCoMa analysis (all local, no shared state).
 *   - Results are marshaled back via callAsync → storeEntry() / applyCorpusResults().
 *   - Static helpers (loadCacheEntry, writeCacheEntry, computeCorpus) are pure
 *     functions with no shared state — safe to call from any thread.
 *
 * Cache: ~/.w2sampler/cache/<fnv64(path)>.json
 */

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "FluCoMaAnalyser.h"

#include <array>
#include <vector>
#include <unordered_map>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
struct AnalysisSettings
{
    int   windowSize    = 1024;
    int   hopSize       = 512;
    int   mfccCoeff     = 13;
    int   onsetDebounce = 2;
};

// ─────────────────────────────────────────────────────────────────────────────
struct SampleEntry
{
    juce::File   file;
    juce::String hash;
    juce::String analysedAt;
    double  duration    = 0.0;
    float   tempo       = 0.f;
    int     key         = 0;
    float   keyConf     = 0.f;
    juce::String keyName;

    std::array<float, 50> descriptor = {};
    std::array<float, 2>  umap2d     = {};
    int                   cluster    = -1;

    juce::String metaKey;
    float        metaBpm = 0.f;

    FluCoMaResult analysis;
    bool valid = false;
};

/** One result from computeCorpus() — umap2d position + cluster index per entry. */
struct CorpusPoint
{
    std::array<float, 2> umap2d  = {};
    int                  cluster = -1;
};

// Cluster palette (8 colours, shared with CorpusView)
static constexpr uint32_t kClusterCols[8] = {
    0xff0A84FF, 0xffFF9F0A, 0xffBF5AF2, 0xff30D158,
    0xffFF453A, 0xffFFD60A, 0xff64D2FF, 0xffFF6961
};

// ─────────────────────────────────────────────────────────────────────────────
class SampleDatabase
{
public:
    static SampleDatabase& instance();

    int  size()    const { return (int)entries_.size(); }
    const std::vector<SampleEntry>& getEntries() const { return entries_; }
    const SampleEntry* getEntry (const juce::File& file) const;

    void              setSettings (AnalysisSettings s) { settings_ = s; }
    AnalysisSettings  getSettings() const              { return settings_; }

    /** Add a file immediately (valid=false) and read file metadata. Message thread only. */
    void addFileToList (const juce::File& file);

    /** Store (or replace) an entry. Message thread only — called via callAsync from worker. */
    void storeEntry (SampleEntry&& e);

    /** Apply KMeans+UMAP results to entries. Message thread only — called via callAsync. */
    void applyCorpusResults (const std::vector<juce::String>& hashes,
                             const std::vector<CorpusPoint>& pts);

    void clear() { entries_.clear(); entryMap_.clear(); }

    void removeEntry (int index)
    {
        if (index < 0 || index >= (int)entries_.size()) return;
        entries_.erase (entries_.begin() + (size_t)index);
        entryMap_.clear();
        for (int i = 0; i < (int)entries_.size(); ++i)
            entryMap_[entries_[(size_t)i].hash.toStdString()] = i;
    }

    // ── Static helpers — no shared state, safe from any thread ────────────────

    /** FNV-64 hash of absolute path — used as cache key. */
    static juce::String fileKey (const juce::File& f);
    static juce::File   cacheDir();

    /** Read cache JSON into out. Does NOT call storeEntry — pure file read.
     *  Returns false if cache missing, invalid, or hash mismatch. */
    static bool loadCacheEntry (const juce::File& file, SampleEntry& out);

    /** Write JSON cache for e. Reads no shared state — safe from background thread. */
    static void writeCacheEntry (const SampleEntry& e);

    /** Run KMeans + UMAP on descriptor snapshots. Pure computation, no shared state.
     *  Returns one CorpusPoint per descriptor row. Safe from background thread. */
    static std::vector<CorpusPoint> computeCorpus (
        const std::vector<std::array<float, 50>>& descriptors,
        int nClusters = 8, int seed = 42);

    // Metadata helpers — static, no shared state
    static juce::String parseMetaKey (const juce::String& raw);
    static juce::String parseMetaBpm (const juce::StringPairArray& meta);

private:
    SampleDatabase()  { formatManager_.registerBasicFormats(); }
    ~SampleDatabase() = default;

    AnalysisSettings                     settings_;
    std::vector<SampleEntry>             entries_;
    std::unordered_map<std::string, int> entryMap_;
    juce::AudioFormatManager             formatManager_;   // message thread only
};
