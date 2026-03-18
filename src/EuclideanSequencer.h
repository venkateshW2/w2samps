#pragma once
#include <vector>
#include <algorithm>  // std::rotate, std::min, std::max, std::clamp

/**
 * Euclidean (Bjorklund) rhythm generator.
 *
 * A Euclidean rhythm distributes k hits across n steps as evenly as possible.
 * This maps to many traditional world music patterns — see ARCHITECTURE.md §8.
 *
 * Usage:
 *   EuclideanSequencer seq;
 *   seq.set(16, 5, 0);    // 16 steps, 5 hits, no rotation
 *   bool hit = seq.tick(); // advance one step; returns true on a hit
 *
 * IMPORTANT: set() does NOT reset the playhead. Call reset() explicitly if
 * you want to restart from step 0 (e.g. on transport rewind).
 */
class EuclideanSequencer
{
public:
    EuclideanSequencer (int steps = 16, int hits = 4, int rotation = 0)
    {
        set (steps, hits, rotation);
    }

    /**
     * Rebuild the pattern from new parameters.
     * Safe to call from the audio thread — does NOT allocate memory if
     * steps count hasn't changed (pattern_ vector is reused).
     * Does NOT move the playhead — the sequence keeps playing from where it was.
     */
    void set (int steps, int hits, int rotation)
    {
        steps_    = std::max (1, steps);
        hits_     = std::clamp (hits, 0, steps_);
        rotation_ = rotation;
        // NOTE: we do NOT reset currentStep_ here.
        // Resetting it every call was a bug: the audio thread calls set() ~100x/sec
        // (once per processBlock) so the sequencer would never advance past step 0.
        // currentStep_ is only reset by reset() or prepareToPlay().
        buildPattern();
    }

    /** Return the total number of steps in the pattern. */
    int getSteps() const { return steps_; }

    /** Return the number of active hits in the pattern. */
    int getHits()  const { return hits_; }

    /** Return which step will fire on the NEXT tick() call (0-indexed). */
    int getStep()  const { return currentStep_; }

    /**
     * Advance one step and return whether this step is a hit.
     * Called from the audio thread once per sequencer step.
     * Wraps automatically at the end of the pattern.
     */
    bool tick()
    {
        bool hit     = pattern_[(size_t) currentStep_];
        currentStep_ = (currentStep_ + 1) % steps_;
        return hit;
    }

    /** Peek at any step without moving the playhead. Useful for UI drawing. */
    bool getStepValue (int step) const
    {
        return pattern_[(size_t)(step % steps_)];
    }

    /** Reset playhead to step 0. Call this on transport rewind / plugin init. */
    void reset() { currentStep_ = 0; }

    /** Return the full boolean pattern array (read-only). Used by the UI. */
    const std::vector<bool>& getPattern() const { return pattern_; }

private:
    /**
     * The Bjorklund algorithm.
     *
     * We build two groups of sub-sequences:
     *   groups    = k copies of [1]       (the hits)
     *   remainder = (n-k) copies of [0]   (the rests)
     *
     * Each round: append one remainder to each group (until remainder runs out
     * or has only 1 element left). The leftover becomes the new remainder.
     * Repeat until remainder has ≤ 1 element. Flatten to get the pattern.
     * Then rotate left by `rotation_` steps to shift the downbeat.
     */
    void buildPattern()
    {
        pattern_.assign ((size_t) steps_, false);
        if (hits_ == 0) return;  // all rests — nothing to do

        // Initialise: each hit and each rest starts as its own sub-sequence
        std::vector<std::vector<bool>> groups, remainder;
        for (int i = 0; i < hits_;           ++i) groups.push_back    ({true});
        for (int i = 0; i < steps_ - hits_;  ++i) remainder.push_back ({false});

        // Iteratively merge remainder elements onto groups
        while (remainder.size() > 1)
        {
            std::vector<std::vector<bool>> newGroups;
            int distribute = (int) std::min (groups.size(), remainder.size());

            // Append one remainder sub-sequence to each group
            for (int i = 0; i < distribute; ++i)
            {
                auto g = groups[(size_t) i];
                g.insert (g.end(), remainder[(size_t) i].begin(),
                                   remainder[(size_t) i].end());
                newGroups.push_back (g);
            }

            // Whatever is left over (from whichever list was longer) becomes
            // the new remainder for the next round
            if (groups.size() > remainder.size())
            {
                // More groups than remainders — leftover groups become remainder
                remainder = std::vector<std::vector<bool>> (
                    groups.begin() + distribute, groups.end());
            }
            else if (remainder.size() > groups.size())
            {
                // More remainders than groups — leftover remainders carry over
                remainder = std::vector<std::vector<bool>> (
                    remainder.begin() + distribute, remainder.end());
            }
            else
            {
                // Equal sizes — nothing left over, algorithm terminates next loop
                remainder.clear();
            }

            groups = std::move (newGroups);
        }

        // Flatten: write all sub-sequences into pattern_ in order
        int idx = 0;
        for (auto& g : groups)    for (bool v : g) pattern_[(size_t)(idx++)] = v;
        for (auto& r : remainder) for (bool v : r) pattern_[(size_t)(idx++)] = v;

        // Apply rotation: shift the pattern left so a different step is the "1"
        int rot = ((rotation_ % steps_) + steps_) % steps_;
        std::rotate (pattern_.begin(), pattern_.begin() + rot, pattern_.end());
    }

    int steps_       = 16;  // total number of steps in the pattern
    int hits_        = 4;   // number of active (true) steps
    int rotation_    = 0;   // how many steps to shift the pattern left
    int currentStep_ = 0;   // which step fires on the next tick() — the playhead

    std::vector<bool> pattern_;  // the computed boolean pattern, length == steps_
};
