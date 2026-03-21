#pragma once
/**
 * SoundBrowser — detachable sound analysis + slice corpus browser.
 *
 * Architecture:
 *   • File list  : one row per file, immediate on add (unanalysed OK)
 *   • Waveform   : full-height onset lines, playhead, highlighted slice region
 *   • Analysis   : detected key + metadata key side-by-side, BPM, pitch, MFCC
 *   • Settings   : window / hop / MFCC count / onset threshold controls
 *   • Corpus     : UMAP 2D scatter of onset SLICES (not whole files)
 *   • Background : AnalysisWorker thread processes file queue; auto-rebuilds
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "SampleDatabase.h"
#include "Playlist.h"
#include "PluginProcessor.h"

#if defined (__APPLE__)
#include <pthread.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Pre-computed waveform thumbnail — 1024 min/max pairs built on worker thread.
// Paint uses this O(width) lookup instead of scanning millions of raw samples.
// ─────────────────────────────────────────────────────────────────────────────
struct WaveThumb
{
    static constexpr int kSize = 1024;
    struct Pt { float lo = 0.f, hi = 0.f; };
    std::vector<Pt> pts;   // kSize entries, built by worker thread

    /** Build thumbnail with a single sequential pass — 4096-sample working buffer,
     *  no large allocation regardless of file length. Safe from background thread. */
    static WaveThumb buildFromReader (juce::AudioFormatReader& rdr)
    {
        WaveThumb t;
        const int64_t len = rdr.lengthInSamples;
        const int     ch  = (int)rdr.numChannels;
        if (len == 0 || ch == 0) return t;
        t.pts.resize (kSize, { 0.f, 0.f });

        const int chunkSz = 4096;
        juce::AudioBuffer<float> work (ch, chunkSz);
        int64_t pos = 0;
        while (pos < len)
        {
            int n = (int)std::min ((int64_t)chunkSz, len - pos);
            work.clear();
            rdr.read (&work, 0, n, pos, true, true);
            for (int s = 0; s < n; ++s)
            {
                int px = (int)((pos + (int64_t)s) * (int64_t)kSize / len);
                px = juce::jlimit (0, kSize - 1, px);
                for (int c = 0; c < ch; ++c)
                {
                    float v = work.getSample (c, s);
                    if (v > t.pts[(size_t)px].hi) t.pts[(size_t)px].hi = v;
                    if (v < t.pts[(size_t)px].lo) t.pts[(size_t)px].lo = v;
                }
            }
            pos += n;
        }
        return t;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Background analysis worker
//
// Threading contract:
//   - Background thread ONLY: file I/O, FluCoMa analysis, JSON read/write.
//     All work is on local variables — no shared SampleDatabase state touched.
//   - Results are marshaled to the message thread via callAsync.
//   - storeEntry() / applyCorpusResults() run on message thread inside callAsync.
// ─────────────────────────────────────────────────────────────────────────────
class AnalysisWorker : public juce::Thread
{
public:
    enum class JobType { AnalyseFile, BuildCorpus, LoadWaveform, LoadPreview, CheckCache };
    struct Job
    {
        JobType    type;
        juce::File file;
        float      sensitivity     = 0.5f;
        bool       forceReanalysis = false;
        AnalysisSettings settings;   // copied at queue time; bg thread never reads settings_
        // LoadWaveform: thumbnail only, buf arg is always nullptr (no large allocation)
        std::function<void(std::shared_ptr<juce::AudioBuffer<float>>, double, WaveThumb)> waveCallback;
        // LoadPreview: fires with decoded buffer (30s max) when user presses Play
        std::function<void(std::shared_ptr<juce::AudioBuffer<float>>, double)> previewCallback;
        // BuildCorpus: descriptor + hash snapshot captured on message thread
        std::vector<std::array<float, 50>> descriptors;
        std::vector<juce::String>          hashes;
    };

    std::function<void()> onProgress;   // called on message thread after each step

    AnalysisWorker() : juce::Thread ("W2AnalysisWorker")
    {
        // Register codecs here (message thread) — CoreAudio codec init from a
        // background thread on macOS is expensive and contends with the run loop.
        fmtMgr_.registerBasicFormats();
        startThread (juce::Thread::Priority::background);
    }
    ~AnalysisWorker() override { stopThread (5000); }

    void addFileJob (juce::File f, float sens, bool forceReanalysis = false)
    {
        Job j;
        j.type            = JobType::AnalyseFile;
        j.file            = std::move (f);
        j.sensitivity     = sens;
        j.forceReanalysis = forceReanalysis;
        j.settings        = SampleDatabase::instance().getSettings();  // copy on msg thread
        const juce::ScopedLock lock (queueLock_);
        queue_.push_back (std::move (j));
        notify();
    }

    /** Read-only cache lookup — no audio I/O, no FluCoMa. Fast.
     *  If JSON cache hit: marshals SampleEntry to message thread via callAsync.
     *  If miss: does nothing (file stays valid=false until user clicks Analyse). */
    void addCheckCacheJob (juce::File f)
    {
        Job j;
        j.type = JobType::CheckCache;
        j.file = std::move (f);
        const juce::ScopedLock lock (queueLock_);
        queue_.push_back (std::move (j));
        notify();
    }

    /** Build waveform thumbnail from file (sequential read, no large alloc).
     *  Callback fires on message thread with buf=nullptr and the WaveThumb. */
    void addWaveformJob (juce::File f,
                         std::function<void(std::shared_ptr<juce::AudioBuffer<float>>, double, WaveThumb)> cb)
    {
        Job j;
        j.type         = JobType::LoadWaveform;
        j.file         = std::move (f);
        j.waveCallback = std::move (cb);
        const juce::ScopedLock lock (queueLock_);
        queue_.insert (queue_.begin(), std::move (j));  // front — show waveform first
        notify();
    }

    /** Load up to 30s of audio for preview playback. Inserts at front of queue.
     *  Callback fires on message thread with the decoded buffer. */
    void addPreviewJob (juce::File f,
                        std::function<void(std::shared_ptr<juce::AudioBuffer<float>>, double)> cb)
    {
        Job j;
        j.type            = JobType::LoadPreview;
        j.file            = std::move (f);
        j.previewCallback = std::move (cb);
        const juce::ScopedLock lock (queueLock_);
        queue_.insert (queue_.begin(), std::move (j));  // front — play ASAP
        notify();
    }

    /** Snapshot descriptors from message thread, queue KMeans+UMAP job.
     *  Must be called from message thread (reads entries_). */
    void queueRebuildCorpus()
    {
        const auto& entries = SampleDatabase::instance().getEntries();
        Job j;
        j.type = JobType::BuildCorpus;
        for (const auto& e : entries)
            if (e.valid) { j.descriptors.push_back (e.descriptor); j.hashes.push_back (e.hash); }
        if (j.descriptors.empty()) return;
        const juce::ScopedLock lock (queueLock_);
        queue_.push_back (std::move (j));
        notify();
    }

    bool isBusy()       const { return busy_.load(); }
    int  pendingCount() const
    {
        const juce::ScopedLock lock (queueLock_);
        return (int)queue_.size() + (busy_.load() ? 1 : 0);
    }

    void run() override
    {
       #if defined (__APPLE__)
        // UTILITY: gets more CPU than BACKGROUND while still never competing with
        // the real-time CoreAudio thread (which runs at THREAD_TIME_CONSTRAINT_POLICY).
        pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
       #endif

        while (!threadShouldExit())
        {
            Job job;
            bool hasJob = false;
            {
                const juce::ScopedLock lock (queueLock_);
                if (!queue_.empty())
                {
                    job = std::move (queue_.front());
                    queue_.erase (queue_.begin());
                    hasJob = true;
                }
            }
            // Wait OUTSIDE the lock — message thread must never block on queueLock_
            if (!hasJob) { wait (200); continue; }

            busy_.store (true);

            // ── Analyse file ──────────────────────────────────────────────────
            if (job.type == JobType::AnalyseFile && job.file.existsAsFile())
            {
                // 1. Try JSON cache — pure file read, no shared state
                SampleEntry cached;
                if (!job.forceReanalysis && SampleDatabase::loadCacheEntry (job.file, cached))
                {
                    juce::MessageManager::callAsync (
                        [this, e = std::move (cached)]() mutable
                        {
                            SampleDatabase::instance().storeEntry (std::move (e));
                            if (onProgress) onProgress();
                        });
                    busy_.store (false);
                    continue;
                }

                // 2. Read audio
                auto reader = std::unique_ptr<juce::AudioFormatReader> (
                    fmtMgr_.createReaderFor (job.file));
                if (!reader)
                {
                    juce::MessageManager::callAsync ([this] { if (onProgress) onProgress(); });
                    busy_.store (false);
                    continue;
                }

                int64_t nSmp = reader->lengthInSamples;
                juce::AudioBuffer<float> buf ((int)reader->numChannels, (int)nSmp);
                reader->read (&buf, 0, (int)nSmp, 0, true, true);
                double sr = reader->sampleRate;

                // 3. Read metadata tags
                const auto& meta = reader->metadataValues;
                juce::String metaKey;
                for (const char* k : { "TKEY","initialkey","key","Key","IKEY","KeyWords" })
                {
                    juce::String v = meta.getValue (k, "");
                    if (v.isNotEmpty()) { metaKey = SampleDatabase::parseMetaKey (v); break; }
                }
                juce::String metaBpmStr = SampleDatabase::parseMetaBpm (meta);

                // 4. Run FluCoMa — CPU intensive, all local
                FluCoMaAnalyser fa;
                fa.windowSize     = job.settings.windowSize;
                fa.fftSize        = job.settings.windowSize;
                fa.hopSize        = job.settings.hopSize;
                fa.nMfccCoeff     = job.settings.mfccCoeff;
                fa.onsetThreshold = (double)(1.0f - job.sensitivity) * 0.5 + 0.05;
                fa.onsetDebounce  = job.settings.onsetDebounce;
                FluCoMaResult result = fa.analyse (buf, sr);

                // 5. Build complete SampleEntry locally — no shared state
                SampleEntry e;
                e.file       = job.file;
                e.hash       = SampleDatabase::fileKey (job.file);
                e.analysedAt = juce::Time::getCurrentTime().toISO8601 (true);
                e.duration   = (double)nSmp / sr;
                e.tempo      = result.estimatedBpm;
                e.key        = result.key;
                e.keyConf    = result.keyConfidence;
                e.keyName    = result.keyName;
                e.metaKey    = metaKey;
                e.metaBpm    = metaBpmStr.getFloatValue();
                e.descriptor = result.descriptor;
                e.analysis   = std::move (result);
                e.valid      = true;

                // 6. Write JSON cache — file I/O, no shared state
                SampleDatabase::writeCacheEntry (e);

                // 7. Marshal to message thread
                juce::MessageManager::callAsync (
                    [this, entry = std::move (e)]() mutable
                    {
                        SampleDatabase::instance().storeEntry (std::move (entry));
                        if (onProgress) onProgress();
                    });
            }

            // ── Cache-only lookup (no audio I/O, no analysis) ────────────────
            // Runs quickly — just reads a small JSON file from ~/.w2sampler/cache/.
            // Restores previously-analysed metadata for files added to the list.
            else if (job.type == JobType::CheckCache && job.file.existsAsFile())
            {
                SampleEntry cached;
                if (SampleDatabase::loadCacheEntry (job.file, cached))
                {
                    juce::MessageManager::callAsync (
                        [this, e = std::move (cached)]() mutable
                        {
                            SampleDatabase::instance().storeEntry (std::move (e));
                            if (onProgress) onProgress();
                        });
                }
            }

            // ── KMeans + UMAP corpus ──────────────────────────────────────────
            else if (job.type == JobType::BuildCorpus && !job.descriptors.empty())
            {
                // Pure computation on snapshot — no shared state
                auto pts    = SampleDatabase::computeCorpus (job.descriptors);
                auto hashes = std::move (job.hashes);
                juce::MessageManager::callAsync (
                    [this, pts = std::move (pts), hashes = std::move (hashes)]() mutable
                    {
                        SampleDatabase::instance().applyCorpusResults (hashes, pts);
                        if (onProgress) onProgress();
                    });
            }

            // ── Load waveform thumbnail (no large allocation) ─────────────────
            // Reads entire file sequentially with a 4096-sample working buffer.
            // Never allocates a large AudioBuffer — no heap pressure, no malloc stalls.
            else if (job.type == JobType::LoadWaveform
                     && job.file.existsAsFile() && job.waveCallback)
            {
                auto reader = std::unique_ptr<juce::AudioFormatReader> (
                    fmtMgr_.createReaderFor (job.file));
                if (reader)
                {
                    double    sr    = reader->sampleRate;
                    WaveThumb thumb = WaveThumb::buildFromReader (*reader);
                    auto cb = job.waveCallback;
                    // Pass nullptr for buf — preview loads separately on Play press
                    juce::MessageManager::callAsync (
                        [cb, sr, th = std::move (thumb)]() mutable
                        { cb (nullptr, sr, std::move (th)); });
                }
            }

            // ── Load preview buffer on demand (Play pressed) ──────────────────
            // Reads at most 30s — manageable allocation, only when user asks to play.
            else if (job.type == JobType::LoadPreview
                     && job.file.existsAsFile() && job.previewCallback)
            {
                auto reader = std::unique_ptr<juce::AudioFormatReader> (
                    fmtMgr_.createReaderFor (job.file));
                if (reader)
                {
                    double  sr   = reader->sampleRate;
                    int64_t nSmp = std::min (reader->lengthInSamples,
                                             (int64_t)(sr * 30.0));   // 30s max
                    auto buf = std::make_shared<juce::AudioBuffer<float>> (
                        (int)reader->numChannels, (int)nSmp);
                    reader->read (buf.get(), 0, (int)nSmp, 0, true, true);
                    auto cb = job.previewCallback;
                    juce::MessageManager::callAsync (
                        [cb, buf, sr]() mutable { cb (buf, sr); });
                }
            }

            busy_.store (false);
        }
    }

private:
    mutable juce::CriticalSection  queueLock_;
    std::vector<Job>               queue_;
    std::atomic<bool>              busy_ { false };
    juce::AudioFormatManager       fmtMgr_;   // registered on message thread, read-only after
};

// ─────────────────────────────────────────────────────────────────────────────
// CorpusView — UMAP 2D scatter of whole-file SampleEntry dots
// ─────────────────────────────────────────────────────────────────────────────
class CorpusView : public juce::Component
{
public:
    std::function<void(int fileIdx)> onSelect;

    void refresh (const std::vector<SampleEntry>& entries, int selected = -1)
    {
        entries_  = &entries;
        selected_ = selected;
        minX_ = minY_ =  1e9f;
        maxX_ = maxY_ = -1e9f;
        for (const auto& e : entries)
        {
            if (!e.valid || (e.umap2d[0] == 0.f && e.umap2d[1] == 0.f)) continue;
            minX_ = std::min (minX_, e.umap2d[0]); maxX_ = std::max (maxX_, e.umap2d[0]);
            minY_ = std::min (minY_, e.umap2d[1]); maxY_ = std::max (maxY_, e.umap2d[1]);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.fillAll (juce::Colour (0xff151518));
        g.setColour (juce::Colour (0xff2A2A2E)); g.drawRect (b, 1);

        if (entries_ == nullptr || entries_->empty())
        {
            g.setColour (juce::Colour (0xff6E6E73)); g.setFont (11.f);
            g.drawText ("No files — add files, Analyse, then Rebuild Corpus",
                        b, juce::Justification::centred);
            return;
        }

        bool hasUmap = false;
        for (const auto& e : *entries_)
            if (e.valid && (e.umap2d[0] != 0.f || e.umap2d[1] != 0.f))
                { hasUmap = true; break; }

        if (!hasUmap)
        {
            g.setColour (juce::Colour (0xff6E6E73)); g.setFont (11.f);
            g.drawText (juce::String ((int)entries_->size()) + " files — click Rebuild Corpus to layout",
                        b, juce::Justification::centred);
            return;
        }

        float rangeX = maxX_ - minX_; if (rangeX < 1e-6f) rangeX = 1.f;
        float rangeY = maxY_ - minY_; if (rangeY < 1e-6f) rangeY = 1.f;
        const float m = 10.f;
        float w = (float)b.getWidth() - 2.f * m;
        float h = (float)b.getHeight() - 2.f * m;

        for (int i = 0; i < (int)entries_->size(); ++i)
        {
            const auto& e = (*entries_)[(size_t)i];
            if (!e.valid || (e.umap2d[0] == 0.f && e.umap2d[1] == 0.f)) continue;
            float nx = (e.umap2d[0] - minX_) / rangeX;
            float ny = 1.f - (e.umap2d[1] - minY_) / rangeY;
            float px = m + nx * w, py = m + ny * h;
            float r  = (i == selected_) ? 7.f : 4.f;
            int   ci = (e.cluster >= 0 && e.cluster < 8) ? e.cluster : 0;
            g.setColour (juce::Colour (kClusterCols[ci]).withAlpha (i == selected_ ? 1.f : 0.7f));
            g.fillEllipse (px - r, py - r, r * 2.f, r * 2.f);
            if (i == selected_) { g.setColour (juce::Colours::white); g.drawEllipse (px-r,py-r,r*2.f,r*2.f,1.5f); }
        }
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        if (entries_ == nullptr) return;
        auto  b = getLocalBounds();
        float rangeX = maxX_ - minX_; if (rangeX < 1e-6f) rangeX = 1.f;
        float rangeY = maxY_ - minY_; if (rangeY < 1e-6f) rangeY = 1.f;
        const float m = 10.f;
        float w = (float)b.getWidth() - 2.f * m;
        float h = (float)b.getHeight() - 2.f * m;
        int bestIdx = -1; float bestDist = 20.f;
        for (int i = 0; i < (int)entries_->size(); ++i)
        {
            const auto& e = (*entries_)[(size_t)i];
            if (!e.valid || (e.umap2d[0] == 0.f && e.umap2d[1] == 0.f)) continue;
            float nx = (e.umap2d[0] - minX_) / rangeX;
            float ny = 1.f - (e.umap2d[1] - minY_) / rangeY;
            float d  = std::hypot ((float)ev.x - (m + nx * w), (float)ev.y - (m + ny * h));
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        if (bestIdx >= 0) { selected_ = bestIdx; repaint(); if (onSelect) onSelect (bestIdx); }
    }

private:
    const std::vector<SampleEntry>* entries_ = nullptr;
    int   selected_ = -1;
    float minX_ = 0.f, maxX_ = 1.f, minY_ = 0.f, maxY_ = 1.f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Drag divider bar
// ─────────────────────────────────────────────────────────────────────────────
struct DragBar : juce::Component
{
    bool isVert_;  // true = left/right resize, false = up/down resize
    std::function<void(int)> onDelta;
    int lastScreen_ = 0;

    explicit DragBar (bool isVert) : isVert_ (isVert) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff242428));
        g.setColour (juce::Colour (0xff3A3A3E));
        if (isVert_) g.fillRect (getWidth()/2 - 1, 4, 2, getHeight() - 8);
        else         g.fillRect (4, getHeight()/2 - 1, getWidth() - 8, 2);
    }
    void mouseEnter (const juce::MouseEvent&) override
    {
        setMouseCursor (isVert_ ? juce::MouseCursor::LeftRightResizeCursor
                                : juce::MouseCursor::UpDownResizeCursor);
    }
    void mouseDown  (const juce::MouseEvent& e) override
    {
        lastScreen_ = isVert_ ? e.getScreenX() : e.getScreenY();
    }
    void mouseDrag  (const juce::MouseEvent& e) override
    {
        int cur = isVert_ ? e.getScreenX() : e.getScreenY();
        if (onDelta) onDelta (cur - lastScreen_);
        lastScreen_ = cur;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SoundBrowserContent
// ─────────────────────────────────────────────────────────────────────────────
class SoundBrowserContent : public juce::Component,
                            public juce::ListBoxModel,
                            public juce::ChangeListener,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer
{
public:
    explicit SoundBrowserContent (W2SamplerProcessor& p) : proc_ (p)
    {
        // Toolbar
        for (auto* b : { &analyseAllBtn, &rebuildBtn })
            addAndMakeVisible (b);
        analyseAllBtn.onClick = [this] { analyseAll(); };
        rebuildBtn   .onClick = [this] { triggerRebuildCorpus(); };

        // Left-panel tab row: [Browse] [Playlist] | combo | [Remove]
        addAndMakeVisible (browseTabBtn);
        addAndMakeVisible (playlistTabBtn);
        browseTabBtn  .setButtonText ("Browse");
        playlistTabBtn.setButtonText ("Playlist");
        browseTabBtn  .setClickingTogglesState (false);
        playlistTabBtn.setClickingTogglesState (false);
        browseTabBtn  .onClick = [this] { setLeftMode (LeftMode::Browse); };
        playlistTabBtn.onClick = [this] { setLeftMode (LeftMode::Playlist); };

        addAndMakeVisible (plCombo);
        plCombo.setTextWhenNothingSelected ("— no playlist —");
        plCombo.onChange = [this] { onPlaylistComboChanged(); };
        refreshPlaylistCombo();

        addAndMakeVisible (plRemoveBtn);
        plRemoveBtn.setButtonText ("Remove");
        plRemoveBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3A1A1A));
        plRemoveBtn.onClick = [this] { removeFromPlaylist(); };

        // Filesystem browser setup
        dirThread_.startThread();
        currentDir_ = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        dirContents_ = std::make_unique<juce::DirectoryContentsList> (&audioFilter_, dirThread_);
        dirContents_->addChangeListener (this);
        dirContents_->setDirectory (currentDir_, true, true);

        addAndMakeVisible (upDirBtn_);
        upDirBtn_.onClick = [this] { navigateUp(); };

        addAndMakeVisible (pathLabel_);
        pathLabel_.setFont (juce::Font (juce::FontOptions{}.withHeight (10.f)));
        pathLabel_.setColour (juce::Label::textColourId, juce::Colour (0xff8E8E93));
        pathLabel_.setText (currentDir_.getFullPathName(), juce::dontSendNotification);

        // Quick-location buttons: Home / Music / Desktop / Volumes
        for (auto* b : { &locHomeBtn_, &locMusicBtn_, &locDeskBtn_, &locVolBtn_ })
            addAndMakeVisible (b);
        locHomeBtn_ .setButtonText ("~");
        locMusicBtn_.setButtonText ("Music");
        locDeskBtn_ .setButtonText ("Desktop");
        locVolBtn_  .setButtonText ("Volumes");
        locHomeBtn_ .onClick = [this] { navigateTo (juce::File::getSpecialLocation (juce::File::userHomeDirectory)); };
        locMusicBtn_.onClick = [this] { navigateTo (juce::File::getSpecialLocation (juce::File::userMusicDirectory)); };
        locDeskBtn_ .onClick = [this] { navigateTo (juce::File::getSpecialLocation (juce::File::userDesktopDirectory)); };
        locVolBtn_  .onClick = [this]
        {
            juce::File vols ("/Volumes");
            juce::PopupMenu m;
            if (vols.isDirectory())
            {
                for (auto& f : vols.findChildFiles (juce::File::findDirectories, false))
                    m.addItem (f.getFileName(), [this, f] { navigateTo (f); });
            }
            if (m.getNumItems() == 0) m.addItem ("(no volumes found)", [] {});
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&locVolBtn_));
        };

        // File list — used for both Browse and Playlist mode (getNumRows/paint checks mode)
        addAndMakeVisible (fileList);
        fileList.setModel (this);
        fileList.setRowHeight (22);
        fileList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1A1A1E));

        updateTabAppearance();

        // Waveform
        addAndMakeVisible (waveComp);

        // Playback controls
        addAndMakeVisible (playBtn);
        addAndMakeVisible (stopBtn);
        addAndMakeVisible (levelSlider);
        playBtn.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6"));
        stopBtn.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xa0"));
        levelSlider.setRange (0.0, 1.0, 0.01); levelSlider.setValue (0.7);
        levelSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        levelSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        levelSlider.setTooltip ("Preview level");
        levelSlider.onValueChange = [this] { proc_.setPreviewLevel ((float)levelSlider.getValue()); };
        playBtn.onClick = [this]
        {
            // If buffer already decoded for the current selection, play immediately
            if (previewBufPtr_ && previewBufPtr_->getNumSamples() > 0)
            {
                proc_.startPreview (previewBufPtr_, previewSampleRate_);
                return;
            }

            // Resolve file — works for Browse (even without DB entry) and Playlist
            juce::File f = resolveSelectedFile();
            if (!f.existsAsFile())
            {
                statusLabel.setText ("No file selected", juce::dontSendNotification);
                return;
            }

            int gen = ++previewGeneration_;
            statusLabel.setText ("Loading preview\xe2\x80\xa6", juce::dontSendNotification);

            worker_.addPreviewJob (f, [this, gen]
                (std::shared_ptr<juce::AudioBuffer<float>> buf, double sr)
            {
                if (gen != previewGeneration_ || !buf) return;
                previewBufPtr_     = buf;
                previewSampleRate_ = sr;
                proc_.startPreview (buf, sr);
            });
        };
        stopBtn.onClick = [this] { proc_.stopPreview(); };

        // Analyse button + sensitivity slider
        addAndMakeVisible (analyseBtn);
        addAndMakeVisible (sensLabel);
        addAndMakeVisible (sensSlider);
        sensLabel.setText ("Sens:", juce::dontSendNotification);
        sensLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (11.f)));
        sensLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8E8E93));
        sensSlider.setRange (0.0, 1.0, 0.01); sensSlider.setValue (0.5);
        sensSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        sensSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        sensSlider.setTooltip ("Onset sensitivity — drag, then click Analyse");
        analyseBtn.onClick = [this] { analyseSelected(); };

        // Analysis settings
        addAndMakeVisible (settingsLabel);
        settingsLabel.setText ("Window:", juce::dontSendNotification);
        settingsLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (10.f)));
        settingsLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8E8E93));

        for (int i = 0; i < 3; ++i) addAndMakeVisible (winBtn[i]);
        winBtn[0].setButtonText ("512");  winBtn[0].onClick = [this] { setWindow (512); };
        winBtn[1].setButtonText ("1024"); winBtn[1].onClick = [this] { setWindow (1024); };
        winBtn[2].setButtonText ("2048"); winBtn[2].onClick = [this] { setWindow (2048); };
        winBtn[1].setToggleState (true, juce::dontSendNotification);

        for (int i = 0; i < 3; ++i) addAndMakeVisible (hopBtn[i]);
        hopBtn[0].setButtonText ("128"); hopBtn[0].onClick = [this] { setHop (128); };
        hopBtn[1].setButtonText ("256"); hopBtn[1].onClick = [this] { setHop (256); };
        hopBtn[2].setButtonText ("512"); hopBtn[2].onClick = [this] { setHop (512); };
        hopBtn[2].setToggleState (true, juce::dontSendNotification);

        addAndMakeVisible (hopLabel); addAndMakeVisible (mfccLabel);
        hopLabel .setText ("Hop:",  juce::dontSendNotification);
        mfccLabel.setText ("MFCC:", juce::dontSendNotification);
        for (auto* l : { &hopLabel, &mfccLabel })
        {
            l->setFont (juce::Font (juce::FontOptions{}.withHeight (10.f)));
            l->setColour (juce::Label::textColourId, juce::Colour (0xff8E8E93));
        }

        addAndMakeVisible (mfccSlider);
        mfccSlider.setRange (4, 13, 1); mfccSlider.setValue (13);
        mfccSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        mfccSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 24, 14);
        mfccSlider.onValueChange = [this] {
            auto s = SampleDatabase::instance().getSettings();
            s.mfccCoeff = (int)mfccSlider.getValue();
            SampleDatabase::instance().setSettings (s);
        };

        // Analysis labels (right panel)
        for (auto* l : { &keyDetLabel, &keyMetaLabel, &bpmLabel, &onsetLabel, &pitchLabel })
        {
            addAndMakeVisible (l);
            l->setFont (juce::Font (juce::FontOptions{}.withHeight (12.f)));
            l->setColour (juce::Label::textColourId, juce::Colour (0xffE0E0E5));
        }

        // Send-to-voice buttons (use ASCII to avoid encoding issues)
        for (int v = 0; v < 3; ++v)
        {
            sendBtn[v].setButtonText ("-> V" + juce::String (v + 1));
            addAndMakeVisible (sendBtn[v]);
            sendBtn[v].onClick = [this, v] { sendToVoice (v); };
        }

        // Corpus view
        addAndMakeVisible (corpusView);
        corpusView.onSelect = [this] (int fileIdx) { selectFileRow (fileIdx); };

        // ── Playlist controls below waveform ─────────────────────────────────
        addAndMakeVisible (plAddBtn);
        plAddBtn.setButtonText ("+ Add to Playlist");
        plAddBtn.onClick = [this] { addSelectedToPlaylist(); };

        addAndMakeVisible (plNewBtn);
        plNewBtn.setButtonText ("New Playlist");
        plNewBtn.onClick = [this] { createNewPlaylist(); };

        // Status label
        addAndMakeVisible (statusLabel);
        statusLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (10.f)));
        statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8E8E93));

        // Progress bar (hidden when idle)
        addAndMakeVisible (progressBar_);
        progressBar_.setVisible (false);
        progressBar_.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff2A2A2E));
        progressBar_.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff30D158));

        // Drag dividers
        addAndMakeVisible (hBar1); addAndMakeVisible (hBar2); addAndMakeVisible (vBar);
        hBar1.onDelta = [this](int dx) {
            listW_ = juce::jlimit (120, getWidth() - 500, listW_ + dx); resized();
        };
        hBar2.onDelta = [this](int dx) {
            analysisW_ = juce::jlimit (140, 350, analysisW_ - dx); resized();
        };
        vBar.onDelta = [this](int dy) {
            corpusH_ = juce::jlimit (80, getHeight() - 200, corpusH_ - dy); resized();
        };

        // Analysis worker
        worker_.onProgress = [this] { onAnalysisProgress(); };

        setSize (1000, 680);
        startTimerHz (20);
    }

    ~SoundBrowserContent() override
    {
        stopTimer();
        proc_.stopPreview();
        if (dirContents_) dirContents_->removeChangeListener (this);
        dirThread_.stopThread (2000);
    }

    //──────────────────────────────────────────────────────────────────────────
    int getNumRows() override
    {
        if (leftMode_ == LeftMode::Playlist) return currentPlaylist_.size();
        return dirContents_ ? dirContents_->getNumFiles() : 0;
    }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (leftMode_ == LeftMode::Playlist)
        {
            paintPlaylistRow (row, g, w, h, sel);
            return;
        }
        paintBrowseRow (row, g, w, h, sel);
    }

    void paintBrowseRow (int row, juce::Graphics& g, int w, int h, bool sel)
    {
        if (!dirContents_ || row >= dirContents_->getNumFiles()) return;
        juce::DirectoryContentsList::FileInfo info;
        if (!dirContents_->getFileInfo (row, info)) return;

        if (sel) g.fillAll (juce::Colour (0xff30D158).withAlpha (0.22f));
        else if (row % 2 == 0) g.fillAll (juce::Colour (0xff1E1E22));

        bool isDir = info.isDirectory;
        if (isDir)
        {
            // Blue folder indicator
            g.setColour (juce::Colour (0xff0A84FF));
            g.fillRect (0, 2, 4, h - 4);
            g.setColour (juce::Colour (0xff6E9FD4));
            g.setFont (10.f);
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x96\xb6"), 7, 0, 12, h,
                        juce::Justification::centred);
            g.setColour (juce::Colour (0xffC0C0C8));
            g.setFont (11.f);
            g.drawText (info.filename, 22, 0, w - 24, h,
                        juce::Justification::centredLeft, true);
        }
        else
        {
            juce::File f = currentDir_.getChildFile (info.filename);
            const SampleEntry* dbEntry = SampleDatabase::instance().getEntry (f);

            if (dbEntry && dbEntry->valid)
            {
                int ci = (dbEntry->cluster >= 0 && dbEntry->cluster < 8) ? dbEntry->cluster : 7;
                g.setColour (juce::Colour (kClusterCols[ci]));
            }
            else if (dbEntry)
                g.setColour (juce::Colour (0xffFFD60A).withAlpha (0.7f));
            else
                g.setColour (juce::Colour (0xff444448));
            g.fillRect (0, 2, 4, h - 4);

            g.setColour (juce::Colour (0xffE0E0E5));
            g.setFont (11.f);
            g.drawText (info.filename, 10, 0, w - 160, h,
                        juce::Justification::centredLeft, true);

            if (dbEntry && dbEntry->valid)
            {
                juce::String meta = dbEntry->keyName
                    + (dbEntry->tempo > 0.f
                       ? "  " + juce::String (dbEntry->tempo, 0) + " bpm" : "");
                g.setColour (juce::Colour (0xff6E6E73));
                g.setFont (10.f);
                g.drawText (meta, w - 158, 0, 154, h,
                            juce::Justification::centredRight, true);
            }
        }
    }

    void paintPlaylistRow (int row, juce::Graphics& g, int w, int h, bool sel)
    {
        SampleEntry e;
        bool ok = currentPlaylist_.getEntry (row, e);

        if (sel) g.fillAll (juce::Colour (0xff0A84FF).withAlpha (0.22f));
        else if (row % 2 == 0) g.fillAll (juce::Colour (0xff1E1E22));

        // Cluster colour strip
        int ci = (e.cluster >= 0 && e.cluster < 8) ? e.cluster : 7;
        g.setColour (ok ? juce::Colour (kClusterCols[ci]) : juce::Colour (0xff555558));
        g.fillRect (0, 2, 4, h - 4);

        // File path indicator — small dot if file exists on disk
        bool exists = ok && juce::File (currentPlaylist_.getFilePath (row)).existsAsFile();
        g.setColour (exists ? juce::Colour (0xff30D158) : juce::Colour (0xffFF453A));
        g.fillEllipse (7.f, (float)h * 0.5f - 3.f, 6.f, 6.f);

        // Name
        g.setColour (juce::Colour (0xffE0E0E5));
        g.setFont (11.f);
        juce::String name = ok ? currentPlaylist_.getEntryName (row) : "—";
        g.drawText (name, 18, 0, w - 170, h, juce::Justification::centredLeft, true);

        // Key + BPM (from embedded analysis)
        if (ok && e.valid)
        {
            juce::String meta = e.keyName
                                + (e.tempo > 0.f ? "  " + juce::String (e.tempo, 0) + " bpm" : "");
            g.setColour (juce::Colour (0xff6E6E73)); g.setFont (10.f);
            g.drawText (meta, w - 158, 0, 154, h, juce::Justification::centredRight, true);
        }
    }

    // Use only selectedRowsChanged — listBoxItemClicked would fire twice per click
    void listBoxItemClicked  (int, const juce::MouseEvent&) override {}

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (leftMode_ != LeftMode::Browse || !dirContents_) return;
        juce::DirectoryContentsList::FileInfo info;
        if (!dirContents_->getFileInfo (row, info)) return;
        if (info.isDirectory)
            navigateTo (currentDir_.getChildFile (info.filename));
    }
    void selectedRowsChanged (int lastRow) override
    {
        if (leftMode_ == LeftMode::Playlist)
        {
            selectedPlaylistRow_ = lastRow;
            previewBufPtr_ = nullptr;
            ++previewGeneration_;
            // Load the file from the playlist for preview / waveform
            SampleEntry e;
            if (currentPlaylist_.getEntry (lastRow, e))
            {
                proc_.stopPreview();
                previewBufPtr_     = nullptr;
                previewSampleRate_ = 44100.0;
                waveComp.clear();
                // Show waveform + analysis from embedded playlist data
                waveComp.setLoading (true);
                juce::File f = e.file;
                int gen      = ++waveLoadGeneration_;
                worker_.addWaveformJob (f, [this, gen, e]
                    (std::shared_ptr<juce::AudioBuffer<float>>, double sr, WaveThumb thumb)
                {
                    if (gen != waveLoadGeneration_) return;
                    waveLoading_ = false;
                    waveComp.setLoading (false);
                    previewSampleRate_ = sr;
                    waveComp.setBuffer (std::move (thumb), e.analysis.onsetPositions);
                    waveComp.setSliceRegion (-1.f, -1.f);
                });
                // Show analysis panel from embedded data (no SampleDatabase lookup needed)
                showPlaylistEntryAnalysis (e);
            }
        }
        else
        {
            // Browse mode — select a file from dirContents_
            if (!dirContents_ || lastRow >= dirContents_->getNumFiles()) return;
            juce::DirectoryContentsList::FileInfo info;
            if (!dirContents_->getFileInfo (lastRow, info)) return;
            if (info.isDirectory) return;  // don't load waveform for folders

            juce::File f = currentDir_.getChildFile (info.filename);
            selectedFileIdx_    = -1;  // reset DB index
            selectedBrowseFile_ = f;   // always track the actual file
            previewBufPtr_      = nullptr;
            ++previewGeneration_;

            // Check if in SampleDatabase
            const SampleEntry* dbEntry = SampleDatabase::instance().getEntry (f);
            if (dbEntry)
            {
                auto& entries = SampleDatabase::instance().getEntries();
                for (int i = 0; i < (int)entries.size(); ++i)
                    if (entries[(size_t)i].file == f) { selectedFileIdx_ = i; break; }
            }

            proc_.stopPreview();
            refreshWaveformForFile (f);
            refreshAnalysisPanelForFile (f);
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    // FileDragAndDropTarget — drag a folder onto the browser to navigate there
    //──────────────────────────────────────────────────────────────────────────
    bool isInterestedInFileDrag (const juce::StringArray&) override
    {
        return leftMode_ == LeftMode::Browse;
    }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        if (files.isEmpty()) return;
        juce::File f (files[0]);
        if (f.isDirectory())
            navigateTo (f);
        else if (f.getParentDirectory().isDirectory())
            navigateTo (f.getParentDirectory());
    }

    //──────────────────────────────────────────────────────────────────────────
    void resized() override
    {
        auto b = getLocalBounds();
        const int toolH = 34;
        const int barSz = 5;
        const int ctrlH = 28;
        const int settH = 24;

        // Toolbar
        auto top = b.removeFromTop (toolH);
        statusLabel   .setBounds (top.removeFromRight (200).reduced (4, 8));
        analyseAllBtn .setBounds (top.removeFromLeft (80).reduced (2, 5));
        rebuildBtn    .setBounds (top.removeFromLeft (108).reduced (2, 5));

        // Corpus at bottom
        corpusView.setBounds (b.removeFromBottom (corpusH_));
        vBar       .setBounds (b.removeFromBottom (barSz));

        // Left panel: tab row + list
        auto leftPanel = b.removeFromLeft (listW_);
        hBar1.setBounds (b.removeFromLeft (barSz));
        auto tabRow = leftPanel.removeFromTop (26);
        browseTabBtn  .setBounds (tabRow.removeFromLeft (58).reduced (1, 2));
        playlistTabBtn.setBounds (tabRow.removeFromLeft (62).reduced (1, 2));
        upDirBtn_     .setBounds (tabRow.removeFromLeft (24).reduced (1, 2));
        upDirBtn_     .setVisible (leftMode_ == LeftMode::Browse);
        plRemoveBtn   .setBounds (tabRow.removeFromRight (58).reduced (1, 2));
        plCombo       .setBounds (tabRow.removeFromRight (130).reduced (2, 3));
        pathLabel_    .setBounds (tabRow.reduced (2, 4));
        pathLabel_    .setVisible (leftMode_ == LeftMode::Browse);

        // Quick-location buttons row (Browse mode only)
        bool browse = (leftMode_ == LeftMode::Browse);
        if (browse)
        {
            auto locRow = leftPanel.removeFromTop (22);
            int bw = locRow.getWidth() / 4;
            locHomeBtn_ .setBounds (locRow.removeFromLeft (bw).reduced (1, 2));
            locMusicBtn_.setBounds (locRow.removeFromLeft (bw).reduced (1, 2));
            locDeskBtn_ .setBounds (locRow.removeFromLeft (bw).reduced (1, 2));
            locVolBtn_  .setBounds (locRow.reduced (1, 2));
        }
        for (auto* b2 : { &locHomeBtn_, &locMusicBtn_, &locDeskBtn_, &locVolBtn_ })
            b2->setVisible (browse);

        fileList      .setBounds (leftPanel);

        // Analysis panel right
        auto right = b.removeFromRight (analysisW_);
        hBar2 .setBounds (b.removeFromRight (barSz));
        layoutAnalysisPanel (right);

        // Center: settings row + controls bar + waveform + playlist strip
        auto settRow = b.removeFromTop (settH);
        layoutSettingsRow (settRow);
        auto ctrlRow = b.removeFromBottom (ctrlH);
        layoutControlsRow (ctrlRow);
        auto plRow = b.removeFromBottom (26);
        layoutPlaylistRow (plRow);
        waveComp.setBounds (b.removeFromTop (80));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1A1A1E));
        paintMfccBars (g);
    }

