#pragma once
#include "state.h"

// Beoremote Halo: websocket client, configuration JSON, wheel/button
// event handling, and display page/button updates.

// Halo page and button identifiers (UUIDs the Halo config declares).
extern const char* const HALO_PAGE_ID;
extern const char* const HALO_BTN_PREV;
extern const char* const HALO_BTN_PLAY;
extern const char* const HALO_BTN_STOP;    // record player mode only
extern const char* const HALO_BTN_NEXT;
extern const char* const HALO_BTN_STANDBY;

struct ButtonUpdate {
    String id;
    bool pending;
    unsigned long timestamp;
};
extern ButtonUpdate pendingUpdate;  // Track pending button updates

// Icon shown on a Play button in icon mode: turntable artwork for a record
// deck, the generic music icon for CD and tape.
const char* playIconFor(DeviceType dt);

// Action label for a Play button in icon mode: STOP while a CD is playing,
// PLAY otherwise.
const char* playActionTitle(DeviceType dt, bool playing);

void sendButtonIconUpdate(const char* buttonID, const char* icon, const char* title = nullptr, const char* subtitle = nullptr);
void sendButtonUpdate(const char* buttonID, const char* state = nullptr, const char* title = nullptr, const char* text = nullptr, const char* subtitle = nullptr, int value = -1);
void sendPageUpdate(const char* pageID, const char* buttonID);
void sendConfigToHalo();
void onMessageCallback(WebsocketsMessage message);
void secondButtonUpdate();
void connectToHalo();

// Apply a change to the page layout — a new page, a different button set, or
// a new page title. Resending the configuration on the live socket does not
// reliably retitle a page, so the socket is dropped and reconnected instead;
// the reconnect handler sends a fresh configuration, which the Halo applies.
void reconnectHalo(const char* reason);

// Draw the volume ring on the Play button of every page, so both stay in
// step with the product's volume rather than only the page in front.
void setVolumeRing(int percent);
void activateHaloPage();

// Reflect playback state on the Halo. Handles both layouts: the CD
// toggle button and the record player's separate Play/Stop pair.
void updateHaloPlayback(bool playing, const char* subtitle = nullptr);
void updateHaloSubtitle(const char* subtitle);
void updateHaloVolume(int level, int minimum, int maximum);
void resetHaloVolumeWhenProductDisconnected();
