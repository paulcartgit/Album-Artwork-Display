#pragma once
#include <Arduino.h>

// ─── UPnP event receiver ───
//
// Sonos delivers GENA events with the HTTP method NOTIFY, which
// ESPAsyncWebServer does not parse — it recognises only GET, POST, DELETE,
// PUT, PATCH, HEAD and OPTIONS, so the callback would be rejected. Rather than
// patch a dependency, run a small raw listener on its own port.
//
// The body is deliberately not parsed. An event is used only as a "something
// changed" trigger; the existing poll remains the source of truth, so a missed
// or malformed event costs nothing worse than a slightly later update.

void upnpEventsBegin(uint16_t port);

// Non-blocking. Accepts any waiting connection, answers 200, and reports
// whether an event arrived since the last call.
bool upnpEventsPoll();

uint16_t upnpEventsPort();
