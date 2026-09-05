#pragma once
#include <stdint.h>
#include <stddef.h>

// ─── Album-art history policy ───
//
// The decisions that had bugs, extracted so they can be tested without an SD
// card: which entry to evict, and what timestamp a new entry should carry.

struct HistoryEntryMeta {
    uint32_t ts;      // epoch seconds (or a legacy millis()-derived value)
    bool     pinned;
};

// Any timestamp at or above this is a real wall-clock time.  Values below it
// were written by firmware that stored millis(), and are treated as older than
// anything with a real clock reading — which they are.
static const uint32_t HISTORY_EPOCH_PLAUSIBLE = 1600000000UL; // 2020-09-13

// Index of the entry to evict, or -1 when every entry is pinned.
inline int historyPruneIndex(const HistoryEntryMeta* entries, int count) {
    int oldest = -1;
    uint32_t oldestTs = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        if (entries[i].pinned) continue;
        if (entries[i].ts < oldestTs || oldest < 0) {
            oldestTs = entries[i].ts;
            oldest = i;
        }
    }
    return oldest;
}

// Timestamp for a newly saved entry.
//
// Storing millis() here was the bug: it restarts at zero on every boot, so
// after a power cycle every new entry looked older than everything already on
// the card and the pruner deleted the newest artwork first.  When the wall
// clock isn't available yet we stay monotonic instead, sitting just above the
// newest entry we already hold.
inline uint32_t historyNextTimestamp(uint32_t wallClockSeconds,
                                     const HistoryEntryMeta* entries, int count) {
    if (wallClockSeconds >= HISTORY_EPOCH_PLAUSIBLE) return wallClockSeconds;

    uint32_t highest = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].ts > highest) highest = entries[i].ts;
    }
    return highest + 1;
}
