#pragma once
#include <vector>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

/**
 * FuncGen — drawable function generator (Catmull-Rom spline, 512-point LUT).
 *
 * Message thread: add/move/remove control points → rebuild() bakes LUT.
 * Audio thread:   evaluate(phase [0,1]) reads the active LUT buffer.
 *
 * Thread safety via double-buffering: message thread writes to the inactive
 * buffer and flips readBuf_ with release-store; audio thread reads with
 * acquire-load. No locks needed.
 *
 * Modulation destinations — applied in VoiceChannel::processBlock:
 *   0=None  1=Pitch  2=Attack  3=Decay  4=Sustain  5=Release
 *   6=FltHz  7=FltQ  8=Drive  9=RvbMix  10=RvbSz  11=LoopMs
 */

enum class ModDest : int
{
    None = 0,
    Pitch, Attack, Decay, Sustain, Release,
    FilterFreq, FilterQ,
    Drive, ReverbMix, ReverbSize,
    LoopSizeMs,
    kCount
};

static constexpr int kNumModDests = (int) ModDest::kCount;

static const char* kModDestNames[(int) ModDest::kCount] = {
    "None",
    "Pitch", "Attack", "Decay", "Sustain", "Release",
    "Flt Hz", "Flt Q",
    "Drive", "Rvb Mix", "Rvb Sz",
    "Loop ms"
};

// FuncGen rate presets.
// Index 0-6:  sync rates (multiplier vs master phasor delta per sample)
// Index 7-13: free-running rates in Hz (independent of BPM/clock)
static constexpr float kFgRateMults[]    = { 16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f };
static constexpr float kFgFreeRateHz[]   = { 0.1f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f };
static const char*     kFgRateNames[]    = {
    // sync (indices 0-6)
    "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bar", "4 bar",
    // free Hz (indices 7-13)
    "0.1Hz", "0.25Hz", "0.5Hz", "1Hz", "2Hz", "4Hz", "8Hz"
};
static constexpr int   kNumFgRates       = 14;  // total presets
static constexpr int   kNumFgSyncRates   = 7;   // first 7 = sync
static constexpr int   kNumFgFreeRates   = 7;   // last  7 = free Hz

//==============================================================================
class FuncGen
{
public:
    static constexpr int kLutSize   = 512;
    static constexpr int kMaxPoints = 8;

    struct Point { float x = 0.0f; float y = 0.5f; };

    //==========================================================================
    // Message-thread API

    /** Add a control point (sorted by x). Rebuilds LUT. */
    void addPoint (float x, float y)
    {
        if ((int) pts_.size() >= kMaxPoints) return;
        pts_.push_back ({ clamp01 (x), clamp01 (y) });
        sortAndRebuild();
    }

    /** Move control point at index idx. Sort order may change. */
    void movePoint (int idx, float x, float y)
    {
        if (idx < 0 || idx >= (int) pts_.size()) return;
        pts_[(size_t) idx] = { clamp01 (x), clamp01 (y) };
        sortAndRebuild();
    }

    /** Remove control point at index idx. */
    void removePoint (int idx)
    {
        if (idx >= 0 && idx < (int) pts_.size())
            pts_.erase (pts_.begin() + idx);
        rebuild();
    }

    /** Replace all points. */
    void setPoints (const std::vector<Point>& pts)
    {
        pts_ = pts;
        sortAndRebuild();
    }

    void clear() { pts_.clear(); rebuild(); }

    /** Find the nearest point index within radius px (returns -1 if none). */
    int findNearest (float normX, float normY, float radiusNorm) const
    {
        float best = radiusNorm * radiusNorm;
        int   idx  = -1;
        for (int i = 0; i < (int) pts_.size(); ++i)
        {
            float dx = pts_[(size_t) i].x - normX;
            float dy = pts_[(size_t) i].y - normY;
            float d2 = dx*dx + dy*dy;
            if (d2 < best) { best = d2; idx = i; }
        }
        return idx;
    }

    const std::vector<Point>& getPoints() const { return pts_; }
    int                        pointCount() const { return (int) pts_.size(); }

    //==========================================================================
    // Audio-thread API

    /** Evaluate the curve at phase ∈ [0,1]. Lock-free. */
    float evaluate (float phase) const noexcept
    {
        int buf = readBuf_.load (std::memory_order_acquire);
        int idx = (int) (phase * (float) (kLutSize - 1));
        if (idx < 0) idx = 0;
        if (idx >= kLutSize) idx = kLutSize - 1;
        return lut_[buf][idx];
    }

    //==========================================================================
    // Serialisation helpers (called by PluginProcessor state I/O)

    int  serialisedPointCount() const { return (int) pts_.size(); }
    Point serialisedPoint (int i) const { return pts_[(size_t) i]; }

    void rebuild()
    {
        int w = 1 - readBuf_.load (std::memory_order_relaxed);
        for (int i = 0; i < kLutSize; ++i)
        {
            float t = (float) i / (float) (kLutSize - 1);
            lut_[w][i] = evalCurve (t);
        }
        readBuf_.store (w, std::memory_order_release);
    }

private:
    //==========================================================================
    static float clamp01 (float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    void sortAndRebuild()
    {
        std::sort (pts_.begin(), pts_.end(),
                   [] (const Point& a, const Point& b) { return a.x < b.x; });
        rebuild();
    }

    /** Catmull-Rom spline through the control points at global t ∈ [0,1]. */
    float evalCurve (float t) const noexcept
    {
        const int n = (int) pts_.size();
        if (n == 0) return 0.5f;
        if (n == 1) return pts_[0].y;

        // Find segment: pts[seg].x <= t < pts[seg+1].x
        int seg = 0;
        for (int i = 0; i < n - 1; ++i)
        {
            if (t <= pts_[(size_t)(i + 1)].x) { seg = i; break; }
            seg = i;
        }

        float x0 = pts_[(size_t) seg].x;
        float x1 = pts_[(size_t)(seg + 1)].x;
        float span  = x1 - x0;
        float localT = (span > 1e-5f) ? (t - x0) / span : 0.0f;
        localT = localT < 0.f ? 0.f : (localT > 1.f ? 1.f : localT);

        // Catmull-Rom: use surrounding y values (clamped at boundaries)
        auto getY = [&] (int i) -> float {
            int ci = i < 0 ? 0 : (i >= n ? n - 1 : i);
            return pts_[(size_t) ci].y;
        };
        float p0 = getY (seg - 1);
        float p1 = getY (seg);
        float p2 = getY (seg + 1);
        float p3 = getY (seg + 2);

        float t2  = localT * localT;
        float t3  = t2 * localT;
        float val = 0.5f * ((2.f*p1)
                         + (-p0 + p2)             * localT
                         + (2.f*p0 - 5.f*p1 + 4.f*p2 - p3)   * t2
                         + (-p0 + 3.f*p1 - 3.f*p2 + p3)       * t3);
        return val < 0.f ? 0.f : (val > 1.f ? 1.f : val);
    }

    std::vector<Point> pts_;
    float              lut_[2][kLutSize] = {};
    std::atomic<int>   readBuf_ { 0 };
};
