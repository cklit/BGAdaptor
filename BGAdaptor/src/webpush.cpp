#include "webpush.h"
#include <ArduinoJson.h>
#include "peer.h"

static WebsocketsServer uiServer;
static const int MAX_UI_CLIENTS = 4;
static WebsocketsClient uiClients[MAX_UI_CLIENTS];
static bool uiClientIsPeer[MAX_UI_CLIENTS] = {false};
String drivenByPeer;
String drivenByPeerIP;

static String beogramStateJson() {
    JsonDocument doc;
    doc["state"] = beogramStateText;
    doc["track"] = beogramTrack;
    // The deck type rides along so a peer adaptor learns the layout for its
    // Halo page from the same message as the state — on connect and on every
    // change, with no polling and no second request to time out. The browser
    // page ignores the extra key.
    doc["deck"] = deviceType == DEVICE_RECORD ? "record" : deviceType == DEVICE_TAPE ? "tape" : "cd";
    doc["title"] = adaptorName;
    doc["product_name"] = productName;
    doc["playing"] = beogramPlaying;
    doc["driven_by"] = drivenByPeer;
    doc["driven_by_ip"] = drivenByPeerIP;
    // A linked peer's deck rides along, so the page can show its controls
    // live rather than waiting for the five-second status poll. A peer's own
    // browser never sees these keys, because a peer has no peer of its own.
    if (peerConfigured()) {
        doc["peer_online"] = peerOnline;
        doc["peer_playing"] = peerPlaying;
        doc["peer_state"] = peerStateText;
        doc["peer_track"] = peerTrack;
        doc["peer_deck"] = peerDeck == DEVICE_RECORD ? "record" : peerDeck == DEVICE_TAPE ? "tape" : "cd";
        doc["peer_title"] = peerTitle;
    }
    String json;
    serializeJson(doc, json);
    return json;
}

void webpushBegin() {
    uiServer.listen(UI_WS_PORT);
}

void webpushLoop() {
    // Accept new browser connections and greet them with the current state
    if (uiServer.poll()) {
        for (int i = 0; i < MAX_UI_CLIENTS; i++) {
            if (!uiClients[i].available()) {
                uiClients[i] = uiServer.accept();
                uiClientIsPeer[i] = false;
                if (uiClients[i].available()) {
                    // A browser never sends anything on this socket; another
                    // adaptor announces itself, which is the only way this one
                    // learns it is being driven.
                    uiClients[i].onMessage([i](WebsocketsClient&, WebsocketsMessage m) {
                        JsonDocument doc;
                        if (deserializeJson(doc, m.data())) return;
                        if (String(doc["role"] | "") != "peer") return;
                        uiClientIsPeer[i] = true;
                        String name = doc["name"] | "";
                        drivenByPeerIP = doc["ip"] | "";
                        drivenByPeer = name.length() ? name : String("another adaptor");
                        beogramStateDirty = true;
                        Serial.println("Driven by peer: " + drivenByPeer);
                    });
                    uiClients[i].send(beogramStateJson());
                }
                break;
            }
        }
    }
    // Service connected clients, and recompute who is driving us. Doing it
    // from what is still connected means an adaptor that is powered off or
    // unlinked clears the flag on its own, with nothing to time out.
    bool peerStillHere = false;
    for (int i = 0; i < MAX_UI_CLIENTS; i++) {
        if (uiClients[i].available()) {
            uiClients[i].poll();
            if (uiClientIsPeer[i]) peerStillHere = true;
        } else {
            uiClientIsPeer[i] = false;
        }
    }
    if (!peerStillHere && drivenByPeer.length()) {
        Serial.println("Peer disconnected — no longer driven");
        drivenByPeer = "";
        drivenByPeerIP = "";
        beogramStateDirty = true;
    }
    // Push on change, flagged from processBuffer
    if (beogramStateDirty) {
        beogramStateDirty = false;
        broadcastBeogramState();
    }
}

void broadcastBeogramState() {
    String json = beogramStateJson();
    for (int i = 0; i < MAX_UI_CLIENTS; i++) {
        if (uiClients[i].available()) uiClients[i].send(json);
    }
}
