#pragma once
#include <ArduinoJson.h>
#include "state.h"

// mDNS discovery of B&O products (/discover) and Beoremote Halo
// (/discover-halo). Discovery only — connections still use the IP.

void collectService(const char* service, const char* nameKey,
                    const char* serialKey, int serialStart, int serialLen,
                    const char* platformTag, JsonArray& arr, String& seenIPs);
void handleDiscover();
void handleDiscoverHalo();
void handleDiscoverSpeakers();
void checkProductRecovery();
