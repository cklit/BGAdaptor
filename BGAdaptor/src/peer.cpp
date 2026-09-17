#include "peer.h"
#include "halo.h"
#include "beogram.h"
#include <ArduinoJson.h>

const char* const HALO_PAGE2_ID  = "9d1c4f30-5a22-4e87-bb41-0c6e2f7a9d14";
const char* const HALO_P2_PREV   = "9d1c4f30-5a22-4e87-bb41-0c6e2f7a9d15";
const char* const HALO_P2_PLAY   = "9d1c4f30-5a22-4e87-bb41-0c6e2f7a9d16";
const char* const HALO_P2_STOP   = "9d1c4f30-5a22-4e87-bb41-0c6e2f7a9d17";
const char* const HALO_P2_NEXT   = "9d1c4f30-5a22-4e87-bb41-0c6e2f7a9d18";
const char* const HALO_P2_STANDBY= "9d1c4f30-5a22-4e87-bb41-0c6e2f7a9d19";

String peerIP, peerName, peerId;
DeviceType peerDeck = DEVICE_RECORD;
String peerTitle;
bool peerAutoStop = true;

bool peerOnline = false;
bool peerPlaying = false;
String peerStateText = "Unknown";
String peerTrack = "-";

// ── Internals ───────────────────────────────────────────────────────
static WebsocketsClient peerClient;
static unsigned long peerLastReconnect = 0;
static unsigned long peerReconnectDelay = reconnectInterval;
static bool peerHaloDirty = false;
// The peer only sends on state changes, so an idle Standby can leave this
// socket silent for a long time — long enough for a WiFi AP/NAT to drop it
// without either side seeing a close. available() then keeps reporting true
// forever, so nothing here would ever notice to reconnect. Every other
// long-lived websocket in this codebase (product, Halo) pings on the same
// timeout for the same reason; this one just never got it when it was added.
static unsigned long peerLastPingReceived = 0;

// Command queue. Entries are string literals, so nothing is allocated in the
// Halo callback and nothing can dangle. Four is enough to absorb a burst of
// Next presses; beyond that dropping is the right answer, because a command
// that arrives ten seconds late is worse than one that never arrives.
static const int PEER_QUEUE_LEN = 4;
static const char* peerQueue[PEER_QUEUE_LEN];
static int peerQHead = 0, peerQCount = 0;

// An unreachable peer costs one connect timeout per command. After a failure,
// stop trying for a while and drop what is queued — the same reasoning as the
// product reconnect backoff, for the same reason (a blocking connect() stalls
// the whole loop).
static unsigned long peerLastSendFail = 0;
static const unsigned long PEER_FAIL_BACKOFF_MS = 5000;

// Two different jobs, two different tolerances. A command is fire-and-forget
// and must never hold up the loop. A deck-type read happens once per
// connection and has to survive the peer being briefly busy in its own loop.
static const int PEER_CMD_TIMEOUT_MS = 400;
static const int PEER_QUERY_TIMEOUT_MS = 2000;

static void peerQueueClear() { peerQHead = 0; peerQCount = 0; }

void peerMarkHaloDirty() { peerHaloDirty = true; }

// ── Persistence ─────────────────────────────────────────────────────
void peerLoadPrefs() {
    peerIP   = preferences.getString("peerIP", "");
    peerName = preferences.getString("peerName", "");
    peerId   = preferences.getString("peerId", "");
    String deck = preferences.getString("peerDeck", "record");
    peerDeck = (deck == "cd")   ? DEVICE_CD
             : (deck == "tape") ? DEVICE_TAPE
                                : DEVICE_RECORD;
    peerTitle = preferences.getString("peerTitle", "");
    peerAutoStop = preferences.getBool("peerAutoStop", true);
    if (peerConfigured()) {
        Serial.println("Peer adaptor: " + (peerName.length() ? peerName : peerIP));
    }
}

