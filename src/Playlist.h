#pragma once
/**
 * Playlist — a named, ordered collection of audio files with their full FluCoMa
 * analysis embedded. Self-contained: loading a playlist restores all descriptors,
 * onset positions, key, BPM, UMAP coords and cluster — no re-analysis needed.
 *
 * Storage: ~/.w2sampler/playlists/<name>.json
 * JSON schema mirrors SampleDatabase cache entries, grouped under a named list.
 *
 * All methods are message-thread-only.
 */

#include <juce_core/juce_core.h>
#include "SampleDatabase.h"

// ─────────────────────────────────────────────────────────────────────────────
class Playlist
{
public:
    juce::String              name;

    // ── Persistence ──────────────────────────────────────────────────────────

    static juce::File playlistDir()
    {
        juce::File d = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                           .getChildFile (".w2sampler/playlists");
        d.createDirectory();
        return d;
    }

    static juce::File fileFor (const juce::String& playlistName)
    {
        return playlistDir().getChildFile (playlistName + ".json");
    }

    /** Append a SampleEntry to this playlist (deduplicates by hash). */
    void addEntry (const SampleEntry& e)
    {
        // Deduplicate
        for (const auto& ex : entries_)
            if (ex["hash"].toString() == e.hash) return;

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

        entries_.push_back (juce::var (obj));
        dirty_ = true;
    }

    /** Remove by hash. Returns true if found and removed. */
    bool removeEntry (const juce::String& hash)
    {
        for (int i = 0; i < (int)entries_.size(); ++i)
        {
            if (entries_[(size_t)i]["hash"].toString() == hash)
            {
                entries_.erase (entries_.begin() + i);
                dirty_ = true;
                return true;
            }
        }
        return false;
    }

    bool contains (const juce::String& hash) const
    {
        for (const auto& e : entries_)
            if (e["hash"].toString() == hash) return true;
        return false;
    }

    int size() const { return (int)entries_.size(); }

    /** Reconstruct SampleEntry from playlist slot i. Returns false on bad data. */
    bool getEntry (int i, SampleEntry& out) const
    {
        if (i < 0 || i >= (int)entries_.size()) return false;
        const auto& v = entries_[(size_t)i];

        out = SampleEntry();
        out.file       = juce::File (v["file"].toString());
        out.hash       = v["hash"].toString();
        out.analysedAt = v["analysedAt"].toString();
        out.duration   = (double)v["duration"];
        out.tempo      = (float)(double)v["tempo"];
        out.key        = (int)v["key"];
        out.keyConf    = (float)(double)v["keyConf"];
        out.keyName    = v["keyName"].toString();
        out.umap2d[0]  = (float)(double)v["umap_x"];
        out.umap2d[1]  = (float)(double)v["umap_y"];
        out.cluster    = (int)v["cluster"];
        out.valid      = true;

        auto* descArr = v["descriptor"].getArray();
        if (descArr && descArr->size() == 50)
            for (int d = 0; d < 50; ++d)
                out.descriptor[(size_t)d] = (float)(double)(*descArr)[(size_t)d];

        auto* onsetsArr = v["onsets"].getArray();
        if (onsetsArr)
            for (const auto& ov : *onsetsArr)
                out.analysis.onsetPositions.push_back ((float)(double)ov);

        out.analysis.estimatedBpm  = out.tempo;
        out.analysis.key           = out.key;
        out.analysis.keyConfidence = out.keyConf;
        out.analysis.keyName       = out.keyName;

        for (int c = 0; c < 13; ++c) out.analysis.mfccMean[(size_t)c]   = out.descriptor[(size_t)c];
        for (int c = 0; c < 13; ++c) out.analysis.mfccStd[(size_t)c]    = out.descriptor[(size_t)(13 + c)];
        for (int c = 0; c < 12; ++c) out.analysis.chromaMean[(size_t)c] = out.descriptor[(size_t)(26 + c)];
        for (int c = 0; c < 7;  ++c) out.analysis.shapeMean[(size_t)c]  = out.descriptor[(size_t)(38 + c)];
        out.analysis.pitchHz         = out.descriptor[45] * 4000.f;
        out.analysis.pitchConfidence = out.descriptor[46];

        return true;
    }

    juce::String getFilePath (int i) const
    {
        if (i < 0 || i >= (int)entries_.size()) return {};
        return entries_[(size_t)i]["file"].toString();
    }

    juce::String getEntryName (int i) const
    {
        juce::String path = getFilePath (i);
        return path.isEmpty() ? juce::String() : juce::File (path).getFileNameWithoutExtension();
    }

    // ── Serialisation ─────────────────────────────────────────────────────────

    bool save() const
    {
        juce::DynamicObject* root = new juce::DynamicObject();
        root->setProperty ("name",    name);
        root->setProperty ("version", 1);

        juce::Array<juce::var> arr;
        for (const auto& e : entries_) arr.add (e);
        root->setProperty ("entries", arr);

        juce::var rootVar (root);
        bool ok = fileFor (name).replaceWithText (juce::JSON::toString (rootVar, false));
        if (ok) dirty_ = false;
        return ok;
    }

    static bool load (const juce::String& playlistName, Playlist& out)
    {
        juce::File f = fileFor (playlistName);
        if (!f.existsAsFile()) return false;

        juce::var json;
        if (juce::JSON::parse (f.loadFileAsString(), json).failed()) return false;

        out.name    = json["name"].toString();
        out.entries_.clear();
        out.dirty_  = false;

        auto* arr = json["entries"].getArray();
        if (arr)
            for (const auto& e : *arr)
                out.entries_.push_back (e);

        return true;
    }

    /** List all saved playlist names (without .json extension). */
    static juce::StringArray listSaved()
    {
        juce::StringArray names;
        juce::Array<juce::File> files;
        playlistDir().findChildFiles (files, juce::File::findFiles, false, "*.json");
        files.sort();
        for (const auto& f : files)
            names.add (f.getFileNameWithoutExtension());
        return names;
    }

    bool isDirty() const { return dirty_; }

private:
    std::vector<juce::var> entries_;
    mutable bool           dirty_ = false;
};
