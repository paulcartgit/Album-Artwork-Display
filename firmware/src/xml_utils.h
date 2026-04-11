#pragma once

// ─── Simple XML tag extraction (no dependency on external XML library) ───

inline String extractTag(const String& xml, const String& tag) {
    String open  = "<" + tag + ">";
    String close = "</" + tag + ">";
    int s = xml.indexOf(open);
    if (s < 0) return "";
    s += open.length();
    int e = xml.indexOf(close, s);
    if (e < 0) return "";
    return xml.substring(s, e);
}

inline String decodeXmlEntities(const String& s) {
    String out = s;
    out.replace("&lt;",   "<");
    out.replace("&gt;",   ">");
    out.replace("&amp;",  "&");
    out.replace("&quot;", "\"");
    out.replace("&apos;", "'");
    return out;
}