private:
    W2SamplerProcessor& proc_;

    // Layout dimensions (draggable)
    int listW_    = 280;
    int analysisW_ = 210;
    int corpusH_  = 200;

    // Left-panel mode
    enum class LeftMode { Browse, Playlist };
    LeftMode leftMode_ = LeftMode::Browse;
    int      selectedPlaylistRow_ = -1;

    // Toolbar
    juce::TextButton analyseAllBtn{ "Analyse All" };
    juce::TextButton rebuildBtn   { "Rebuild Corpus" };
    juce::Label      statusLabel;

    // Filesystem browser (Browse mode)
    juce::WildcardFileFilter                       audioFilter_  { "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg",
                                                                    "*", "Audio files" };
    juce::TimeSliceThread                          dirThread_    { "W2DirScan" };
    std::unique_ptr<juce::DirectoryContentsList>   dirContents_;
    juce::File                                     currentDir_;
    juce::TextButton                               upDirBtn_     { juce::CharPointer_UTF8 ("\xe2\x86\x91") };
    juce::Label                                    pathLabel_;

    // Quick-location buttons
    juce::TextButton locHomeBtn_, locMusicBtn_, locDeskBtn_, locVolBtn_;

    // Left-panel tab row
    juce::TextButton browseTabBtn, playlistTabBtn, plRemoveBtn;
    juce::ComboBox   plCombo;

    // Playlist controls (below waveform)
    juce::TextButton plAddBtn, plNewBtn;
    Playlist         currentPlaylist_;

    // Controls bar
    juce::TextButton analyseBtn { "Analyse" };
    juce::TextButton playBtn, stopBtn;
    juce::Slider     levelSlider, sensSlider;
    juce::Label      sensLabel;

    // Progress — shows spinning indicator while worker is busy
    double             progressValue_ = -1.0;   // -1 = indeterminate
    juce::ProgressBar  progressBar_   { progressValue_ };

    // Settings row
    juce::Label      settingsLabel, hopLabel, mfccLabel;
    juce::TextButton winBtn[3], hopBtn[3];
    juce::Slider     mfccSlider;

    // Analysis panel labels
    juce::Label keyDetLabel, keyMetaLabel, bpmLabel, onsetLabel, pitchLabel;
    juce::TextButton sendBtn[3];
    juce::Rectangle<int> mfccBounds_;

    // Waveform — uses pre-computed WaveThumb (1024 pts) so paint() is O(width), not O(samples)
    struct WaveComp : public juce::Component
    {
        WaveThumb          thumb_;
        std::vector<float> onsets_;
        float playhead_   = -1.f;
        float sliceStart_ = -1.f;
        float sliceEnd_   = -1.f;
        bool  loading_    = false;
        bool  hasData_    = false;

        /** Called from worker callback — thumbnail already built, zero message-thread cost. */
        void setBuffer (WaveThumb th, std::vector<float> o)
        {
            thumb_   = std::move (th);
            onsets_  = std::move (o);
            hasData_ = !thumb_.pts.empty();
            sliceStart_ = sliceEnd_ = -1.f;
            repaint();
        }
        /** Update onset markers only (after analysis) — no waveform reload needed. */
        void setOnsets (std::vector<float> o) { onsets_ = std::move (o); repaint(); }
        void setLoading (bool l)              { loading_ = l; repaint(); }
        void setPlayhead (float p)            { if (std::abs (p - playhead_) > 0.0005f) { playhead_ = p; repaint(); } }
        void setSliceRegion (float s, float e){ sliceStart_ = s; sliceEnd_ = e; repaint(); }
        void clear()                          { thumb_ = {}; onsets_.clear(); hasData_ = false; repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds();
            const int W = b.getWidth(), H = b.getHeight();
            g.fillAll (juce::Colour (0xff111115));
            g.setColour (juce::Colour (0xff252528)); g.drawRect (b, 1);

            if (loading_)
            {
                g.setColour (juce::Colour (0xff8E8E93)); g.setFont (11.f);
                g.drawText ("Loading\xe2\x80\xa6", b, juce::Justification::centred);
                return;
            }

            if (!hasData_)
            {
                g.setColour (juce::Colour (0xff6E6E73)); g.setFont (11.f);
                g.drawText ("Select a file", b, juce::Justification::centred);
                return;
            }

            const float cy = (float)H * 0.5f;
            const int   N  = (int)thumb_.pts.size();   // = WaveThumb::kSize (1024)

            // Slice region highlight
            if (sliceStart_ >= 0.f && sliceEnd_ > sliceStart_)
            {
                int x0 = (int)(sliceStart_ * (float)W);
                int x1 = (int)(sliceEnd_   * (float)W);
                g.setColour (juce::Colour (0xff0A84FF).withAlpha (0.12f));
                g.fillRect (x0, 0, x1 - x0, H);
            }

            // Waveform — O(width) lookup into pre-built thumbnail
            g.setColour (juce::Colour (0xff3E7A8C));
            for (int px = 0; px < W; ++px)
            {
                // Map screen pixel to thumbnail index range
                int t0 = juce::jlimit (0, N-1, (int)((float)px       / (float)W * (float)N));
                int t1 = juce::jlimit (0, N-1, (int)((float)(px + 1) / (float)W * (float)N));
                float hi = 0.f, lo = 0.f;
                for (int t = t0; t <= t1; ++t)
                {
                    hi = std::max (hi, thumb_.pts[(size_t)t].hi);
                    lo = std::min (lo, thumb_.pts[(size_t)t].lo);
                }
                g.drawVerticalLine (px, cy - hi * cy, cy - lo * cy + 1.f);
            }

            // Centre line
            g.setColour (juce::Colour (0xff303035));
            g.drawHorizontalLine ((int)cy, 0.f, (float)W);

            // Onset ticks — full height, semi-transparent orange
            g.setColour (juce::Colour (0xffFF9F0A).withAlpha (0.55f));
            for (float np : onsets_)
                g.drawVerticalLine ((int)(np * (float)W), 0.f, (float)H);

            // Slice region border
            if (sliceStart_ >= 0.f)
            {
                g.setColour (juce::Colour (0xff0A84FF).withAlpha (0.8f));
                g.drawVerticalLine ((int)(sliceStart_ * (float)W), 0.f, (float)H);
                g.drawVerticalLine ((int)(sliceEnd_   * (float)W), 0.f, (float)H);
            }

            // Playhead
            if (playhead_ >= 0.f)
            {
                g.setColour (juce::Colour (0xffFFD60A).withAlpha (0.9f));
                g.drawVerticalLine ((int)(playhead_ * (float)W), 0.f, (float)H);
            }
        }
    } waveComp;

    juce::ListBox fileList { "files", this };
    CorpusView    corpusView;
    DragBar       hBar1 { true }, hBar2 { true }, vBar { false };

    std::shared_ptr<juce::AudioBuffer<float>> previewBufPtr_;   // shared with processor — zero-copy
    double                             previewSampleRate_ = 44100.0;
    AnalysisWorker                     worker_;

    int        selectedFileIdx_    = -1;
    juce::File selectedBrowseFile_;               // set in Browse mode even if not in DB
    int  waveLoadGeneration_ = 0;  // incremented per load; lets stale callbacks self-cancel
    int  previewGeneration_  = 0;  // incremented on each new Play press; stale-cancel guard
    bool waveLoading_        = false;
    int  lastEntryCount_     = -1;   // avoids redundant corpus repaints in timer

    //──────────────────────────────────────────────────────────────────────────
    void layoutAnalysisPanel (juce::Rectangle<int> r)
    {
        r = r.reduced (5, 4);
        int x = r.getX(), y = r.getY(), w = r.getWidth();
        auto row = [&](int h) { juce::Rectangle<int> out{x, y, w, h}; y += h + 2; return out; };
        keyDetLabel .setBounds (row (14));
        keyMetaLabel.setBounds (row (14));
        bpmLabel    .setBounds (row (14));
        onsetLabel  .setBounds (row (14));
        pitchLabel  .setBounds (row (14));
        mfccBounds_ = { x, y, w, 72 }; y += 76;
        for (int v = 0; v < 3; ++v) { sendBtn[v].setBounds (x, y, w, 22); y += 25; }
    }

    void layoutSettingsRow (juce::Rectangle<int> r)
    {
        r = r.reduced (2, 2);
        settingsLabel.setBounds (r.removeFromLeft (50));
        for (int i = 0; i < 3; ++i) winBtn[i].setBounds (r.removeFromLeft (38).reduced (1, 1));
        r.removeFromLeft (8);
        hopLabel.setBounds (r.removeFromLeft (30));
        for (int i = 0; i < 3; ++i) hopBtn[i].setBounds (r.removeFromLeft (34).reduced (1, 1));
        r.removeFromLeft (8);
        mfccLabel.setBounds (r.removeFromLeft (36));
        mfccSlider.setBounds (r.removeFromLeft (80).reduced (0, 2));
    }

    void layoutControlsRow (juce::Rectangle<int> r)
    {
        r = r.reduced (2, 3);
        playBtn  .setBounds (r.removeFromLeft (28));
        stopBtn  .setBounds (r.removeFromLeft (28));
        levelSlider.setBounds (r.removeFromLeft (80).reduced (0, 2));
        r.removeFromLeft (6);
        analyseBtn.setBounds (r.removeFromLeft (64).reduced (0, 1));
        r.removeFromLeft (4);
        sensLabel.setBounds (r.removeFromLeft (32));
        sensSlider.setBounds (r.removeFromLeft (100).reduced (0, 2));
        r.removeFromLeft (4);
        progressBar_.setBounds (r.reduced (0, 4));
    }

    void layoutPlaylistRow (juce::Rectangle<int> r)
    {
        r = r.reduced (2, 3);
        plNewBtn.setBounds (r.removeFromRight (88).reduced (1, 1));
        plAddBtn.setBounds (r.reduced (0, 1));
    }

    //──────────────────────────────────────────────────────────────────────────
    void timerCallback() override
    {
        // Playhead — only repaint waveform if position actually moved
        float prog = proc_.getPreviewProgress();
        waveComp.setPlayhead (prog);

        // Progress bar + status
        int  pending = worker_.pendingCount();
        bool busy    = worker_.isBusy();
        bool anyWork = busy || pending > 0 || waveLoading_;

        progressBar_.setVisible (anyWork);
        if (anyWork) progressValue_ = -1.0;   // indeterminate spin

        juce::String status;
        if (pending > 0)   status = "Analysing: " + juce::String (pending) + " left";
        else if (busy)     status = "Analysing\xe2\x80\xa6";
        else if (waveLoading_) status = "Loading\xe2\x80\xa6";
        statusLabel.setText (status, juce::dontSendNotification);

        // Corpus: only refresh when entry count changes (new files cached/added)
        int nEntries = SampleDatabase::instance().size();
        if (nEntries != lastEntryCount_)
        {
            lastEntryCount_ = nEntries;
            corpusView.refresh (SampleDatabase::instance().getEntries(), selectedFileIdx_);
        }
    }

    void onAnalysisProgress()
    {
        fileList.updateContent();
        fileList.repaint();
        // Refresh corpus whenever analysis completes (new umap2d may have been applied)
        lastEntryCount_ = -1;  // force refresh in next timerCallback
        corpusView.refresh (SampleDatabase::instance().getEntries(), selectedFileIdx_);

        if (selectedFileIdx_ >= 0)
        {
            auto& entries = SampleDatabase::instance().getEntries();
            if (selectedFileIdx_ < (int)entries.size()
                && entries[(size_t)selectedFileIdx_].valid)
            {
                if (!waveLoading_)
                    waveComp.setOnsets (entries[(size_t)selectedFileIdx_].analysis.onsetPositions);
                refreshAnalysisPanel();
            }
        }
    }

    void selectFileRow (int row)
    {
        if (row < 0 || row == selectedFileIdx_) return;
        selectedFileIdx_  = row;
        proc_.stopPreview();
        refreshWaveform();
        refreshAnalysisPanel();
    }

    void refreshWaveform()
    {
        auto& entries = SampleDatabase::instance().getEntries();
        if (selectedFileIdx_ < 0 || selectedFileIdx_ >= (int)entries.size())
        {
            waveComp.clear(); return;
        }

        juce::File f   = entries[(size_t)selectedFileIdx_].file;
        int        idx = selectedFileIdx_;
        int        gen = ++waveLoadGeneration_;
        waveLoading_       = true;
        previewBufPtr_     = nullptr;   // clear old buffer — will load on Play press
        previewSampleRate_ = 44100.0;
        waveComp.clear();
        waveComp.setLoading (true);

        // Worker builds WaveThumb with a sequential chunk read (no large allocation).
        // buf arg is always nullptr — preview loads separately when Play is pressed.
        worker_.addWaveformJob (f, [this, idx, gen]
                                (std::shared_ptr<juce::AudioBuffer<float>> /*buf*/, double sr, WaveThumb thumb)
        {
            if (gen != waveLoadGeneration_) return;   // stale — user moved on
            waveLoading_ = false;
            waveComp.setLoading (false);
            if (idx != selectedFileIdx_) return;

            previewSampleRate_ = sr;   // sample rate for preview (loaded on Play press)

            auto& ent = SampleDatabase::instance().getEntries();
            std::vector<float> onsets;
            if (idx < (int)ent.size() && ent[(size_t)idx].valid)
                onsets = ent[(size_t)idx].analysis.onsetPositions;
            waveComp.setBuffer (std::move (thumb), std::move (onsets));
            waveComp.setSliceRegion (-1.f, -1.f);
        });
    }

    void refreshAnalysisPanel()
    {
        auto& entries = SampleDatabase::instance().getEntries();

        const SampleEntry* ep = (selectedFileIdx_ >= 0 && selectedFileIdx_ < (int)entries.size())
                                ? &entries[(size_t)selectedFileIdx_] : nullptr;

        if (!ep) {
            for (auto* l : { &keyDetLabel, &keyMetaLabel, &bpmLabel, &onsetLabel, &pitchLabel })
                l->setText ("", juce::dontSendNotification);
            return;
        }

        // Key detected
        if (ep->valid)
            keyDetLabel.setText ("Key (analysis): " + ep->keyName
                                 + "  (" + juce::String (ep->keyConf, 2) + ")",
                                 juce::dontSendNotification);
        else
            keyDetLabel.setText ("Key (analysis): —", juce::dontSendNotification);

        // Key metadata
        keyMetaLabel.setText (ep->metaKey.isNotEmpty()
                              ? "Key (metadata): " + ep->metaKey
                              : "Key (metadata): —",
                              juce::dontSendNotification);

        // BPM
        juce::String bpmStr = "BPM: ";
        if (ep->valid && ep->tempo > 0.f)    bpmStr += juce::String (ep->tempo, 1);
        else                                  bpmStr += "—";
        if (ep->metaBpm > 0.f)               bpmStr += "   meta: " + juce::String (ep->metaBpm, 1);
        bpmLabel.setText (bpmStr, juce::dontSendNotification);

        // Onset count
        if (ep->valid)
            onsetLabel.setText ("Onsets: " + juce::String ((int)ep->analysis.onsetPositions.size()),
                                juce::dontSendNotification);
        else
            onsetLabel.setText ("", juce::dontSendNotification);

        // Pitch
        if (ep->valid && ep->analysis.pitchHz > 0.f)
            pitchLabel.setText ("Pitch: " + juce::String (ep->analysis.pitchHz, 1)
                                + " Hz  (" + juce::String (ep->analysis.pitchConfidence, 2) + ")",
                                juce::dontSendNotification);
        else
            pitchLabel.setText ("", juce::dontSendNotification);

        repaint (mfccBounds_);
    }

    void paintMfccBars (juce::Graphics& g)
    {
        auto& entries = SampleDatabase::instance().getEntries();

        const float* mfccPtr = nullptr;
        if (selectedFileIdx_ >= 0 && selectedFileIdx_ < (int)entries.size()
                 && entries[(size_t)selectedFileIdx_].valid)
        {
            mfccPtr = entries[(size_t)selectedFileIdx_].analysis.mfccMean.data();
        }

        if (!mfccPtr) return;

        auto b = mfccBounds_;
        g.setColour (juce::Colour (0xff1E1E22)); g.fillRect (b);
        g.setColour (juce::Colour (0xff2E2E32)); g.drawRect (b, 1);

        float bw = (float)b.getWidth() / 13.f;
        float bh = (float)b.getHeight() - 16.f;
        float lo = mfccPtr[0], hi = mfccPtr[0];
        for (int c = 1; c < 13; ++c) { lo = std::min(lo, mfccPtr[c]); hi = std::max(hi, mfccPtr[c]); }
        float range = hi - lo; if (range < 1e-6f) range = 1.f;

        for (int c = 0; c < 13; ++c)
        {
            float norm = (mfccPtr[c] - lo) / range;
            float barH = std::max (1.f, norm * bh);
            float x    = (float)b.getX() + (float)c * bw + 1.f;
            float y    = (float)b.getBottom() - barH - 10.f;
            g.setColour (juce::Colour (0xff0A84FF).withAlpha (0.8f));
            g.fillRect (x, y, bw - 2.f, barH);
        }
        g.setColour (juce::Colour (0xff6E6E73)); g.setFont (9.f);
        g.drawText ("MFCC", b.withHeight (12), juce::Justification::topLeft);
    }

    //──────────────────────────────────────────────────────────────────────────
    void setLeftMode (LeftMode m)
    {
        leftMode_ = m;
        selectedPlaylistRow_ = -1;
        updateTabAppearance();
        fileList.updateContent();
        fileList.deselectAllRows();
        fileList.repaint();
        upDirBtn_.setVisible (m == LeftMode::Browse);
        pathLabel_.setVisible (m == LeftMode::Browse);
    }

    void updateTabAppearance()
    {
        bool pl = (leftMode_ == LeftMode::Playlist);
        browseTabBtn  .setColour (juce::TextButton::buttonColourId,
                                  pl ? juce::Colour (0xff2A2A2E) : juce::Colour (0xff30D158).withAlpha (0.3f));
        playlistTabBtn.setColour (juce::TextButton::buttonColourId,
                                  pl ? juce::Colour (0xff0A84FF).withAlpha (0.3f) : juce::Colour (0xff2A2A2E));
        plRemoveBtn.setVisible (pl);
    }

    /** Show analysis panel from a Playlist entry (no SampleDatabase lookup). */
    void showPlaylistEntryAnalysis (const SampleEntry& e)
    {
        if (!e.valid)
        {
            for (auto* l : { &keyDetLabel, &keyMetaLabel, &bpmLabel, &onsetLabel, &pitchLabel })
                l->setText ("", juce::dontSendNotification);
            return;
        }
        keyDetLabel .setText ("Key (analysis): " + e.keyName
                              + "  (" + juce::String (e.keyConf, 2) + ")",
                              juce::dontSendNotification);
        keyMetaLabel.setText (e.metaKey.isNotEmpty() ? "Key (metadata): " + e.metaKey : "Key (metadata): —",
                              juce::dontSendNotification);
        juce::String bpmStr = "BPM: " + (e.tempo > 0.f ? juce::String (e.tempo, 1) : juce::String ("—"));
        if (e.metaBpm > 0.f) bpmStr += "   meta: " + juce::String (e.metaBpm, 1);
        bpmLabel .setText (bpmStr, juce::dontSendNotification);
        onsetLabel.setText ("Onsets: " + juce::String ((int)e.analysis.onsetPositions.size()),
                            juce::dontSendNotification);
        if (e.analysis.pitchHz > 0.f)
            pitchLabel.setText ("Pitch: " + juce::String (e.analysis.pitchHz, 1)
                                + " Hz  (" + juce::String (e.analysis.pitchConfidence, 2) + ")",
                                juce::dontSendNotification);
        else
            pitchLabel.setText ("", juce::dontSendNotification);
        repaint (mfccBounds_);
    }

    void removeFromPlaylist()
    {
        if (selectedPlaylistRow_ < 0 || selectedPlaylistRow_ >= currentPlaylist_.size()) return;
        SampleEntry e;
        if (!currentPlaylist_.getEntry (selectedPlaylistRow_, e)) return;
        currentPlaylist_.removeEntry (e.hash);
        currentPlaylist_.save();
        selectedPlaylistRow_ = juce::jlimit (-1, currentPlaylist_.size() - 1,
                                             selectedPlaylistRow_ - 1);
        fileList.updateContent();
        fileList.repaint();
        if (selectedPlaylistRow_ >= 0)
            fileList.selectRow (selectedPlaylistRow_, false, true);
        else
        {
            waveComp.clear();
            for (auto* l : { &keyDetLabel, &keyMetaLabel, &bpmLabel, &onsetLabel, &pitchLabel })
                l->setText ("", juce::dontSendNotification);
        }
    }

    void analyseSelected()
    {
        juce::File f = resolveSelectedFile();
        if (!f.existsAsFile()) return;

        // If not yet in DB, register it first
        if (selectedFileIdx_ < 0)
        {
            SampleDatabase::instance().addFileToList (f);
            auto& entries = SampleDatabase::instance().getEntries();
            for (int i = 0; i < (int)entries.size(); ++i)
                if (entries[(size_t)i].file == f) { selectedFileIdx_ = i; break; }
        }

        // forceReanalysis=true: bypass JSON cache so new sensitivity applies
        worker_.addFileJob (f, (float)sensSlider.getValue(), /*forceReanalysis=*/true);
    }

    void setWindow (int size)
    {
        auto s = SampleDatabase::instance().getSettings();
        s.windowSize = size;
        SampleDatabase::instance().setSettings (s);
        for (int i = 0; i < 3; ++i)
            winBtn[i].setToggleState (winBtn[i].getButtonText().getIntValue() == size,
                                      juce::dontSendNotification);
    }

    void setHop (int size)
    {
        auto s = SampleDatabase::instance().getSettings();
        s.hopSize = size;
        SampleDatabase::instance().setSettings (s);
        for (int i = 0; i < 3; ++i)
            hopBtn[i].setToggleState (hopBtn[i].getButtonText().getIntValue() == size,
                                      juce::dontSendNotification);
    }

    void triggerRebuildCorpus()
    {
        worker_.queueRebuildCorpus();
    }

    void removeSelected()
    {
        if (selectedFileIdx_ < 0) return;
        proc_.stopPreview();
        SampleDatabase::instance().removeEntry (selectedFileIdx_);
        selectedFileIdx_  = juce::jlimit (-1, SampleDatabase::instance().size() - 1,
                                          selectedFileIdx_ - 1);
        fileList.updateContent();
        fileList.repaint();
        corpusView.refresh (SampleDatabase::instance().getEntries(), -1);
        if (selectedFileIdx_ >= 0)
        {
            fileList.selectRow (selectedFileIdx_, false, true);
            refreshWaveform();
        }
        else
        {
            waveComp.clear();
            for (auto* l : { &keyDetLabel, &keyMetaLabel, &bpmLabel, &onsetLabel, &pitchLabel })
                l->setText ("", juce::dontSendNotification);
        }
    }

    void sendToVoice (int v)
    {
        if (leftMode_ == LeftMode::Playlist)
        {
            SampleEntry e;
            if (currentPlaylist_.getEntry (selectedPlaylistRow_, e))
                proc_.loadSingleFileWithAnalysis (v, e.file, e.analysis);
            return;
        }

        // Browse mode — get file from dirContents_ if we have a dir selection
        // first try selectedFileIdx_ (if file was also added to DB)
        if (selectedFileIdx_ >= 0)
        {
            auto& entries = SampleDatabase::instance().getEntries();
            if (selectedFileIdx_ < (int)entries.size())
            {
                const auto& e = entries[(size_t)selectedFileIdx_];
                if (e.valid)
                    proc_.loadSingleFileWithAnalysis (v, e.file, e.analysis);
                else
                    proc_.loadSingleFile (v, e.file);
                return;
            }
        }

        // File not in DB — load from currently selected dirContents_ row
        if (dirContents_)
        {
            int row = fileList.getSelectedRow();
            if (row >= 0 && row < dirContents_->getNumFiles())
            {
                juce::DirectoryContentsList::FileInfo info;
                if (dirContents_->getFileInfo (row, info) && !info.isDirectory)
                {
                    juce::File f = currentDir_.getChildFile (info.filename);
                    proc_.loadSingleFile (v, f);
                }
            }
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    // Analyse All — queues AnalyseFile for every audio file in current dir (Browse)
    // or every unanalysed entry (Playlist)
    //──────────────────────────────────────────────────────────────────────────
    void analyseAll()
    {
        if (leftMode_ == LeftMode::Browse && dirContents_)
        {
            float sens = (float)sensSlider.getValue();
            for (int i = 0; i < dirContents_->getNumFiles(); ++i)
            {
                juce::DirectoryContentsList::FileInfo info;
                if (dirContents_->getFileInfo (i, info) && !info.isDirectory)
                {
                    juce::File f = currentDir_.getChildFile (info.filename);
                    SampleDatabase::instance().addFileToList (f);
                    worker_.addCheckCacheJob (f);
                    worker_.addFileJob (f, sens);
                }
            }
            fileList.updateContent();
            fileList.repaint();
        }
        else
        {
            float sens = (float)sensSlider.getValue();
            const auto& entries = SampleDatabase::instance().getEntries();
            for (const auto& e : entries)
                if (!e.valid)
                    worker_.addFileJob (e.file, sens);
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    // Playlist helpers
    //──────────────────────────────────────────────────────────────────────────
    void refreshPlaylistCombo()
    {
        juce::StringArray names = Playlist::listSaved();
        plCombo.clear (juce::dontSendNotification);
        int id = 1;
        for (const auto& n : names)
            plCombo.addItem (n, id++);

        // Re-select current playlist if it still exists
        if (currentPlaylist_.name.isNotEmpty())
        {
            int idx = names.indexOf (currentPlaylist_.name);
            if (idx >= 0) plCombo.setSelectedItemIndex (idx, juce::dontSendNotification);
        }
    }

    void onPlaylistComboChanged()
    {
        juce::String sel = plCombo.getText();
        if (sel.isEmpty()) return;
        if (currentPlaylist_.name == sel) return;

        if (currentPlaylist_.isDirty()) currentPlaylist_.save();

        Playlist pl;
        if (Playlist::load (sel, pl))
        {
            currentPlaylist_ = std::move (pl);
            // Switch to Playlist mode so user sees the contents immediately
            setLeftMode (LeftMode::Playlist);
        }
    }

    void createNewPlaylist()
    {
        // Use a simple non-modal dialog: juce::AlertWindow with a text field
        auto* dialog = new juce::AlertWindow ("New Playlist", "Enter playlist name:",
                                              juce::MessageBoxIconType::NoIcon);
        dialog->addTextEditor ("name", "", "Name:");
        dialog->addButton ("Create", 1);
        dialog->addButton ("Cancel", 0);
        dialog->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, dialog] (int result)
            {
                if (result == 1)
                {
                    juce::String name = dialog->getTextEditorContents ("name").trim();
                    if (name.isNotEmpty())
                    {
                        if (currentPlaylist_.isDirty()) currentPlaylist_.save();
                        currentPlaylist_ = Playlist();
                        currentPlaylist_.name = name;
                        currentPlaylist_.save();
                        refreshPlaylistCombo();
                        juce::StringArray names = Playlist::listSaved();
                        int idx = names.indexOf (name);
                        if (idx >= 0)
                            plCombo.setSelectedItemIndex (idx, juce::dontSendNotification);
                        setLeftMode (LeftMode::Playlist);
                    }
                }
                delete dialog;
            }), true);
    }

    void addSelectedToPlaylist()
    {
        if (currentPlaylist_.name.isEmpty()) { createNewPlaylist(); return; }

        // Get the entry to add — from browse mode only (in playlist mode button is less relevant)
        if (leftMode_ != LeftMode::Browse) return;
        auto& entries = SampleDatabase::instance().getEntries();
        if (selectedFileIdx_ < 0 || selectedFileIdx_ >= (int)entries.size()) return;
        const auto& e = entries[(size_t)selectedFileIdx_];

        if (!e.valid)
        {
            worker_.addFileJob (e.file, (float)sensSlider.getValue());
            statusLabel.setText ("Queued for analysis — add to playlist when done",
                                 juce::dontSendNotification);
            return;
        }

        currentPlaylist_.addEntry (e);
        currentPlaylist_.save();
        statusLabel.setText ("Added to \"" + currentPlaylist_.name + "\"",
                             juce::dontSendNotification);

        // Refresh playlist list if visible
        if (leftMode_ == LeftMode::Playlist)
        {
            fileList.updateContent();
            fileList.repaint();
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    // Resolve the currently selected file regardless of DB membership or mode
    //──────────────────────────────────────────────────────────────────────────
    juce::File resolveSelectedFile() const
    {
        if (leftMode_ == LeftMode::Playlist)
        {
            SampleEntry e;
            if (selectedPlaylistRow_ >= 0 && currentPlaylist_.getEntry (selectedPlaylistRow_, e))
                return e.file;
            return {};
        }
        // Browse mode: prefer DB entry (has correct file path), fall back to tracked file
        if (selectedFileIdx_ >= 0)
        {
            auto& ents = SampleDatabase::instance().getEntries();
            if (selectedFileIdx_ < (int)ents.size())
                return ents[(size_t)selectedFileIdx_].file;
        }
        return selectedBrowseFile_;
    }

    //──────────────────────────────────────────────────────────────────────────
    // Filesystem navigation
    //──────────────────────────────────────────────────────────────────────────
    void navigateTo (const juce::File& dir)
    {
        if (!dir.isDirectory()) return;
        currentDir_ = dir;
        pathLabel_.setText (currentDir_.getFullPathName(), juce::dontSendNotification);
        dirContents_->setDirectory (currentDir_, true, true);
        selectedFileIdx_ = -1;
        fileList.updateContent();
        fileList.repaint();
    }

    void navigateUp()
    {
        juce::File parent = currentDir_.getParentDirectory();
        if (parent != currentDir_) navigateTo (parent);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (leftMode_ == LeftMode::Browse)
        {
            fileList.updateContent();
            fileList.repaint();
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    // Waveform/analysis refresh by File (for Browse mode, not relying on DB index)
    //──────────────────────────────────────────────────────────────────────────
    void refreshWaveformForFile (const juce::File& f)
    {
        int gen = ++waveLoadGeneration_;
        waveLoading_ = true;
        previewBufPtr_ = nullptr;
        previewSampleRate_ = 44100.0;
        waveComp.clear();
        waveComp.setLoading (true);
        worker_.addWaveformJob (f, [this, gen, f]
            (std::shared_ptr<juce::AudioBuffer<float>>, double sr, WaveThumb thumb)
        {
            if (gen != waveLoadGeneration_) return;
            waveLoading_ = false;
            waveComp.setLoading (false);
            previewSampleRate_ = sr;
            const SampleEntry* e = SampleDatabase::instance().getEntry (f);
            std::vector<float> onsets;
            if (e && e->valid) onsets = e->analysis.onsetPositions;
            waveComp.setBuffer (std::move (thumb), std::move (onsets));
            waveComp.setSliceRegion (-1.f, -1.f);
        });
    }

    void refreshAnalysisPanelForFile (const juce::File& f)
    {
        const SampleEntry* e = SampleDatabase::instance().getEntry (f);
        if (!e)
        {
            for (auto* l : { &keyDetLabel, &keyMetaLabel, &bpmLabel, &onsetLabel, &pitchLabel })
                l->setText ("", juce::dontSendNotification);
            return;
        }
        // Sync selectedFileIdx_ so refreshAnalysisPanel works
        auto& entries = SampleDatabase::instance().getEntries();
        for (int i = 0; i < (int)entries.size(); ++i)
            if (entries[(size_t)i].file == f) { selectedFileIdx_ = i; break; }
        refreshAnalysisPanel();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundBrowserContent)
};

// ─────────────────────────────────────────────────────────────────────────────
class SoundBrowser : public juce::DocumentWindow
{
public:
    explicit SoundBrowser (W2SamplerProcessor& p)
        : juce::DocumentWindow ("W2 Sound Browser", juce::Colour (0xff1A1A1E),
                                juce::DocumentWindow::closeButton),
          content_ (p)
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&content_, true);
        setResizable (true, false);
        centreWithSize (1000, 680);
    }

    void closeButtonPressed() override { setVisible (false); }
    void openOrFocus()        { setVisible (true); toFront (true); }

private:
    SoundBrowserContent content_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundBrowser)
};
