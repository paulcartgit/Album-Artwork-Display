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
    info.title  = decodeXmlEntities(extractTag(meta, "dc:title"));
    info.artist = decodeXmlEntities(extractTag(meta, "dc:creator"));
    info.album  = decodeXmlEntities(extractTag(meta, "upnp:album"));

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
    // A zero or positive code means we got an HTTP response (device is reachable).
    if (code < 0) {
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

    // Prefer roomName (clean "Office"), fall back to friendlyName
    String name = extractTag(body, "roomName");
    if (name.length() == 0) name = extractTag(body, "friendlyName");
    if (name.length() == 0) return false;

    strlcpy(nameOut, name.c_str(), nameLen);
    return true;
}

// ─── Topology-based discovery ───
// Ask a known speaker for the household zone group state via UPnP SOAP.
// The response contains ZoneGroupMember entries for every speaker on the
// network, each with Location (URL → IP) and ZoneName attributes.
// Members with Invisible="1" are secondary speakers in stereo pairs / home
// theatre setups and should be hidden from the user.

static const char ZONEGRP_ENVELOPE[] PROGMEM =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:GetZoneGroupState xmlns:u=\"urn:schemas-upnp-org:service:ZoneGroupTopology:1\">"
    "</u:GetZoneGroupState>"
    "</s:Body></s:Envelope>";

static int discoverViaTopology(const char* seedIp, SonosDevice* out, int maxDevices) {
    HTTPClient http;
    String url = String("http://") + seedIp + ":1400/ZoneGroupTopology/Control";
    http.begin(url);
    http.setTimeout(5000);
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction",
        "\"urn:schemas-upnp-org:service:ZoneGroupTopology:1#GetZoneGroupState\"");

    int code = http.POST(ZONEGRP_ENVELOPE);
    if (code != HTTP_CODE_OK) {
        http.end();
        Serial.printf("[Sonos] ZoneGroupTopology: HTTP %d\n", code);
        return 0;
    }
    String body = http.getString();
    http.end();
    Serial.printf("[Sonos] ZoneGroupState response: %d bytes\n", body.length());

    // The ZoneGroupMember data is XML-entity-encoded inside a <ZoneGroupState>
    // tag.  Decode entities so we can parse the inner XML directly.
    String decoded = decodeXmlEntities(body);

    int count = 0;
    int pos = 0;
    while (count < maxDevices) {
        int tagStart = decoded.indexOf("ZoneGroupMember ", pos);
        if (tagStart < 0) break;
        tagStart--; // back to '<'
        int tagEnd = decoded.indexOf("/>", tagStart);
        if (tagEnd < 0) tagEnd = decoded.indexOf('>', tagStart);
        if (tagEnd < 0) break;
        String tag = decoded.substring(tagStart, tagEnd + 2);
        pos = tagEnd + 2;

        // Skip invisible members (secondary in stereo pair / surround)
        if (tag.indexOf("Invisible=\"1\"") >= 0) continue;

        // Extract Location → IP
        int locStart = tag.indexOf("Location=\"");
        if (locStart < 0) continue;
        locStart += 10;
        int locEnd = tag.indexOf('"', locStart);
        if (locEnd < 0) continue;
        String loc = tag.substring(locStart, locEnd);

        int ipStart = loc.indexOf("//");
        if (ipStart < 0) continue;
        ipStart += 2;
        int ipEnd = loc.indexOf(':', ipStart);
        if (ipEnd < 0) ipEnd = loc.indexOf('/', ipStart);
        if (ipEnd <= ipStart) continue;
        String ip = loc.substring(ipStart, ipEnd);

        // Extract ZoneName
        int nameStart = tag.indexOf("ZoneName=\"");
        if (nameStart < 0) continue;
        nameStart += 10;
        int nameEnd = tag.indexOf('"', nameStart);
        if (nameEnd < 0) continue;
        String name = tag.substring(nameStart, nameEnd);

        // Deduplicate (same room name can appear for grouped speakers)
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(out[i].ip, ip.c_str()) == 0) { dup = true; break; }
        }
        if (dup) continue;

        strlcpy(out[count].name, name.c_str(), sizeof(out[count].name));
        strlcpy(out[count].ip,   ip.c_str(),   sizeof(out[count].ip));
        Serial.printf("[Sonos] Found: %s @ %s\n", out[count].name, out[count].ip);
        count++;
    }
    return count;
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

    // We only need ONE SSDP response — then use its topology API for the rest.
    char seedIp[40] = {};
    uint32_t deadline = millis() + timeoutMs;
    char buf[512];

    while (millis() < deadline && !seedIp[0]) {
        int psize = udp.parsePacket();
        if (psize > 0) {
            int len = udp.read(buf, sizeof(buf) - 1);
            buf[len] = '\0';
            if (strncmp(buf, "HTTP/1.1 200", 12) != 0) continue;

            IPAddress remoteIP = udp.remoteIP();
            snprintf(seedIp, sizeof(seedIp), "%d.%d.%d.%d",
                     remoteIP[0], remoteIP[1], remoteIP[2], remoteIP[3]);
            Serial.printf("[Sonos] SSDP seed: %s\n", seedIp);
        } else {
            delay(10);
        }
    }
    udp.stop();

    if (!seedIp[0]) {
        Serial.println("[Sonos] No SSDP response received");
        return 0;
    }

    // Use the topology API to find ALL speakers on the network
    int count = discoverViaTopology(seedIp, out, maxDevices);

    // Fallback: if topology returned nothing, use the seed speaker directly
    if (count == 0 && maxDevices > 0) {
        Serial.println("[Sonos] Topology returned 0 — falling back to seed speaker");
        char name[64] = {};
        if (sonosGetDeviceName(seedIp, name, sizeof(name)) && name[0]) {
            strlcpy(out[0].name, name, sizeof(out[0].name));
        } else {
            strlcpy(out[0].name, seedIp, sizeof(out[0].name));
        }
        strlcpy(out[0].ip, seedIp, sizeof(out[0].ip));
        count = 1;
    }

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
