#include "sonos_client.h"
#include "xml_utils.h"
#include <HTTPClient.h>
#include <WiFiUDP.h>

static const char GETPOS_ENVELOPE[] PROGMEM =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "</u:GetPositionInfo>"
    "</s:Body></s:Envelope>";

static const char GETTRANS_ENVELOPE[] PROGMEM =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "</u:GetTransportInfo>"
    "</s:Body></s:Envelope>";

// ─── Public API ───

bool sonosGetTrackInfo(const char* sonosIp, SonosTrackInfo& info) {
    info = SonosTrackInfo();

    HTTPClient http;
    String url = String("http://") + sonosIp + ":1400/MediaRenderer/AVTransport/Control";
    http.begin(url);
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction",
        "\"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo\"");

    int code = http.POST(GETPOS_ENVELOPE);
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Sonos] GetPositionInfo HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Line-In detection
    String trackUri = extractTag(body, "TrackURI");
    info.isLineIn = trackUri.startsWith("x-rincon-stream:");

    // Parse DIDL-Lite metadata (entity-encoded XML inside TrackMetaData)
    String metaRaw = extractTag(body, "TrackMetaData");
    if (metaRaw.isEmpty()) return true; // valid but no track

    String meta = decodeXmlEntities(metaRaw);
    info.title  = extractTag(meta, "dc:title");
    info.artist = extractTag(meta, "dc:creator");
    info.album  = extractTag(meta, "upnp:album");

    String artPath = decodeXmlEntities(extractTag(meta, "upnp:albumArtURI"));
    if (artPath.length() > 0) {
        if (artPath.startsWith("http")) {
            info.artUrl = artPath;
        } else {
            info.artUrl = String("http://") + sonosIp + ":1400" + artPath;
        }
    }

    Serial.printf("[Sonos] %s — %s\n", info.artist.c_str(), info.title.c_str());
    return true;
}

bool sonosIsPlaying(const char* sonosIp, bool* reachable) {
    if (reachable) *reachable = false;

    HTTPClient http;
    String url = String("http://") + sonosIp + ":1400/MediaRenderer/AVTransport/Control";
    http.begin(url);
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction",
        "\"urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo\"");

    int code = http.POST(GETTRANS_ENVELOPE);

    // Negative codes are transport/connection errors — the device is unreachable.
    // Non-negative codes mean we got an HTTP response (device is reachable).
    if (code <= 0) {
        http.end();
        return false; // reachable stays false
    }
    if (reachable) *reachable = true;

    if (code != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    String state = extractTag(body, "CurrentTransportState");
    return state == "PLAYING";
}

// ─── UPnP SSDP Discovery ───

bool sonosGetDeviceName(const char* ip, char* nameOut, size_t nameLen) {
    HTTPClient http;
    String url = String("http://") + ip + ":1400/xml/device_description.xml";
    http.begin(url);
    http.setTimeout(2000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    String name = extractTag(body, "friendlyName");
    if (name.length() == 0) return false;

    strlcpy(nameOut, name.c_str(), nameLen);
    return true;
}

int sonosDiscover(SonosDevice* out, int maxDevices, uint32_t timeoutMs) {
    // Sonos-specific SSDP search target
    static const char MSEARCH[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
        "\r\n";

    WiFiUDP udp;
    if (!udp.begin(1901)) {
        Serial.println("[Sonos] Discovery: UDP bind failed");
        return 0;
    }

    IPAddress mcast(239, 255, 255, 250);
    udp.beginPacket(mcast, 1900);
    udp.write((const uint8_t*)MSEARCH, strlen(MSEARCH));
    udp.endPacket();
    Serial.println("[Sonos] SSDP M-SEARCH sent");

    int count = 0;
    uint32_t deadline = millis() + timeoutMs;
    char buf[512];

    while (millis() < deadline && count < maxDevices) {
        int psize = udp.parsePacket();
        if (psize > 0) {
            int len = udp.read(buf, sizeof(buf) - 1);
            buf[len] = '\0';

            // Only process 200 OK responses to our M-SEARCH
            if (strncmp(buf, "HTTP/1.1 200", 12) != 0) continue;

            // Use the sender's IP — no need to parse the LOCATION header
            IPAddress remoteIP = udp.remoteIP();
            char ip[40];
            snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
                     remoteIP[0], remoteIP[1], remoteIP[2], remoteIP[3]);

            // Deduplicate
            bool dup = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(out[i].ip, ip) == 0) { dup = true; break; }
            }
            if (dup) continue;

            // Fetch the room name from the device description
            char name[64] = {};
            if (sonosGetDeviceName(ip, name, sizeof(name)) && name[0] != '\0') {
                strlcpy(out[count].name, name, sizeof(out[count].name));
                strlcpy(out[count].ip,   ip,   sizeof(out[count].ip));
                Serial.printf("[Sonos] Found: %s @ %s\n", out[count].name, out[count].ip);
                count++;
            }
        } else {
            delay(10);
        }
    }

    udp.stop();
    Serial.printf("[Sonos] Discovery complete: %d speaker(s)\n", count);
    return count;
}

bool sonosResolveByName(const char* name, char* ipOut, size_t ipLen) {
    SonosDevice devices[16];
    int count = sonosDiscover(devices, 16, 3000);
    for (int i = 0; i < count; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            strlcpy(ipOut, devices[i].ip, ipLen);
            return true;
        }
    }
    return false;
}
