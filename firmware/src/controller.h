#pragma once

// Boot sequence: hardware, SD, display, WiFi, settings, web server.
void controllerSetup();

// One pass of the state machine.  Called from loop().
void controllerLoop();

// True while the display is intentionally paused overnight.
bool inQuietHours();
