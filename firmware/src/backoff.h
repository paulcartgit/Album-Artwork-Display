#pragma once
#include <stdint.h>

// ─── Vinyl identification back-off ───
//
// Pure functions so the escalation policy has exactly one definition.  The
// status endpoint used to re-implement this inline, which meant the countdown
// shown in the portal could disagree with what the device actually did.

// Retries allowed before entering cooldown.  The first cycle gets the full
// allowance; after any escalation we try once and back off again quickly.
inline int vinylMaxRetriesFor(int cooldownLevel, int firstCycleRetries) {
    return (cooldownLevel == 0) ? firstCycleRetries : 1;
}

// Cooldown grows linearly with the escalation level, capped.
inline uint32_t vinylCooldownMsFor(uint32_t baseMs, int cooldownLevel, uint32_t maxMs) {
    if (cooldownLevel < 0) cooldownLevel = 0;
    uint64_t cooldown = (uint64_t)baseMs * (1u + (uint32_t)cooldownLevel);
    if (cooldown > maxMs) return maxMs;
    return (uint32_t)cooldown;
}
