#include "sonos_client.h"
#include "xml_utils.h"
#include <HTTPClient.h>

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
