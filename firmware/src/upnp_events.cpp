#include "upnp_events.h"
#include <WiFi.h>

static WiFiServer* s_server = nullptr;
static uint16_t s_port = 0;

void upnpEventsBegin(uint16_t port) {
    if (s_server) return;
    s_port = port;
    s_server = new WiFiServer(port);
    s_server->begin();
    s_server->setNoDelay(true);
    Serial.printf("[UPnP] Event listener on port %u\n", port);
}

uint16_t upnpEventsPort() { return s_port; }

bool upnpEventsPoll() {
    if (!s_server) return false;
    bool got = false;

    // Drain everything waiting: a burst of events should cost one poll, not one
    // poll each.
    while (true) {
        WiFiClient c = s_server->available();
        if (!c) break;

        // Read and discard the request, bounded in both bytes and time so a
        // half-open connection cannot stall the state machine.
        unsigned long deadline = millis() + 400;
        size_t seen = 0;
        while (c.connected() && millis() < deadline && seen < 8192) {
            int n = c.available();
            if (n <= 0) { delay(1); continue; }
            while (n-- > 0 && seen < 8192) { c.read(); seen++; }
            if (!c.available()) break;   // whole request drained
        }

        c.print(F("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
        c.flush();
        c.stop();
        got = true;
    }
    return got;
}
