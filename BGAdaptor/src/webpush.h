#pragma once
#include "state.h"

// Live state push to the web UI. The embedded page opens a websocket to
// this server (port UI_WS_PORT) and receives a small JSON message every
// time the Beogram reports a state or track change over serial — no
// polling delay. The /status poll remains as initial fill and fallback.

void webpushBegin();
void webpushLoop();
void broadcastBeogramState();

// Name of the adaptor currently driving this one, or "" when none is. A peer
// identifies itself on connect; the value clears when that socket drops, so
// it is live rather than a stored setting. Browser clients never set it.
extern String drivenByPeer;