void peerSetLink(const String& ip, const String& name, const String& id) {
    if (peerClient.available()) peerClient.close();
    peerOnline = false;
    peerPlaying = false;
    peerStateText = "Unknown";
    peerTrack = "-";
    peerQueueClear();

    peerTitle = "";                 // belongs to whichever adaptor was linked
    preferences.putString("peerTitle", "");

    peerIP = ip;
    peerName = name;
    peerId = id;
    preferences.putString("peerIP", peerIP);
    preferences.putString("peerName", peerName);
    preferences.putString("peerId", peerId);

    // Page count and layout both change, so the Halo needs the new config.
    reconnectHalo("peer linked or unlinked");

    Serial.println(peerConfigured() ? "Peer adaptor linked: " + peerIP
                                    : "Peer adaptor cleared");
    peerLastReconnect = 0;          // connect on the next loop, don't wait out the interval
    peerReconnectDelay = reconnectInterval;
}

// ── Deck type ───────────────────────────────────────────────────────
bool peerFetchDeckType() {
    if (!peerConfigured()) return false;

    WiFiClient client;
    HTTPClient req;                 // local instance: the global `http` belongs to transport_moz
    req.setConnectTimeout(PEER_QUERY_TIMEOUT_MS);
    req.setTimeout(PEER_QUERY_TIMEOUT_MS);
    if (!req.begin(client, "http://" + peerIP + "/status")) return false;

    int code = req.GET();
    if (code != 200) {
        req.end();
        Serial.println("Peer /status failed: " + String(code));
        return false;
    }
    String body = req.getString();
    req.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    String deck = doc["device_type"] | "";
    if (deck != "cd" && deck != "record" && deck != "tape") return false;

    String previousTitle = peerTitle;
    DeviceType previous = peerDeck;
    peerDeck = (deck == "cd") ? DEVICE_CD : (deck == "tape") ? DEVICE_TAPE : DEVICE_RECORD;
    preferences.putString("peerDeck", deck);

    String title = doc["adaptor_name"] | "";
    if (title != peerTitle) {
        peerTitle = title;
        preferences.putString("peerTitle", peerTitle);
    }

    if (peerName.length() == 0) {
        String n = doc["product_name"] | "";
        if (n.length()) { peerName = n; preferences.putString("peerName", peerName); }
    }

    // A different deck means a different button set on page 2; a different
    // title means a different page heading. Either needs a fresh config.
    if (previous != peerDeck || previousTitle != peerTitle) {
        reconnectHalo("peer deck or name read on connect");
    }
    return true;
}

// ── Outgoing commands ───────────────────────────────────────────────
void peerSendCommand(const char* command) {
    if (!peerConfigured()) return;
    if (peerQCount >= PEER_QUEUE_LEN) {
        Serial.println("Peer command queue full, dropping " + String(command));
        return;
    }
    peerQueue[(peerQHead + peerQCount) % PEER_QUEUE_LEN] = command;
    peerQCount++;
}

static void peerDrainQueue() {
    if (peerQCount == 0) return;
    if (peerLastSendFail && millis() - peerLastSendFail < PEER_FAIL_BACKOFF_MS) {
        peerQueueClear();           // peer is down; stale commands are worse than none
        return;
    }

    const char* command = peerQueue[peerQHead];
    peerQHead = (peerQHead + 1) % PEER_QUEUE_LEN;
    peerQCount--;

    WiFiClient client;
    HTTPClient req;
    req.setConnectTimeout(PEER_CMD_TIMEOUT_MS);
    req.setTimeout(PEER_CMD_TIMEOUT_MS);
    if (!req.begin(client, "http://" + peerIP + "/command/" + command)) {
        peerLastSendFail = millis();
        return;
    }
    int code = req.POST("");
    req.end();

    if (code == 200) {
        peerLastSendFail = 0;
        Serial.println("Peer ← " + String(command));
    } else {
        peerLastSendFail = millis();
        Serial.println("Peer command failed (" + String(code) + "): " + String(command));
    }
}

