/**
 * SampleDatabase implementation.
 * Heavy FluCoMa (KMeans / UMAP) headers kept here — only one translation unit.
 */

#include "SampleDatabase.h"
#include "FluCoMaAnalyser.h"

#include <flucoma/algorithms/public/KMeans.hpp>
#include <flucoma/algorithms/public/UMAP.hpp>
#include <flucoma/data/FluidDataSet.hpp>
#include <flucoma/data/FluidIndex.hpp>
#include <flucoma/data/FluidMemory.hpp>

#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
SampleDatabase& SampleDatabase::instance()
{
    static SampleDatabase db;
    return db;
}

// ─────────────────────────────────────────────────────────────────────────────
juce::String SampleDatabase::fileKey (const juce::File& f)
{
    std::string s = f.getFullPathName().toStdString();
    uint64_t    h = 14695981039346656037ULL;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
    char buf[17];
    std::snprintf (buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return juce::String (buf);
}

juce::File SampleDatabase::cacheDir()
{
    juce::File dir = juce::File::getSpecialLocation (
        juce::File::userHomeDirectory).getChildFile (".w2sampler/cache");
    dir.createDirectory();
    return dir;
}

// ─────────────────────────────────────────────────────────────────────────────
const SampleEntry* SampleDatabase::getEntry (const juce::File& file) const
{
    auto key = fileKey (file);
    auto it  = entryMap_.find (key.toStdString());
    return (it != entryMap_.end()) ? &entries_[(size_t)it->second] : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Metadata helpers
// ─────────────────────────────────────────────────────────────────────────────
juce::String SampleDatabase::parseMetaBpm (const juce::StringPairArray& meta)
{
    for (const char* k : { "BPM", "TBPM", "bpm", "Tempo", "tempo", "IBPM", "BeatsPerMinute" })
        if (meta.containsKey (k))
        {
            juce::String v = meta.getValue (k, "");
            if (v.isNotEmpty()) return v;
        }
    return {};
}

juce::String SampleDatabase::parseMetaKey (const juce::String& rawIn)
{
    juce::String raw = rawIn.trim();
    if (raw.isEmpty()) return {};

    // Camelot notation: "8A" = Am, "8B" = A, etc.
    static const char* camelotMinor[] = { "Am","Em","Bm","F#m","Dbm","Abm","Ebm","Bbm","Fm","Cm","Gm","Dm" };
    static const char* camelotMajor[] = { "C", "G", "D", "A",  "E",  "B",  "F#", "Db", "Ab","Eb","Bb","F"  };
    juce::juce_wchar last = raw.getLastCharacter();
    if (last == 'A' || last == 'B' || last == 'a' || last == 'b')
    {
        int num = raw.dropLastCharacters (1).getIntValue();
        if (num >= 1 && num <= 12)
        {
            bool minor = (last == 'A' || last == 'a');
            return juce::String (minor ? camelotMinor[(size_t)(num - 1)]
                                       : camelotMajor[(size_t)(num - 1)])
                   + " " + (minor ? "minor" : "major");
        }
    }

    bool isMinor = raw.containsIgnoreCase ("minor") || raw.containsIgnoreCase ("min");
    bool isMajor = raw.containsIgnoreCase ("major") || raw.containsIgnoreCase ("maj");

    juce::String root = raw;
    for (const char* suffix : { "minor","major","min","maj","m","M" })
    {
        if (root.endsWithIgnoreCase (suffix))
        {
            juce::String stripped = root.dropLastCharacters ((int)std::strlen (suffix)).trim();
            if (stripped.isNotEmpty())
            {
                if (std::strcmp (suffix, "m") == 0 && !isMinor && !isMajor)
                    isMinor = true;
                root = stripped;
                break;
            }
        }
    }

    if (!isMajor && !isMinor) isMajor = true;
    return root + " " + (isMinor ? "minor" : "major");
}

// ─────────────────────────────────────────────────────────────────────────────
void SampleDatabase::addFileToList (const juce::File& file)
{
    juce::String key = fileKey (file);
    if (entryMap_.find (key.toStdString()) != entryMap_.end()) return;

    // Register path immediately — NO reader creation on message thread.
    // Duration/metadata populated later via CheckCache or AnalyseFile background jobs.
    SampleEntry e;
    e.file  = file;
    e.hash  = key;
    e.valid = false;
    storeEntry (std::move (e));
}

// ─────────────────────────────────────────────────────────────────────────────
void SampleDatabase::storeEntry (SampleEntry&& e)
{
    std::string key = e.hash.toStdString();
    auto it = entryMap_.find (key);
    if (it != entryMap_.end())
        entries_[(size_t)it->second] = std::move (e);
    else
    {
        entryMap_[key] = (int)entries_.size();
        entries_.push_back (std::move (e));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void SampleDatabase::applyCorpusResults (const std::vector<juce::String>& hashes,
                                         const std::vector<CorpusPoint>& pts)
{
    for (size_t i = 0; i < hashes.size() && i < pts.size(); ++i)
    {
        auto it = entryMap_.find (hashes[i].toStdString());
        if (it == entryMap_.end()) continue;
        auto& e   = entries_[(size_t)it->second];
        e.umap2d  = pts[i].umap2d;
        e.cluster = pts[i].cluster;
        writeCacheEntry (e);   // persist updated umap+cluster
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers — no shared state, safe from background thread
// ─────────────────────────────────────────────────────────────────────────────

bool SampleDatabase::loadCacheEntry (const juce::File& file, SampleEntry& out)
{
    juce::String key       = fileKey (file);
    juce::File   cacheFile = cacheDir().getChildFile (key + ".json");
    if (!cacheFile.existsAsFile()) return false;

    juce::var json;
    if (juce::JSON::parse (cacheFile.loadFileAsString(), json).failed()) return false;

    if (json["hash"].toString() != key) return false;

    SampleEntry e;
    e.file       = file;
    e.hash       = key;
    e.analysedAt = json["analysedAt"].toString();
    e.duration   = (double)json["duration"];
    e.tempo      = (float)(double)json["tempo"];
    e.key        = (int)json["key"];
    e.keyConf    = (float)(double)json["keyConf"];
    e.keyName    = json["keyName"].toString();
    e.umap2d[0]  = (float)(double)json["umap_x"];
    e.umap2d[1]  = (float)(double)json["umap_y"];
    e.cluster    = (int)json["cluster"];
    e.valid      = true;

    auto* descArr = json["descriptor"].getArray();
    if (descArr && descArr->size() == 50)
        for (int i = 0; i < 50; ++i)
            e.descriptor[(size_t)i] = (float)(double)(*descArr)[(size_t)i];

    auto* onsets = json["onsets"].getArray();
    if (onsets)
        for (const auto& v : *onsets)
            e.analysis.onsetPositions.push_back ((float)(double)v);

    e.analysis.estimatedBpm  = e.tempo;
    e.analysis.key           = e.key;
    e.analysis.keyConfidence = e.keyConf;
    e.analysis.keyName       = e.keyName;
    e.analysis.descriptor    = e.descriptor;

    // Reconstruct detail fields from flat descriptor
    for (int c = 0; c < 13; ++c) e.analysis.mfccMean[(size_t)c]   = e.descriptor[(size_t)c];
    for (int c = 0; c < 13; ++c) e.analysis.mfccStd[(size_t)c]    = e.descriptor[(size_t)(13 + c)];
    for (int c = 0; c < 12; ++c) e.analysis.chromaMean[(size_t)c] = e.descriptor[(size_t)(26 + c)];
    for (int c = 0; c < 7;  ++c) e.analysis.shapeMean[(size_t)c]  = e.descriptor[(size_t)(38 + c)];
    e.analysis.pitchHz         = e.descriptor[45] * 4000.f;
    e.analysis.pitchConfidence = e.descriptor[46];

    out = std::move (e);
    return true;
}

void SampleDatabase::writeCacheEntry (const SampleEntry& e)
{
    juce::DynamicObject* obj = new juce::DynamicObject();
    obj->setProperty ("hash",       e.hash);
    obj->setProperty ("file",       e.file.getFullPathName());
    obj->setProperty ("analysedAt", e.analysedAt);
    obj->setProperty ("duration",   e.duration);
    obj->setProperty ("tempo",      (double)e.tempo);
    obj->setProperty ("key",        e.key);
    obj->setProperty ("keyConf",    (double)e.keyConf);
    obj->setProperty ("keyName",    e.keyName);
    obj->setProperty ("umap_x",     (double)e.umap2d[0]);
    obj->setProperty ("umap_y",     (double)e.umap2d[1]);
    obj->setProperty ("cluster",    e.cluster);

    juce::Array<juce::var> descArr;
    for (float v : e.descriptor) descArr.add ((double)v);
    obj->setProperty ("descriptor", descArr);

    juce::Array<juce::var> onsetsArr;
    for (float v : e.analysis.onsetPositions) onsetsArr.add ((double)v);
    obj->setProperty ("onsets", onsetsArr);

    juce::var root (obj);
    cacheDir().getChildFile (e.hash + ".json")
              .replaceWithText (juce::JSON::toString (root, false));
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<CorpusPoint> SampleDatabase::computeCorpus (
    const std::vector<std::array<float, 50>>& descriptors,
    int nClusters, int seed)
{
    using fidx    = fluid::index;
    using DataSet = fluid::FluidDataSet<std::string, double, 1>;
    constexpr int kDescLen = 50;

    int row = (int)descriptors.size();
    if (row < 2) return {};

    fluid::FluidTensor<std::string, 1> ids  ((fidx)row);
    fluid::FluidTensor<double, 2>      data ((fidx)row, (fidx)kDescLen);

    for (int r = 0; r < row; ++r)
    {
        ids ((fidx)r) = std::to_string (r);
        for (int d = 0; d < kDescLen; ++d)
            data ((fidx)r, (fidx)d) = (double)descriptors[(size_t)r][(size_t)d];
    }

    DataSet ds (ids, data);

    fluid::algorithm::KMeans km;
    int k = std::min (nClusters, row);
    km.train (ds, (fidx)k, 100,
              fluid::algorithm::KMeans::InitMethod::randomSampling, (fidx)seed);

    fluid::algorithm::UMAP umap;
    int umapK = std::min (15, row - 1);
    DataSet umapResult = umap.train (ds, (fidx)umapK, 2, 0.1, 200, 1.0, (fidx)seed);
    (void)umapResult;

    fluid::FluidTensor<double, 2> emb ((fidx)row, (fidx)2);
    umap.getEmbedding (emb);

    fluid::FluidTensor<double, 1> pt ((fidx)kDescLen);

    std::vector<CorpusPoint> result ((size_t)row);
    for (int r = 0; r < row; ++r)
    {
        for (int d = 0; d < kDescLen; ++d)
            pt ((fidx)d) = (double)descriptors[(size_t)r][(size_t)d];
        result[(size_t)r].cluster   = (int)km.vq (pt);
        result[(size_t)r].umap2d[0] = (float)emb ((fidx)r, (fidx)0);
        result[(size_t)r].umap2d[1] = (float)emb ((fidx)r, (fidx)1);
    }
    return result;
}
