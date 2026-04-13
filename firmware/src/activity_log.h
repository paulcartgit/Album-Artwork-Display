#pragma once
#include <Arduino.h>

#define LOG_MAX_ENTRIES 30
#define LOG_MAX_MSG     120

struct LogEntry {
    unsigned long timestamp; // millis()
    char message[LOG_MAX_MSG];
};

void activityLog(const char* msg);
void activityLogf(const char* fmt, ...);
int  activityLogGet(LogEntry* out, int maxEntries); // returns count, newest first