// ── Incoming state ──────────────────────────────────────────────────
static void peerOnMessage(WebsocketsMessage message) {
    JsonDocument doc;
    if (deserializeJson(doc, message.data())) return;

    bool playing = doc["playing"] | false;
    String state = doc["state"] | "";
    String track = doc["track"] | "";

    // Firmware from this version on reports its deck type with every state
    // message, so a deck changed on the peer is picked up live. An older peer
    // omits the key and falls back to the /status read done on connect.
    bool layoutChanged = false;

    String deck = doc["deck"] | "";
    if (deck == "cd" || deck == "record" || deck == "tape") {
        DeviceType reported = (deck == "cd") ? DEVICE_CD
                            : (deck == "tape") ? DEVICE_TAPE : DEVICE_RECORD;
        if (reported != peerDeck) {
            peerDeck = reported;
            preferences.putString("peerDeck", deck);
            Serial.println("Peer deck type is now " + deck);
            layoutChanged = true;       // different button set on page 2
        }
    }

    // The peer names its own page, so renaming it there is enough — there is
    // no second place to keep in step.
    if (doc["title"].is<const char*>()) {
        String title = doc["title"].as<String>();
        if (title != peerTitle) {
            Serial.println("Peer name is now '" + title + "' (was '" + peerTitle + "')");
            peerTitle = title;
            preferences.putString("peerTitle", peerTitle);
            layoutChanged = true;       // different page heading
        }
    }

    if (peerName.length() == 0 && doc["product_name"].is<const char*>()) {
        String name = doc["product_name"].as<String>();
        if (name.length() > 0) {
            peerName = name;
            preferences.putString("peerName", peerName);
        }
    }

    // Same path as a local rename: a resend would not retitle page 2, which
    // is why renaming the peer appeared to do nothing until the next drop.
    if (layoutChanged) reconnectHalo("peer renamed or changed deck");

    bool changed = (playing != peerPlaying) || (state != peerStateText) || (track != peerTrack);
    peerPlaying = playing;
    if (state.length()) peerStateText = state;
    if (track.length()) peerTrack = track;
    if (changed) {
        peerHaloDirty = true;
        beogramStateDirty = true;   // the web page shows the peer's deck too
    }
}

// ── Halo page 2 ─────────────────────────────────────────────────────
bool peerIsButton(const String& id) {
    return id == HALO_P2_PLAY || id == HALO_P2_STOP || id == HALO_P2_PREV ||
           id == HALO_P2_NEXT || id == HALO_P2_STANDBY;
}

void peerHandleButton(const String& id) {
    if (id == HALO_P2_PLAY) {
        // Record and tape decks have their own Stop button, so Play is always
        // Play there. A CD toggles against the last state the peer reported —
        // which is trustworthy for exactly the same reason it is locally.
        if (peerDeck != DEVICE_CD || !peerPlaying) {
            // Newest interaction wins: both decks feed the same speakers, and
            // this adaptor is the only place that knows about both.
            if (peerAutoStop && beogramPlaying) sendHexCommand(STOP);
            peerSendCommand("play");
        } else {
            peerSendCommand("stop");
        }
    } else if (id == HALO_P2_STOP) {
        peerSendCommand("stop");
    } else if (id == HALO_P2_PREV) {
        peerSendCommand("prev");
    } else if (id == HALO_P2_NEXT) {
        peerSendCommand("next");
    } else if (id == HALO_P2_STANDBY && peerDeck == DEVICE_RECORD) {
        peerSendCommand("standby");
    }
}

void peerPushHaloState() {
    if (!peerConfigured() || !haloClient.available()) return;

    // With no state there is nothing honest to show, so say so rather than
    // rendering a Stopped title that may be a lie.
    const char* title = !peerOnline ? "Offline" : (peerPlaying ? "Playing" : "Stopped");

    // Text mode has no separate state field for the track, so the subtitle
    // must be resent on every change including a clear — an omitted field
    // (nullptr) leaves the Halo showing whatever it had before, which is how
    // a track used to stick around after the peer's deck went to standby. A
    // lone space is what actually blanks it; an empty string is ignored.
    String sub;
    const char* subtitle = nullptr;
    if (peerDeck == DEVICE_CD) {
        sub = (peerOnline && peerTrack.length() && peerTrack != "-") ? ("Track " + peerTrack) : " ";
        subtitle = sub.c_str();
    }

    if (haloPlayIcon) {
        // Icon mode: the artwork stays put, so the state moves to the
        // subtitle — same arrangement as page 1. Sending text content here
        // would replace the icon with a label.
        String iconSub = title;
        if (peerOnline && peerDeck == DEVICE_CD && peerTrack.length() && peerTrack != "-") {
            iconSub += " - " + peerTrack;
        }
        sendButtonIconUpdate(HALO_P2_PLAY, playIconFor(peerDeck),
                             playActionTitle(peerDeck, peerPlaying), iconSub.c_str());
        if (peerDeck != DEVICE_CD) sendButtonUpdate(HALO_P2_STOP, nullptr, "", "II", nullptr);
        return;
    }

    if (peerDeck != DEVICE_CD) {
        sendButtonUpdate(HALO_P2_PLAY, nullptr, title, "Play", subtitle);
        sendButtonUpdate(HALO_P2_STOP, nullptr, "", "II", nullptr);
    } else {
        sendButtonUpdate(HALO_P2_PLAY, nullptr, title, peerPlaying ? "Stop" : "Play", subtitle);
    }
}

