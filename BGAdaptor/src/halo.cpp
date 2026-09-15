#include "halo.h"
#include "beogram.h"
#include "transport.h"
#include "peer.h"
#include <ArduinoJson.h>

const char* const HALO_PAGE_ID    = "67461a06-74b6-4114-a808-ab90e8abc03f";
const char* const HALO_BTN_PREV    = "032ed0e4-c61f-4d22-af95-740741217d55";
const char* const HALO_BTN_PLAY    = "872b4893-bfdf-4d51-bb53-b5738149fc61";
const char* const HALO_BTN_STOP    = "5c1f9a7e-2b64-4de3-9f10-8a7c3d6e41b2";
const char* const HALO_BTN_NEXT    = "03481fcc-e2cc-47ba-bcae-6152bbf93692";
const char* const HALO_BTN_STANDBY = "03481fcc-e2cc-47ba-bcae-6152bbf93482";

ButtonUpdate pendingUpdate = {"", false, 0}; // Track pending button updates
static bool haloAwake = true;

void sendButtonUpdate(const char* buttonID, const char* state, const char* title, const char* text, const char* subtitle, int value) {
    if (!haloAwake) return;
    JsonDocument doc;
    doc["update"]["type"] = "button";
    doc["update"]["id"] = buttonID;
    if (value != -1) doc["update"]["value"] = value;
    if (state != nullptr) doc["update"]["state"] = state;
    if (title != nullptr) doc["update"]["title"] = title;
    if (subtitle != nullptr) doc["update"]["subtitle"] = subtitle;
    if (text != nullptr) doc["update"]["content"]["text"] = text;
    String output;
    serializeJson(doc, output);
    haloClient.send(output);
}

// Same as sendButtonUpdate, but sets the button content to one of Halo's
// built-in icons. content is oneOf {text} | {icon}, so the two are
// mutually exclusive — never send both.
void sendButtonIconUpdate(const char* buttonID, const char* icon, const char* title, const char* subtitle) {
    if (!haloAwake) return;
    JsonDocument doc;
    doc["update"]["type"] = "button";
    doc["update"]["id"] = buttonID;
    if (title != nullptr) doc["update"]["title"] = title;
    if (subtitle != nullptr) doc["update"]["subtitle"] = subtitle;
    doc["update"]["content"]["icon"] = icon;
    String output;
    serializeJson(doc, output);
    haloClient.send(output);
}

void sendPageUpdate(const char* pageID, const char* buttonID) {
    if (!haloAwake) {
        Serial.println("displaypage skipped — Halo is asleep");
        return;
    }
    JsonDocument doc;
    doc["update"]["type"] = "displaypage";
    doc["update"]["pageid"] = pageID; 
    doc["update"]["buttonid"] = buttonID;
    String output;
    serializeJson(doc, output);
    haloClient.send(output);
}

// Build one button entry for the Halo configuration, with an icon as
// content instead of a text label.
static String haloIconButton(const char* id, const char* icon, const char* title, const char* subtitle) {
    return String("{") +
        "\"id\": \"" + id + "\"," +
        "\"title\": \"" + title + "\"," +
        "\"subtitle\": \"" + subtitle + "\"," +
        "\"value\": 100," +
        "\"state\": \"inactive\"," +
        "\"content\": { \"icon\": \"" + icon + "\" }" +
    "}";
}

// Build one button entry for the Halo configuration.
static String haloButton(const char* id, const char* label) {
    return String("{") +
        "\"id\": \"" + id + "\"," +
        "\"title\": \"\"," +
        "\"subtitle\": \"\"," +
        "\"value\": 100," +
        "\"state\": \"inactive\"," +
        "\"content\": { \"text\": \"" + label + "\" }" +
    "}";
}

static const char* deckTitle(DeviceType dt) {
    return (dt == DEVICE_TAPE)   ? "Beocord"
         : (dt == DEVICE_RECORD) ? "Beogram"
                                 : "Beogram CD";
}

// A page is titled after its deck unless that adaptor has been given a name
// of its own. Two decks of the same type would otherwise produce two
// identically titled pages, and the title is the only label a Halo page has.
// The name is set per adaptor on its own Type card, not here — the secondary
// never connects to a Halo, so it could not be set from a Halo setting.
static const char* pageTitle(const String& custom, DeviceType dt) {
    return custom.length() ? custom.c_str() : deckTitle(dt);
}

