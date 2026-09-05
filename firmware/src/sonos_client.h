#pragma once
#include <Arduino.h>

struct SonosTrackInfo {
    String artist;
    String title;
    String album;
    String artUrl;
    bool isLineIn;
};

// A discovered Sonos speaker (room name + current IP).
struct SonosDevice {
    char name[64];
    char ip[40];
};

bool sonosGetTrackInfo(const char* sonosIp, SonosTrackInfo& info);

// Returns true if the speaker is currently PLAYING.
// reachable (optional out-param) is set to true whenever the device responds
// to the HTTP request (even if stopped), and false on connection error only.
// This lets the caller distinguish "stopped" from "unreachable / IP changed".
bool sonosIsPlaying(const char* sonosIp, bool* reachable = nullptr);

// Discover all Sonos speakers on the local network via UPnP SSDP.
// Returns the number of devices found (≤ maxDevices).
// timeoutMs controls how long to listen for responses (default 2500 ms).
int sonosDiscover(SonosDevice* out, int maxDevices, uint32_t timeoutMs = 2500);

// Find the current IP of a speaker by its room name.
// Runs a full discovery scan. Returns true and fills ipOut on success.
bool sonosResolveByName(const char* name, char* ipOut, size_t ipLen);

// Find the IP of the speaker coordinating the group that `roomName` belongs to.
//
// When speakers are grouped, only the coordinator carries the group's transport
// state — a member reports its own, which is not what is playing. Polling the
// member therefore gives wrong or empty metadata in exactly the situation Sonos
// exists for. Falls back to seedIp when the speaker is ungrouped or the
// topology cannot be read.
bool sonosResolveCoordinator(const char* seedIp, const char* roomName,
                             char* ipOut, size_t ipLen);

// ─── UPnP eventing (GENA) ───
// Sonos will push a NOTIFY the moment transport state changes, which removes
// the latency of polling. Used purely as a "something changed" trigger: the
// existing poll remains the source of truth, so a missed or malformed event
// costs nothing beyond a slightly later update.
bool sonosSubscribe(const char* ip, const char* callbackUrl,
                    char* sidOut, size_t sidLen, uint32_t* timeoutSecOut);
bool sonosRenewSubscription(const char* ip, const char* sid, uint32_t* timeoutSecOut);
void sonosUnsubscribe(const char* ip, const char* sid);

// Fetch the friendly (room) name of a speaker whose IP is already known.
// Returns true and fills nameOut on success.
bool sonosGetDeviceName(const char* ip, char* nameOut, size_t nameLen);