// ── Loop ────────────────────────────────────────────────────────────
void peerLoop() {
    static String announcedName;
    if (!peerConfigured()) return;

    // The peer link exists only to put that deck on this adaptor's Halo. With
    // no Halo linked there is nothing to drive, so let go of the peer rather
    // than holding its socket open — otherwise unlinking the Halo here leaves
    // the secondary locked out of its own Halo settings, with nothing on
    // either screen explaining why. The link itself is kept, so relinking a
    // Halo brings the page back without reconfiguring anything.
    if (haloIP.length() == 0) {
        if (peerClient.available()) {
            peerClient.close();
            Serial.println("No Halo linked — releasing peer");
        }
        peerOnline = false;
        peerPlaying = false;
        peerStateText = "Unknown";
        peerLastReconnect = 0;                  // reconnect at once when a Halo returns
        peerReconnectDelay = reconnectInterval;
        return;
    }

    peerClient.poll();

    bool up = peerClient.available();
    if (up != peerOnline) {
        peerOnline = up;
        peerHaloDirty = true;
        beogramStateDirty = true;
        if (!up) {
            // Keep the last known playing flag out of the UI: we no longer know.
            peerStateText = "Unknown";
        }
    }

    // available() only reflects what this side's socket thinks, and a
    // connection an AP quietly dropped while idle can look open forever.
    // Ping on the same schedule as the other long-lived sockets so a dead
    // link gets noticed (ping/pong or send failing closes it) instead of
    // silently sitting there.
    if (up) {
        if (millis() - peerLastPingReceived >= pingTimeout) {
            peerClient.ping();
            peerLastPingReceived = millis();
        }
    } else {
        peerLastPingReceived = millis();
    }

    if (!up && millis() - peerLastReconnect > peerReconnectDelay) {
        peerLastReconnect = millis();
        if (peerClient.connect(("ws://" + peerIP + ":" + UI_WS_PORT).c_str())) {
            peerClient.onMessage(peerOnMessage);
            peerClient.onEvent([](WebsocketsEvent event, String) {
                if (event == WebsocketsEvent::ConnectionOpened ||
                    event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
                    peerLastPingReceived = millis();
                }
            });
            JsonDocument announcement;
            announcement["role"] = "peer";
            announcement["name"] = adaptorName;
            announcement["ip"] = WiFi.localIP().toString();
            String announcementJson;
            serializeJson(announcement, announcementJson);
            peerClient.send(announcementJson);
            announcedName = adaptorName;
            Serial.println("Peer state websocket connected");
            peerReconnectDelay = reconnectInterval;
            peerOnline = true;
            peerLastPingReceived = millis();
            peerHaloDirty = true;
        } else {
            peerReconnectDelay = min(peerReconnectDelay * 2, reconnectMaxInterval);
        }
    }

    if (peerClient.available() && announcedName != adaptorName) {
        JsonDocument announcement;
        announcement["role"] = "peer";
        announcement["name"] = adaptorName;
        announcement["ip"] = WiFi.localIP().toString();
        String announcementJson;
        serializeJson(announcement, announcementJson);
        peerClient.send(announcementJson);
        announcedName = adaptorName;
    }

    peerDrainQueue();

    if (peerHaloDirty && haloClient.available()) {
        peerHaloDirty = false;
        peerPushHaloState();
    }
}