// Build one page of the Halo configuration. Page 1 (the local deck) and
// page 2 (a peer adaptor's deck) differ only in their ids and title, so both
// are assembled here.
//
// Record players get a dedicated Stop button; CD players use a single button
// that toggles, since their reported state is trustworthy. A turntable never
// reports a lifted tonearm and a tape deck's stop is a distinct action —
// both get a dedicated second button.
//
// Turntable only: the Play button can show Halo's turntable icon, with the
// action in the title and the playback state in the subtitle.
// Icon shown on the Play button when icon mode is on. A turntable gets the
// dedicated artwork; a CD or tape deck has no equivalent, so both use the
// generic music icon.
// In icon mode the title carries the action, since the artwork cannot. A CD
// has one button that toggles, so the title has to follow the state; a record
// or tape deck has a separate Stop button and always offers Play.
const char* playActionTitle(DeviceType dt, bool playing) {
    return (dt == DEVICE_CD && playing) ? "STOP" : "PLAY";
}

const char* playIconFor(DeviceType dt) {
    return (dt == DEVICE_RECORD) ? "turntable" : "music";
}

static String haloPageJson(const char* pageId, const char* title, DeviceType dt,
                           bool playIcon,
                           const char* bPrev, const char* bPlay, const char* bStop,
                           const char* bNext, const char* bStandby) {
    const char* prevLabel = (dt == DEVICE_RECORD) ? "<"
                          : (dt == DEVICE_TAPE)   ? "<<" : "I<";
    const char* nextLabel = (dt == DEVICE_RECORD) ? ">"
                          : (dt == DEVICE_TAPE)   ? ">>" : ">I";
    bool icon = playIcon;

    String buttons = haloButton(bPrev, prevLabel) + "," +
                     (icon ? haloIconButton(bPlay, playIconFor(dt), "PLAY", "Stopped")
                           : haloButton(bPlay, "Play")) + ",";
    if (dt != DEVICE_CD) buttons += haloButton(bStop, "II") + ",";
    buttons += haloButton(bNext, nextLabel);
    if (dt == DEVICE_RECORD) {
        buttons += "," + (icon
            ? haloIconButton(bStandby, "sleep", "", "")
            : haloButton(bStandby, "Stby"));
    }

    return String("{") +
        "\"title\": \"" + title + "\"," +
        "\"id\": \"" + pageId + "\"," +
        "\"buttons\": [" + buttons + "]" +
    "}";
}

