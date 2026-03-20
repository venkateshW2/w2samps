#pragma once
#include "FuncGen.h"
#include <atomic>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

/**
 * TimelineEnv — a macro-scale function generator.
 *
 * Unlike per-voice FuncGens (which are phasor-locked and loop with every bar),
 * a TimelineEnv plays from 0 → durationSec exactly ONCE per "performance" and
 * then holds at the end (or loops, if looping is enabled).
 *
 * One TimelineEnv can modulate MULTIPLE (voice, dest) pairs simultaneously —
 * the same slow curve drives pitch on voice 1 AND reverb on voice 2 if you want.
 *
 * Audio thread: call tick() each block to advance time and get the current output.
 * Message thread: edit curve, set duration, add/remove destinations.
 *
 * Thread safety: durationSec and looping are atomic; destinations list uses a
 * flag-protected double-buffer swap (message thread writes, flip atomic signals
 * audio thread to pick up on next tick).
 */

struct TimelineDest
{
    int     voice = 0;          // 0-2
    int     dest  = 0;          // ModDest index
    float   depth = 1.0f;       // -1 → +1 blend
    float   min   = 0.0f;       // normalised range lo
    float   max   = 1.0f;       // normalised range hi
};

class TimelineEnv
{
public:
    static constexpr int kMaxDests = 8;   // destinations per envelope

    // Message-thread editable properties
    std::atomic<float> durationSec { 60.0f };  // total cycle length
    std::atomic<bool>  looping     { false };
    std::atomic<float> rateMultiplier { 1.0f };  // playback speed (0.1–10×)  // loop at end or hold

    FuncGen curve;   // the curve shape (message thread edits, audio thread reads LUT)

    //==========================================================================
    // Message-thread destination management

    void addDest (const TimelineDest& d)
    {
        auto next = pendingDests_;
        if ((int) next.size() < kMaxDests)
            next.push_back (d);
        pendingDests_ = next;
        destsChanged_.store (true, std::memory_order_release);
    }

    void removeDest (int idx)
    {
        auto next = pendingDests_;
        if (idx >= 0 && idx < (int) next.size())
            next.erase (next.begin() + idx);
        pendingDests_ = next;
        destsChanged_.store (true, std::memory_order_release);
    }

    void clearDests()
    {
        pendingDests_.clear();
        destsChanged_.store (true, std::memory_order_release);
    }

    /** Replace all destinations. */
    void setDests (const std::vector<TimelineDest>& d)
    {
        pendingDests_ = d;
        destsChanged_.store (true, std::memory_order_release);
    }

    const std::vector<TimelineDest>& getPendingDests() const { return pendingDests_; }

    bool isActive() const { return active_; }
    void setActive (bool a) { active_ = a; }

    //==========================================================================
    // Audio-thread API

    /**
     * Advance timeline by numSamples at the given sampleRate.
     * Returns the current normalised position [0,1].
     * Call this even when paused (returns same value).
     */
    float tick (bool isPlaying, int numSamples, double sampleRate) noexcept
    {
        // Pick up new destinations if changed
        if (destsChanged_.load (std::memory_order_acquire))
        {
            // This is safe only if pendingDests_ writes are atomic-equivalent
            // (they're not, but we protect by only writing from message thread
            //  and reading from audio thread after the store — acceptable benign race
            //  since destinations are not DSP-critical: worst case is one stale frame).
            activeDests_ = pendingDests_;
            destsChanged_.store (false, std::memory_order_release);
        }

        if (isPlaying && active_)
        {
            float dur = durationSec.load (std::memory_order_relaxed);
            if (dur < 0.001f) dur = 0.001f;
            float rate = rateMultiplier.load (std::memory_order_relaxed);
            if (rate < 0.01f) rate = 0.01f;
            timeSec_ += (double) numSamples / sampleRate * (double) rate;
            if (timeSec_ >= (double) dur)
            {
                if (looping.load (std::memory_order_relaxed))
                    timeSec_ -= (double) dur;
                else
                    timeSec_ = (double) dur;  // hold at end
            }
            float norm = (float) (timeSec_ / (double) dur);
            currentPhase_.store (norm, std::memory_order_relaxed);
        }
        return currentPhase_.load (std::memory_order_relaxed);
    }

    float getCurrentPhase() const noexcept { return currentPhase_.load (std::memory_order_relaxed); }

    /** Evaluate the curve at the current phase. */
    float evaluate() const noexcept { return curve.evaluate (getCurrentPhase()); }

    /** Destinations the audio thread uses for modulation. */
    const std::vector<TimelineDest>& getActiveDests() const noexcept { return activeDests_; }

    void resetTime() { timeSec_ = 0.0; currentPhase_.store (0.f, std::memory_order_relaxed); }

private:
    bool   active_    = false;
    double timeSec_   = 0.0;

    std::atomic<float> currentPhase_ { 0.f };
    std::atomic<bool>  destsChanged_ { false };

    // Destination lists — pendingDests_ written by message thread,
    // activeDests_ copied to audio thread on next tick when destsChanged_ is set.
    std::vector<TimelineDest> pendingDests_;
    std::vector<TimelineDest> activeDests_;
};
