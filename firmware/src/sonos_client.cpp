#include "sonos_client.h"
#include "xml_utils.h"
#include <HTTPClient.h>
#include <WiFiUDP.h>
#include <ArduinoJson.h>

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

bool sonosIsPlaying(const char* sonosIp) {
    HTTPClient http;
    String url = String("http://") + sonosIp + ":1400/MediaRenderer/AVTransport/Control";
    http.begin(url);
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction",
        "\"urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo\"");

    int code = http.POST(GETTRANS_ENVELOPE);
    if (code != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    String state = extractTag(body, "CurrentTransportState");
    return state == "PLAYING";
}

// ─── SSDP Discovery ───

String sonosDiscover(int timeoutMs) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    WiFiUDP udp;
    if (!udp.begin(54321)) {
        Serial.println("[Sonos] Discovery: UDP bind failed");
        String out; serializeJson(doc, out); return out;
    }

    static const char MSEARCH[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
        "\r\n";

    udp.beginPacket("239.255.255.250", 1900);
    udp.write((const uint8_t*)MSEARCH, strlen(MSEARCH));
    udp.endPacket();
    Serial.println("[Sonos] SSDP M-SEARCH sent");

    static const int MAX_DEVICES = 8;
    char foundIPs[MAX_DEVICES][16];
    int foundCount = 0;

    unsigned long deadline = millis() + (unsigned long)timeoutMs;
    while (millis() < deadline && foundCount < MAX_DEVICES) {
        int sz = udp.parsePacket();
        if (sz > 0) {
            char buf[512];
            int len = udp.read(buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';
                String resp(buf);
                String lower = resp;
                lower.toLowerCase();
                int locIdx = lower.indexOf("location:");
                if (locIdx >= 0) {
                    int s = locIdx + 9;
                    while (s < (int)resp.length() && resp[s] == ' ') s++;
                    int e = resp.indexOf('\n', s);
                    if (e < 0) e = resp.length();
                    String loc = resp.substring(s, e);
                    loc.trim();
                    // Extract IP from http://IP:PORT/...
                    int ipS = loc.startsWith("http://") ? 7 :
                               (loc.startsWith("https://") ? 8 : 0);
                    int colon = loc.indexOf(':', ipS);
                    int slash  = loc.indexOf('/', ipS);
                    int ipE = (colon > ipS && (slash < 0 || colon < slash)) ? colon :
                               (slash > ipS ? slash : (int)loc.length());
                    String ip = loc.substring(ipS, ipE);
                    if (ip.length() > 0 && ip.length() < 16) {
                        bool dup = false;
                        for (int i = 0; i < foundCount; i++) {
                            if (strcmp(foundIPs[i], ip.c_str()) == 0) { dup = true; break; }
                        }
                        if (!dup) {
                            strlcpy(foundIPs[foundCount++], ip.c_str(), 16);
                            Serial.printf("[Sonos] Discovered: %s\n", ip.c_str());
                        }
                    }
                }
            }
        }
        delay(10);
    }
    udp.stop();

    // Resolve friendly names from device description
    for (int i = 0; i < foundCount; i++) {
        String ip   = String(foundIPs[i]);
        String name = ip;
        HTTPClient http;
        http.begin("http://" + ip + ":1400/xml/device_description.xml");
        http.setTimeout(2000);
        if (http.GET() == HTTP_CODE_OK) {
            String fn = extractTag(http.getString(), "friendlyName");
            if (fn.length() > 0) name = fn;
        }
        http.end();
        JsonObject obj = arr.add<JsonObject>();
        obj["ip"]   = ip;
        obj["name"] = name;
    }

    String out; serializeJson(doc, out); return out;
}
