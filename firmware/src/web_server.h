#pragma once

void webServerInit();

// Setup-mode captive portal (runs before normal WiFi is configured)
void captivePortalInit();
void captivePortalLoop();
