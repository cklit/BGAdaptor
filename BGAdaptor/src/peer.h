#pragma once
#include "state.h"

// Peer BGAdaptor: a second adaptor whose deck gets its own Halo page.
//
// Control is one-way by design. This adaptor owns the Halo connection and
// POSTs to the peer's existing /command/* routes — the same ones the web UI
// and Home Assistant already use — so the peer needs no knowledge that it is
// being driven remotely, and no firmware change beyond mDNS advertising.
//
// State flows back the other way over the peer's UI websocket (UI_WS_PORT),
// which it already broadcasts on every Beogram state change and sends once
// on connect. That is what lets page 2 show a real Playing/Stopped title
// instead of guessing.
//
// Nothing here ever sends inline from the Halo callback: commands are queued
// and drained from peerLoop(), so an unreachable peer can never stall the
// Halo websocket or the web UI.

// Halo page 2 identifiers. Distinct UUIDs from page 1 — the Halo keys button
// events by id alone, so a shared id would be ambiguous between pages.
extern const char* const HALO_PAGE2_ID;
extern const char* const HALO_P2_PREV;
extern const char* const HALO_P2_PLAY;
extern const char* const HALO_P2_STOP;
extern const char* const HALO_P2_NEXT;
extern const char* const HALO_P2_STANDBY;

// ── Config (persisted in Preferences) ───────────────────────────────
extern String peerIP;
extern String peerName;
extern String peerId;        // peer's MAC suffix — its identity across DHCP changes
extern DeviceType peerDeck;  // cached, so page 2 still renders while the peer is offline
extern String peerTitle;     // the peer's own Halo page title ("" = derive from its deck)
extern bool peerAutoStop;    // newest interaction wins: starting one deck stops the other

// ── Live state, fed by the peer's state websocket ───────────────────
extern bool peerOnline;
extern bool peerPlaying;
extern String peerStateText;
extern String peerTrack;

inline bool peerConfigured() { return peerIP.length() > 0; }

void peerLoadPrefs();
void peerSetLink(const String& ip, const String& name, const String& id);
void peerSendCommand(const char* command);   // queued, never sent inline
bool peerFetchDeckType();                    // GET /status, caches device_type
void peerLoop();                             // call every loop() iteration

bool peerIsButton(const String& id);
void peerHandleButton(const String& id);
void peerPushHaloState();
void peerMarkHaloDirty();                    // re-push page 2 when the Halo wakes
