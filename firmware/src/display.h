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

// True while a refresh is in flight.  Only meaningful when called from a task
// *other* than the one driving the display (e.g. the async web server), since
// the calling task is by definition blocked for the duration otherwise.
bool displayIsBusy();
