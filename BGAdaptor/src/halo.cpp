#include "halo.h"
#include "beogram.h"
#include "transport.h"
#include <ArduinoJson.h>

const char* const HALO_PAGE_ID    = "67461a06-74b6-4114-a808-ab90e8abc03f";
const char* const HALO_BTN_PREV    = "032ed0e4-c61f-4d22-af95-740741217d55";
const char* const HALO_BTN_PLAY    = "872b4893-bfdf-4d51-bb53-b5738149fc61";
const char* const HALO_BTN_STOP    = "5c1f9a7e-2b64-4de3-9f10-8a7c3d6e41b2";
const char* const HALO_BTN_NEXT    = "03481fcc-e2cc-47ba-bcae-6152bbf93692";
const char* const HALO_BTN_STANDBY = "03481fcc-e2cc-47ba-bcae-6152bbf93482";

ButtonUpdate pendingUpdate = {"", false, 0}; // Track pending button updates

void sendButtonUpdate(const char* buttonID, const char* state, const char* title, const char* text, const char* subtitle, int value) {
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

void sendConfigToHalo() {
    // Record players get a dedicated Stop button; CD players use a single
    // button that toggles, since their reported state is trustworthy.
    const char* pageTitle = (deviceType == DEVICE_TAPE)   ? "Beocord"
                          : (deviceType == DEVICE_RECORD) ? "Beogram"
                                                          : "Beogram CD";    
    const char* prevLabel = (deviceType == DEVICE_TAPE) ? "Rew" : "Prev";
    const char* nextLabel = (deviceType == DEVICE_TAPE) ? "Fwd" : "Next";

    // Turntable only: the Play button can show Halo's turntable icon, with
    // the action in the title and the playback state in the subtitle.
    bool playIcon = (deviceType == DEVICE_RECORD && haloPlayIcon);

    String buttons = haloButton(HALO_BTN_PREV, prevLabel) + "," +
                     (playIcon ? haloIconButton(HALO_BTN_PLAY, "turntable", "PLAY", "Stopped")
                               : haloButton(HALO_BTN_PLAY, "Play")) + ",";
    // CD trusts its own reported state, so one toggling button is enough.
    // A turntable never reports a lifted tonearm and a tape deck's stop is
    // a distinct action — both get a dedicated second button.
    if (deviceType == DEVICE_RECORD) buttons += haloButton(HALO_BTN_STOP, "Lift") + ",";
    if (deviceType == DEVICE_TAPE)   buttons += haloButton(HALO_BTN_STOP, "Stop") + ",";
    buttons += haloButton(HALO_BTN_NEXT, nextLabel);

    String jsonMessage = String("{") +
        "\"configuration\": {" +
            "\"version\": \"1.0.1\"," +
            "\"id\": \"ae32d6dd-3300-4725-a6a0-2df6b5f8326f\"," +
            "\"pages\": [" +
                "{" +
                    "\"title\": \"" + String(pageTitle) + "\"," +
                    "\"id\": \"" + HALO_PAGE_ID + "\"," +
                    "\"buttons\": [" + buttons + "]" +
                "}" +
            "]" +
        "}" +
    "}";

    haloClient.send(jsonMessage);
    Serial.println("📡 Sent configuration update to Halo");

    if (platform == PLATFORM_MOZART) {
        // Mozart exposes playback state over REST — poll it to sync the Halo button
        if (productIP.length() > 0) sendHttpRequest("/api/v1/playback/state");
    } else {
        // ASE has no equivalent poll — derive the button state from lineInActive
        haloUpdate = STATE;
    }
}

// Reflect playback state on the Halo.
//  CD:     one button, its label toggles between Play and Stop.
//  Record: two buttons with fixed labels; only the status title changes,
//          because a lifted tonearm is never reported and a toggling
//          label would end up lying about what the button does.
void updateHaloPlayback(bool playing, const char* subtitle) {
    const char* title = playing ? "Playing" : "Stopped";
    if (deviceType == DEVICE_RECORD && haloPlayIcon) {
        // Icon mode: the button shows a turntable, so the label moves to
        // the title and the state is reported in the subtitle instead.
        sendButtonIconUpdate(HALO_BTN_PLAY, "turntable", "PLAY", title);
        sendButtonUpdate(HALO_BTN_STOP, nullptr, "", "Lift", subtitle);
        return;
    }
    if (deviceType != DEVICE_CD) {
        // Only the Play button carries the status title; the second button
        // keeps an empty one so the state is stated once, not twice.
        sendButtonUpdate(HALO_BTN_PLAY, nullptr, title, "Play", subtitle);
        sendButtonUpdate(HALO_BTN_STOP, nullptr, "",
                         (deviceType == DEVICE_TAPE) ? "Stop" : "Lift", subtitle);
    } else {
        sendButtonUpdate(HALO_BTN_PLAY, nullptr, title, playing ? "Stop" : "Play", subtitle);
    }
}

void updateHaloSubtitle(const char* subtitle) {
    // In icon mode the Play subtitle carries the playback state, so leave
    // it alone here (a turntable reports no track info anyway).
    if (!(deviceType == DEVICE_RECORD && haloPlayIcon)) {
        sendButtonUpdate(HALO_BTN_PLAY, nullptr, nullptr, nullptr, subtitle);
    }
    if (deviceType != DEVICE_CD) {
        sendButtonUpdate(HALO_BTN_STOP, nullptr, nullptr, nullptr, subtitle);
    }
}

void updateHaloVolume(int level, int minimum, int maximum) {
    if (!haloClient.available() || maximum <= minimum) return;
    int percent = ((level - minimum) * 100 + (maximum - minimum) / 2) / (maximum - minimum);
    percent = constrain(percent, 0, 100);
    sendButtonUpdate(HALO_BTN_PLAY, nullptr, nullptr, nullptr, nullptr, percent);
}

void resetHaloVolumeWhenProductDisconnected() {
    static bool resetSent = false;
    if (productConnected()) {
        resetSent = false;
        return;
    }
    if (!haloClient.available() || resetSent) return;
    sendButtonUpdate(HALO_BTN_PLAY, nullptr, nullptr, nullptr, nullptr, 100);
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
    if (haloControls && event["type"] == "wheel" &&
        event["id"] == HALO_BTN_PLAY && event["counts"].is<int>()) {
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

        if (buttonID == HALO_BTN_PLAY) {
            // Turntables and tape decks have their own Stop button, so Play
            // is always Play there.
            if (deviceType != DEVICE_CD || playbackState != PLAYING) {
              Serial.println("PLAY");
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
        } else if (buttonID == HALO_BTN_STANDBY) {
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

    if (haloControls && lineInActive && doc["event"].is<JsonObject>() && doc["event"]["type"] == "system" && doc["event"]["state"] == "active") {
        haloActionTime = millis();  // Store the current time
        haloUpdate = PAGE;
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

void activateHaloPage() {
    if (haloClient.available() && haloUpdate == PAGE && (millis() - haloActionTime >= haloActionDelay)) {
        haloUpdate = NONE;  
        sendPageUpdate(HALO_PAGE_ID, HALO_BTN_PLAY);
    }

    if (haloClient.available() && haloUpdate == STATE && (millis() - haloActionTime >= haloActionDelay)) {
        haloUpdate = NONE;
        updateHaloPlayback(lineInActive);
    }
}
