#pragma once

// Minimal Arduino compatibility shim for native (desktop) test builds.
// Only implements what our testable modules actually use.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

// ─── String class (wraps std::string) ───

class String {
    std::string _s;
public:
    String() {}
    String(const char* s) : _s(s ? s : "") {}
    String(const std::string& s) : _s(s) {}
    String(char c) : _s(1, c) {}

    size_t length() const { return _s.length(); }
    const char* c_str() const { return _s.c_str(); }
    bool isEmpty() const { return _s.empty(); }
    char charAt(size_t i) const { return _s[i]; }

    void reserve(size_t n) { _s.reserve(n); }

    int indexOf(const String& sub, size_t from = 0) const {
        auto pos = _s.find(sub._s, from);
        return pos == std::string::npos ? -1 : (int)pos;
    }

    String substring(int from, int to) const {
        if (from < 0) from = 0;
        if (to < from) return "";
        return String(_s.substr(from, to - from));
    }

    bool startsWith(const char* prefix) const {
        return _s.rfind(prefix, 0) == 0;
    }

    void replace(const char* find, const char* rep) {
        std::string f(find), r(rep);
        size_t pos = 0;
        while ((pos = _s.find(f, pos)) != std::string::npos) {
            _s.replace(pos, f.length(), r);
            pos += r.length();
        }
    }

    String operator+(const String& o) const { return String(_s + o._s); }
    String operator+(const char* o) const { return String(_s + o); }
    String& operator+=(const String& o) { _s += o._s; return *this; }
    String& operator+=(const char* o) { _s += o; return *this; }
    String& operator+=(char c) { _s += c; return *this; }

    bool operator==(const String& o) const { return _s == o._s; }
    bool operator==(const char* o) const { return _s == o; }
    bool operator!=(const char* o) const { return _s != o; }

    friend String operator+(const char* a, const String& b) {
        return String(std::string(a) + b._s);
    }
};

// ─── Serial stub ───

struct SerialClass {
    template<typename... Args> void println(Args...) {}
    template<typename... Args> void print(Args...) {}
    void printf(const char* fmt, ...) {}
};

// Do NOT define Serial here — each .cpp that needs it defines its own via NATIVE_TEST guard.

// ─── Macros ───

#define PROGMEM
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
