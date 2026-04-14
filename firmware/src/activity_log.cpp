#include "activity_log.h"
#include <cstdarg>
#include <cstring>

static LogEntry g_log[LOG_MAX_ENTRIES];
static int g_logHead = 0;  // next write position
static int g_logCount = 0;

void activityLog(const char* msg) {
    g_log[g_logHead].timestamp = millis();
    strlcpy(g_log[g_logHead].message, msg, LOG_MAX_MSG);
    Serial.printf("[Activity] %s\n", msg);
    g_logHead = (g_logHead + 1) % LOG_MAX_ENTRIES;
    if (g_logCount < LOG_MAX_ENTRIES) g_logCount++;
}

void activityLogf(const char* fmt, ...) {
    char buf[LOG_MAX_MSG];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    activityLog(buf);
}

int activityLogGet(LogEntry* out, int maxEntries) {
    int count = (maxEntries < g_logCount) ? maxEntries : g_logCount;
    for (int i = 0; i < count; i++) {
        // newest first
        int idx = (g_logHead - 1 - i + LOG_MAX_ENTRIES) % LOG_MAX_ENTRIES;
        out[i] = g_log[idx];
    }
    return count;
}
