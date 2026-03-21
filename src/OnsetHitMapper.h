#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cstdint>

/**
 * OnsetHitMapper — per-voice table mapping euclidean hit index → onset index.
 *
 * map[hitIdx % kSlots] == -1   →  auto (OnsetSeq/Rnd default behaviour)
 * map[hitIdx % kSlots] == n≥0  →  always play onset n on that euclidean hit
 *
 * Threading:
 *   Audio thread reads via get()   — ScopedTryLock, falls back to -1 if contended.
 *   Message thread writes via set() — ScopedLock, always consistent.
 *   Message thread reads via peek() — ScopedLock, for UI display.
 */
struct OnsetHitMapper
{
    static constexpr int kSlots = 32;

    OnsetHitMapper() noexcept
    {
        std::fill (std::begin (map), std::end (map), (int8_t) -1);
    }

    /** Audio thread: non-blocking read. Returns -1 (auto) if lock contended. */
    int8_t get (int hitIdx) const noexcept
    {
        const juce::ScopedTryLock sl (lock);
        return sl.isLocked() ? map[(size_t)(hitIdx % kSlots)] : (int8_t) -1;
    }

    /** Message thread: write one slot. onsetIdx = -1 clears to auto. */
    void set (int hitIdx, int8_t onsetIdx)
    {
        const juce::ScopedLock sl (lock);
        map[(size_t)(hitIdx % kSlots)] = onsetIdx;
    }

    /** Message thread: read one slot for UI display. */
    int8_t peek (int hitIdx) const
    {
        const juce::ScopedLock sl (lock);
        return map[(size_t)(hitIdx % kSlots)];
    }

    /** Message thread: clear all slots to auto (-1). */
    void clear()
    {
        const juce::ScopedLock sl (lock);
        std::fill (std::begin (map), std::end (map), (int8_t) -1);
    }

    mutable juce::CriticalSection lock;
    int8_t                        map[kSlots];
};