// The Halo decides whether a configuration is new from the version string.
// Deriving it from the configuration's own content means any change — a page
// added, a button set changed, a page retitled — produces a different
// version, while an unchanged configuration keeps the same one across
// reconnects. Versioning by page count alone silently ignored renames.
static uint32_t configHash(const String& s) {
    uint32_t h = 2166136261u;                 // FNV-1a
    for (size_t i = 0; i < s.length(); i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

void sendConfigToHalo() {
    String pages = haloPageJson(HALO_PAGE_ID, pageTitle(adaptorName, deviceType), deviceType,
                                haloPlayIcon,
                                HALO_BTN_PREV, HALO_BTN_PLAY, HALO_BTN_STOP,
                                HALO_BTN_NEXT, HALO_BTN_STANDBY);

    // A linked peer adaptor gets a second page. It is built from the cached
    // deck type, so the page still appears while the peer is unreachable —
    // its buttons just report Offline until it comes back. The title follows
    // the deck the same way page 1 does, rather than the peer's own name.
    if (peerConfigured()) {
        pages += "," + haloPageJson(HALO_PAGE2_ID, pageTitle(peerTitle, peerDeck),
                                    peerDeck, haloPlayIcon,
                                    HALO_P2_PREV, HALO_P2_PLAY, HALO_P2_STOP,
                                    HALO_P2_NEXT, HALO_P2_STANDBY);
    }

    String version = String("1.") + (peerConfigured() ? "1." : "0.") +
                     String(configHash(pages) % 100000UL);

    String jsonMessage = String("{") +
        "\"configuration\": {" +
            "\"version\": \"" + version + "\"," +
            "\"id\": \"ae32d6dd-3300-4725-a6a0-2df6b5f8326f\"," +
            "\"pages\": [" + pages + "]" +
        "}" +
    "}";

    haloClient.send(jsonMessage);
    Serial.println("📡 Sent configuration to Halo, version " + version);
    if (debugSerial) Serial.println(jsonMessage);

    if (platform == PLATFORM_MOZART) {
        // Mozart exposes playback state over REST — poll it to sync the Halo button
        if (productIP.length() > 0) sendHttpRequest("/api/v1/playback/state");
    } else {
        // ASE has no equivalent poll — derive the button state from lineInActive
        haloUpdate = STATE;
    }
    peerPushHaloState();
}

// Reflect playback state on the Halo.
//  CD:            one button, its label toggles between Play and Stop.
//  Record / tape: two buttons with fixed labels; only the status title
//                 changes, because a lifted tonearm or a pressed pause is
//                 never reported and a toggling label would end up lying
//                 about what the button does.
// In icon mode the Play button has one line of text to work with, so state
// and track share it. Only a CD reports a track at all.
static String haloIconSubtitle(const char* state, DeviceType dt, const String& track) {
    String sub = state;
    if (dt == DEVICE_CD && track.length() && track != "-") sub += " - " + track;
    return sub;
}

void updateHaloPlayback(bool playing, const char* subtitle) {
    const char* title = playing ? "Playing" : "Stopped";
    if (haloPlayIcon) {
        // Icon mode: the button shows artwork, so the label moves to the
        // title and state and track go to the subtitle together.
        String sub = haloIconSubtitle(title, deviceType, beogramTrack);
        sendButtonIconUpdate(HALO_BTN_PLAY, playIconFor(deviceType),
                             playActionTitle(deviceType, playing), sub.c_str());
        if (deviceType != DEVICE_CD) sendButtonUpdate(HALO_BTN_STOP, nullptr, "", "II", nullptr);
        return;
    }
    if (deviceType != DEVICE_CD) {
        // Only the Play button carries the status title; the second button
        // keeps an empty one so the state is stated once, not twice.
        sendButtonUpdate(HALO_BTN_PLAY, nullptr, title, "Play", subtitle);
        sendButtonUpdate(HALO_BTN_STOP, nullptr, "", "II",
                 deviceType == DEVICE_TAPE ? subtitle : nullptr);
    } else {
        sendButtonUpdate(HALO_BTN_PLAY, nullptr, title, playing ? "Stop" : "Play", subtitle);
    }
}

void updateHaloSubtitle(const char* subtitle) {
    if (haloPlayIcon) {
        // In icon mode the Play subtitle carries state and track together,
        // so rebuild the whole line rather than overwriting it with the
        // track alone — otherwise the state disappears on a track change.
        updateHaloPlayback(beogramPlaying);
    } else {
        sendButtonUpdate(HALO_BTN_PLAY, nullptr, nullptr, nullptr, subtitle);
    }
    if (deviceType == DEVICE_TAPE) {
        sendButtonUpdate(HALO_BTN_STOP, nullptr, nullptr, nullptr, subtitle);
    }
}

// The ring is drawn on the Play button of every page, so both stay in step
// with the product's actual volume rather than only the page in front.
void setVolumeRing(int percent) {
    sendButtonUpdate(HALO_BTN_PLAY, nullptr, nullptr, nullptr, nullptr, percent);
    if (peerConfigured()) {
        sendButtonUpdate(HALO_P2_PLAY, nullptr, nullptr, nullptr, nullptr, percent);
    }
}

void updateHaloVolume(int level, int minimum, int maximum) {
    if (!haloClient.available()) return;
    if (!haloVolumeControls) {
        setVolumeRing(100);
        return;
    }
    if (maximum <= minimum) return;
    int percent = ((level - minimum) * 100 + (maximum - minimum) / 2) / (maximum - minimum);
    percent = constrain(percent, 0, 100);
    setVolumeRing(percent);
}

void resetHaloVolumeWhenProductDisconnected() {
    static bool resetSent = false;
    if (productConnected()) {
        resetSent = false;
        return;
    }
    if (!haloClient.available() || resetSent) return;
    setVolumeRing(100);
    resetSent = true;
}

void onMessageCallback(WebsocketsMessage message) {
    //Serial.println("Message from Halo: " + message.data());

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message.data());

    if (error) {
        Serial.println("JSON parsing failed");
        return;
    }

    JsonObject event = doc["event"];
    // Page 2's Play button carries the same ring. The wheel adjusts this
    // adaptor's own product either way — that is the one with the speakers,
    // whichever deck is feeding it.
    bool wheelOnPlay = (event["id"] == HALO_BTN_PLAY || event["id"] == HALO_P2_PLAY);
    if (haloControls && haloVolumeControls && event["type"] == "wheel" &&
        wheelOnPlay && event["counts"].is<int>()) {
        int counts = event["counts"].as<int>();
        if (counts == 1 || counts == -1) {
            Serial.println(counts > 0 ? "Halo volume up" : "Halo volume down");
            if (platform == PLATFORM_ASE) {
                aseAdjustVolume(counts);
            } else {
                mozartAdjustVolume(counts);
            }
        }
        return;
    }

    // Handle button press events dynamically
    if (doc["event"].is<JsonObject>() && doc["event"]["type"] == "button" && doc["event"]["state"] == "pressed") {
        String buttonID = doc["event"]["id"].as<String>();
        Serial.print("Halo button pressed: ");

        if (peerIsButton(buttonID)) {
            // Page 2 belongs to a peer adaptor — routed, never handled here.
            Serial.println("PEER");
            peerHandleButton(buttonID);
        } else if (buttonID == HALO_BTN_PLAY) {
            // Turntables and tape decks have their own Stop button, so Play
            // is always Play there.
            if (deviceType != DEVICE_CD || playbackState != PLAYING) {
              Serial.println("PLAY");
              // Newest interaction wins: both decks feed the same speakers.
              if (peerAutoStop && peerPlaying) peerSendCommand("stop");
              sendHexCommand(PLAY);
            } else {
              Serial.println("STOP");
              sendHexCommand(STOP);
            }
        } else if (buttonID == HALO_BTN_STOP) {
            Serial.println("STOP");
            sendHexCommand(STOP);
        } else if (buttonID == HALO_BTN_PREV) {
            Serial.println("PREV");
            sendHexCommand(PREVIOUS);
        } else if (buttonID == HALO_BTN_NEXT) {
            Serial.println("NEXT");
            sendHexCommand(NEXT);
        } else if (buttonID == HALO_BTN_STANDBY && deviceType == DEVICE_RECORD) {
            Serial.println("STBY");
            sendHexCommand(STANDBY);
        } else {
            Serial.println("Unknown Button");
        }

        if (haloClient.available()) {
            sendButtonUpdate(pendingUpdate.id.c_str(), "inactive");
        }
        
        // Schedule second update (active state) after 500ms
        pendingUpdate.id = buttonID;
        pendingUpdate.pending = true;
        pendingUpdate.timestamp = millis();
    }

    if (doc["event"].is<JsonObject>() && doc["event"]["type"] == "system") {
        String systemState = doc["event"]["state"].as<String>();
        if (systemState == "active") {
            haloAwake = true;
            // Button updates are suppressed while asleep, so page 2 would
            // otherwise still show whatever it held before standby.
            peerMarkHaloDirty();
            // A deck playing is what makes the controls worth showing, and
            // that is true whether it is this adaptor's or a peer's. Line-in
            // is no use as the test: a peer's deck plays through the peer's
            // own product, which this adaptor may not even be linked to.
            if (haloControls && (beogramPlaying || (peerConfigured() && peerPlaying))) {
                haloActionTime = millis();  // Store the current time
                haloUpdate = PAGE;
            }
        } else if (systemState == "standby" || systemState == "sleep") {
            haloAwake = false;
            haloUpdate = NONE;
        }
    }
}    

void secondButtonUpdate() {
    if (haloClient.available() && pendingUpdate.pending && millis() - pendingUpdate.timestamp >= haloActionDelay) {
        sendButtonUpdate(pendingUpdate.id.c_str(), "inactive");
        pendingUpdate.pending = false;  // Reset update tracker
    }
}

void connectToHalo() {
    haloClient.poll();    
    if (millis() - haloLastReconnectAttempt > reconnectInterval) {
        haloLastReconnectAttempt = millis(); 
        if (!haloClient.available() && haloIP.length() > 0) {
            Serial.println("🔄 Reconnecting to Halo WebSocket at: " + haloIP);
            if (haloClient.connect(("ws://" + haloIP + ":" + HALO_WEBSOCKET_PORT).c_str())) {
                Serial.println("✅ Reconnected to Beoremote Halo WebSocket!");
                haloClient.onMessage(onMessageCallback);
                if (platform == PLATFORM_MOZART && productIP.length() > 0) {
                    sendHttpRequest("/api/v1/playback/state");
                }
            } else {
                Serial.println("❌ Failed to connect to Halo WebSocket.");
            }
        } 
    }
}

void reconnectHalo(const char* reason) {
    if (haloIP.length() == 0 || !haloClient.available()) return;
    haloClient.close();
    // Retry on the next loop rather than waiting out the reconnect interval.
    haloLastReconnectAttempt = 0;
    Serial.println(String("Halo layout changed (") + reason + ") — reconnecting");
}

void activateHaloPage() {
    if (haloClient.available() && haloUpdate == PAGE && (millis() - haloActionTime >= haloActionDelay)) {
        haloUpdate = NONE;  

        // Open the page for whichever deck is actually playing. The local
        // deck wins when both are, since that is the one this adaptor's own
        // product and volume wheel belong to.
        if (!beogramPlaying && peerConfigured() && peerPlaying) {
            sendPageUpdate(HALO_PAGE2_ID, HALO_P2_PLAY);
            peerPushHaloState();
            return;
        }

        sendPageUpdate(HALO_PAGE_ID, HALO_BTN_PLAY);
        updateHaloPlayback(beogramPlaying);
        if (deviceType == DEVICE_CD && beogramTrack != "-") {
            String subtitle = "Track " + beogramTrack;
            updateHaloSubtitle(subtitle.c_str());
        }
    }

    if (haloClient.available() && haloUpdate == STATE && (millis() - haloActionTime >= haloActionDelay)) {
        haloUpdate = NONE;
        updateHaloPlayback(lineInActive);
    }
}
