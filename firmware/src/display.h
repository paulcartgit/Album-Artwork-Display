#pragma once
#include <cstdint>

bool displayInit();

// Renders a packed 4bpp buffer and blocks until the panel has finished its
// ~15 s refresh, yielding throughout so other FreeRTOS tasks (WiFi, the async
// web server) keep running.  There is no non-blocking variant: GxEPD2 drives
// the refresh synchronously and the panel cannot accept new data mid-cycle.
void displayShowImage(const uint8_t* packedBuffer);

void displayShowMessage(const char* msg);
void displayClear();

// Total full refreshes since the counter was last reset. Spectra 6 panels have
// a finite refresh life, so this is worth watching.
uint32_t displayRefreshCount();
void displaySetRefreshCount(uint32_t n);

// millis() of the last completed refresh, 0 if none yet this boot.
unsigned long displayLastRefreshMs();

// The packed 4bpp frame currently on the panel, or nullptr before the first
// image. Kept so the portal can show what is actually displayed rather than
// the source artwork, which says nothing about how it rendered.
const uint8_t* displayCurrentFrame();

// True while a refresh is in flight.  Only meaningful when called from a task
// *other* than the one driving the display (e.g. the async web server), since
// the calling task is by definition blocked for the duration otherwise.
bool displayIsBusy();
