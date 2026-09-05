#pragma once

// ─── PMIC (AXP2101) ───
//
// Bring up the power rails. Must run before the display and SD card.
//
// Returns false when the chip does not answer — which it currently does not on
// this board. Everything works regardless, because the AXP2101 comes out of
// reset with the rails this board needs already enabled, so the configuration
// below has never actually been applied. Worth knowing before trusting it to
// set a rail voltage that matters.
//
// Battery reporting was implemented here and removed again: this frame runs on
// mains, so it was reporting a state nothing depended on. `git log power.h`
// has it if a battery-powered build ever needs it.
bool powerBegin();
