#pragma once
#include "state.h"

// Shared transport surface. Each platform implements its half; callers
// dispatch through these without caring which platform is active.

inline bool productConnected() {
    if (productIP.length() == 0) return false;
    return (platform == PLATFORM_MOZART) ? wsClient.available() : sseClient.connected();
}

// ASE (transport_ase.cpp): SSE stream + BeoZone REST
void forceSource();
void aseBeolink(bool expand);
void aseAdjustVolume(int delta);
void connectToSSE();
void checkSSEConnection();
void processSSE(String message);
void readSSE();

// Mozart (transport_moz.cpp): dual websockets + /api/v1 REST
void handleHttpResponse(const String& endpoint, const String& response);
bool sendHttpRequest(const String& endpoint, const String& method = "GET", const String& payload = "");
void mozartAdjustVolume(int delta);
void mozartBeolink(bool expand);
void checkWebSocketConnection();
void processWebSocketMessage(const String& message);
void processRemoteWebSocketMessage(const String& message);

// ── Playback speaker (Beolink expand) ──────────────────────────────
// Both platforms send the request to the linked product — the experience
// leader — naming the target speaker by JID.
// Called when the product confirms the trigger source is active — expanding
// before that silently does nothing, because there is no experience to
// expand yet. Idempotent: the product may confirm more than once (Mozart
// reports via both the websocket and the state poll).
inline void expandToPlaybackSpeaker() {
    if (playbackJid.length() == 0) return;   // no speaker selected
    if (speakerExpanded) return;             // already joined
    speakerExpanded = true;
    // Deferred by expandDelayMs — see config.h. Scheduled rather than
    // delay()ed so the loop keeps serving the UI and the serial protocol.
    expandDueAt = millis() + expandDelayMs;
    Serial.println("Secondary speaker: expand scheduled in " + String(expandDelayMs) + " ms");
}

// Called every loop from main.cpp.
inline void checkPendingExpand() {
    if (expandDueAt == 0 || millis() < expandDueAt) return;
    expandDueAt = 0;
    if (!speakerExpanded) return;            // cancelled while waiting
    if (platform == PLATFORM_MOZART) mozartBeolink(true); else aseBeolink(true);
}

inline void unexpandPlaybackSpeaker() {
    expandDueAt = 0;                         // cancel anything pending
    if (playbackJid.length() == 0) return;
    if (!speakerExpanded) return;
    speakerExpanded = false;
    if (platform == PLATFORM_MOZART) mozartBeolink(false); else aseBeolink(false);
}
