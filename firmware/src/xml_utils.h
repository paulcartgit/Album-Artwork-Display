#pragma once

// ─── Minimal XML tag extraction (no external XML dependency) ───
//
// Deliberately not a parser: it handles the shapes UPnP/SOAP actually emits.
// Two things it does care about:
//   • an opening tag may carry attributes (<dc:title xmlns:dc="...">)
//   • DIDL-Lite payloads nest, so callers can scope the search to a container
//     element rather than blindly taking the first match in the document.

// Find the content of `tag` within [from, end of string). Returns "" if absent.
// `searchEnd` bounds the search so callers can restrict it to one element.
inline String extractTagIn(const String& xml, const String& tag,
                           int from, int searchEnd) {
    if (from < 0) from = 0;
    if (searchEnd < 0 || searchEnd > (int)xml.length()) searchEnd = xml.length();

    String openPrefix = "<" + tag;
    String close      = "</" + tag + ">";

    int s = from;
    while (true) {
        s = xml.indexOf(openPrefix, s);
        if (s < 0 || s >= searchEnd) return "";
        // The character after the tag name must end it, otherwise we matched a
        // longer name (<title> vs <titleSort>).
        char after = xml.charAt(s + openPrefix.length());
        if (after == '>' || after == ' ' || after == '\t' ||
            after == '\n' || after == '\r' || after == '/') break;
        s += openPrefix.length();
    }

    int contentStart = xml.indexOf('>', s);
    if (contentStart < 0 || contentStart >= searchEnd) return "";
    // Self-closing element — no content.
    if (contentStart > 0 && xml.charAt(contentStart - 1) == '/') return "";
    contentStart++;

    int e = xml.indexOf(close, contentStart);
    if (e < 0 || e > searchEnd) return "";
    return xml.substring(contentStart, e);
}

inline String extractTag(const String& xml, const String& tag) {
    return extractTagIn(xml, tag, 0, xml.length());
}

// Extract `tag` from inside the first `container` element, so that a nested
// DIDL-Lite document can't have an outer element shadow the one we want.
// Falls back to a document-wide search when the container isn't present.
inline String extractTagWithin(const String& xml, const String& container,
                               const String& tag) {
    String inner = extractTag(xml, container);
    if (inner.length() > 0) {
        String v = extractTag(inner, tag);
        if (v.length() > 0) return v;
    }
    return extractTag(xml, tag);
}

inline String decodeXmlEntities(const String& s) {
    String out = s;
    out.replace("&lt;",   "<");
    out.replace("&gt;",   ">");
    out.replace("&quot;", "\"");
    out.replace("&apos;", "'");
    // &amp; must be last: decoding it first would turn "&amp;lt;" into "<".
    out.replace("&amp;",  "&");
    return out;
}
